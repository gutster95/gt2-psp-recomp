/* PS1 SPU (sound processor) and CD-XA audio, played through the PSP's audio hardware.
 *
 * Registers 0x1F801C00-0x1F801FFF (nocash psx-spx "SPU"): 24 voices of 4-bit ADPCM from the 512 KB sound RAM with
 * ADSR envelopes, pitch (incl. pitch modulation), noise and volumes; key on/off, ENDX; sound RAM transfers by DMA4
 * or the data FIFO. Not emulated: reverb (the output is dry), volume sweeps (treated as fixed), SPU interrupts.
 * CD-XA: XA-ADPCM sectors (GT2's music in MUSIC.DAT) are decoded by cdrom.c into a ring buffer and mixed in.
 *
 * A PSP thread mixes 1024-sample blocks at 44.1 kHz (the SPU's own rate) and outputs them blocking. The game writes
 * registers from the main thread; key on/off are latched and applied by the mixer at block start, everything else
 * is read directly (a register change takes effect within one block, ~23 ms). */
#include <string.h>
#include <pspkernel.h>
#include <pspaudio.h>
#include <stdio.h>
#include "rt.h"
#include "spu.h"

#define RAM_SIZE (512 * 1024)
#define BLOCK 1024

static uint8_t g_sram[RAM_SIZE] __attribute__((aligned(64)));
static uint16_t g_reg[0x200];              /* 0x1C00-0x1FFF as halfwords */
#define REG(off) g_reg[((off) - 0x1C00) >> 1]
static uint32_t g_xfer;                    /* transfer address (bytes) */
static volatile uint32_t g_kon, g_koff;    /* latched key on/off bits */
static volatile uint32_t g_endx;

typedef struct {
    uint32_t addr, repeat, counter;
    int pos;
    int16_t buf[28];
    int32_t s1, s2;
    int16_t prev, cur;
    int phase;                             /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    int32_t env;
    int32_t env_wait;
    int16_t out;
} Voice;
static Voice g_v[24];
static int32_t g_noise_level = 1, g_noise_timer;

static const int32_t FILT[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

/* ---- XA ring buffer (stereo, decoder rate) ------------------------------------------------------------------ */
#define XA_RING 65536
static int16_t g_xa[XA_RING][2];
static volatile uint32_t g_xa_w, g_xa_r;    /* write/read positions (samples) */
static uint32_t g_xa_frac, g_xa_step;       /* 16.16 resampling step to 44.1 kHz */

/* ---- voices -------------------------------------------------------------------------------------------------- */

/* Decoded-block cache: looping samples (engines at up to 4x pitch) decode the same blocks with the same decoder
 * history over and over. Key = (block address, history); entries are tagged with the sound RAM generation, which
 * every write to sound RAM bumps. */
#define DCACHE 2048
typedef struct { uint32_t addr, gen; int32_t s1, s2, e1, e2; int16_t pcm[28]; } DecEntry;
static DecEntry g_dcache[DCACHE];
static volatile uint32_t g_sram_gen = 1;

static void decode_block(Voice *v)
{
    uint32_t a = v->addr & (RAM_SIZE - 1) & ~15u;
    uint8_t h = g_sram[a], flags = g_sram[a + 1];
    if (flags & 4) { v->repeat = a; }
    uint32_t key = (a >> 4) * 2654435761u ^ (uint32_t)v->s1 * 40503u ^ (uint32_t)v->s2 * 9973u;
    DecEntry *e = &g_dcache[(key >> 16) & (DCACHE - 1)];
    if (e->gen == g_sram_gen && e->addr == a && e->s1 == v->s1 && e->s2 == v->s2) {
        memcpy(v->buf, e->pcm, sizeof e->pcm);
        v->s1 = e->e1;
        v->s2 = e->e2;
        return;
    }
    e->addr = a;
    e->s1 = v->s1;
    e->s2 = v->s2;
    int shift = h & 15, f = (h >> 4) & 7;
    if (shift > 12) shift = 9;
    if (f > 4) f = 4;
    for (int i = 0; i < 28; i++) {
        int n = (g_sram[a + 2 + (i >> 1)] >> ((i & 1) * 4)) & 15;
        int32_t s = ((int32_t)(int16_t)(n << 12)) >> shift;
        s += (v->s1 * FILT[f][0] + v->s2 * FILT[f][1] + 32) >> 6;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        v->s2 = v->s1;
        v->s1 = s;
        v->buf[i] = (int16_t)s;
    }
    memcpy(e->pcm, v->buf, sizeof e->pcm);
    e->e1 = v->s1;
    e->e2 = v->s2;
    e->gen = g_sram_gen;
}

static void block_end(Voice *v, int i)
{
    uint32_t a = v->addr & (RAM_SIZE - 1) & ~15u;
    uint8_t flags = g_sram[a + 1];
    if (flags & 1) {                       /* loop end */
        g_endx |= 1u << i;
        v->addr = v->repeat;
        if (!(flags & 2)) { v->phase = 4; v->env = 0; }   /* no repeat: release, muted */
    } else {
        v->addr = a + 16;
    }
    decode_block(v);
}

static void key_on(int i)
{
    Voice *v = &g_v[i];
    uint32_t base = 0x1C00 + i * 16;
    v->addr = (uint32_t)REG(base + 6) * 8;
    v->repeat = (uint32_t)REG(base + 14) * 8;
    v->counter = 0;
    v->pos = 0;
    v->s1 = v->s2 = 0;
    v->prev = v->cur = 0;
    v->phase = 1;
    v->env = 0;
    v->env_wait = 0;
    g_endx &= ~(1u << i);
    decode_block(v);
}

/* one ADSR step (psx-spx "SPU ADSR"): mode 1 = exponential, step = +7..+4 or -8..-5 */
static void env_step(Voice *v, int exp_mode, int decrease, int shift, int step)
{
    if (v->env_wait > 0) { v->env_wait--; return; }
    int32_t cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t st = step << (shift < 11 ? 11 - shift : 0);
    if (exp_mode && !decrease && v->env > 0x6000) cycles *= 4;
    if (exp_mode && decrease) st = st * v->env / 0x8000;
    v->env += st;
    if (v->env > 0x7FFF) v->env = 0x7FFF;
    if (v->env < 0) v->env = 0;
    v->env_wait = cycles - 1;
}

static void envelope(Voice *v, int i)
{
    uint32_t base = 0x1C00 + i * 16;
    uint16_t lo = REG(base + 8), hi = REG(base + 10);
    switch (v->phase) {
    case 1:
        env_step(v, lo >> 15, 0, (lo >> 10) & 31, 7 - ((lo >> 8) & 3));
        if (v->env >= 0x7FFF) { v->phase = 2; v->env_wait = 0; }
        break;
    case 2: {
        env_step(v, 1, 1, (lo >> 4) & 15, -8);
        int32_t sl = ((lo & 15) + 1) * 0x800;
        if (v->env <= sl) { v->phase = 3; v->env_wait = 0; }
        break;
    }
    case 3: {
        int dec = (hi >> 14) & 1, stp = (hi >> 6) & 3;
        env_step(v, hi >> 15, dec, (hi >> 8) & 31, dec ? -8 + stp : 7 - stp);
        break;
    }
    case 4:
        env_step(v, (hi >> 5) & 1, 1, hi & 31, -8);
        if (v->env <= 0) { v->phase = 0; v->env = 0; }
        break;
    }
}

static inline int32_t volume(uint16_t r)
{
    if (r & 0x8000) return 0x3FFF;          /* sweep mode: not emulated, use half volume */
    return (int32_t)(int16_t)(r << 1);
}

/* ---- mixer --------------------------------------------------------------------------------------------------- */

/* Block mixer: voice by voice over the whole block (per-voice constants hoisted), accumulated in g_acc. */
static int32_t g_acc[BLOCK * 2];
static uint64_t g_t_voices;   /* debug: time in the voice loop */
static int16_t g_vout[2][BLOCK];           /* output of the previous/current voice (pitch modulation) */
static int16_t g_noise[BLOCK];

/* SPU voices, mixed at 22.05 kHz (every output sample = 2 SPU ticks); the PSP's hardware sample rate converter
 * (SRC channel) brings it to 44.1 kHz at no CPU cost. Halves the per-sample work - the voice loop is the expensive
 * part of the audio (~15-20 voices, many at high pitch). */
#define SPU_DIV 2

static void mix_spu(int16_t *out, int n)
{
    uint32_t kon = g_kon, koff = g_koff;
    g_kon &= ~kon;
    g_koff &= ~koff;
    for (int i = 0; i < 24; i++) {
        if (kon & (1u << i)) key_on(i);
        if ((koff & (1u << i)) && g_v[i].phase) { g_v[i].phase = 4; g_v[i].env_wait = 0; }
    }
    uint16_t cnt = REG(0x1DAA);
    int enabled = (cnt & 0x8000) && (cnt & 0x4000);
    uint32_t pmon = REG(0x1D90) | ((uint32_t)REG(0x1D92) << 16);
    uint32_t non = REG(0x1D94) | ((uint32_t)REG(0x1D96) << 16);
    memset(g_acc, 0, sizeof(int32_t) * 2 * n);

    if (non) {                                  /* noise generator (psx-spx), only when a voice uses it */
        int noise_shift = (cnt >> 10) & 15, noise_step = ((cnt >> 8) & 3) + 4;
        for (int s = 0; s < n; s++) {
            for (int t = 0; t < SPU_DIV; t++) {
                g_noise_timer -= noise_step;
                if (g_noise_timer < 0) {
                    int parity = ((g_noise_level >> 15) ^ (g_noise_level >> 12) ^ (g_noise_level >> 11) ^
                                  (g_noise_level >> 10) ^ 1) & 1;
                    g_noise_level = ((g_noise_level << 1) | parity) & 0xFFFF;
                    g_noise_timer += 0x20000 >> noise_shift;
                    if (g_noise_timer < 0) g_noise_timer += 0x20000 >> noise_shift;
                }
            }
            g_noise[s] = (int16_t)g_noise_level;
        }
    }

    /* Voices in chunks of 32 samples: the envelope advances once per chunk, so the inner loop is pure arithmetic:
     * interpolate, advance the pitch counter, apply the chunk gain. */
    uint64_t tv0 = now_us();
    int prev_active = 0;
    for (int i = 0; enabled && i < 24; i++) {
        Voice *v = &g_v[i];
        int16_t *vo = g_vout[i & 1], *pv = g_vout[(i & 1) ^ 1];
        if (!v->phase) { v->out = 0; prev_active = 0; continue; }
        uint32_t base = 0x1C00 + i * 16;
        uint32_t step0 = REG(base + 4);
        if (step0 > 0x3FFF) step0 = 0x3FFF;
        int32_t vl = volume(REG(base)), vr = volume(REG(base + 2));
        int pm = i > 0 && (pmon & (1u << i)) && prev_active;
        int nz = (non >> i) & 1;
        int need_out = i < 23 && ((pmon >> (i + 1)) & 1);    /* the next voice is modulated by this one */
        int s = 0;
        while (s < n) {
            int end = s + 32 < n ? s + 32 : n;
            int32_t gl = v->env * vl >> 15, gr = v->env * vr >> 15;
            int32_t env = v->env;
            uint32_t counter = v->counter;
            int16_t prev = v->prev, cur = v->cur;
            if (!nz && !pm && !need_out) {          /* common case: plain voice, no per-sample special cases */
                uint32_t inc = step0 * SPU_DIV;
                int32_t *acc = g_acc + s * 2;
                for (int k = s; k < end; k++, acc += 2) {
                    int32_t smp = prev + (((int32_t)cur - prev) * (int32_t)(counter & 0xFFF) >> 12);
                    counter += inc;
                    while (counter >= 0x1000) {
                        counter -= 0x1000;
                        prev = cur;
                        cur = v->buf[v->pos];
                        if (++v->pos >= 28) { v->pos = 0; block_end(v, i); }
                    }
                    acc[0] += smp * gl >> 15;
                    acc[1] += smp * gr >> 15;
                }
            } else
            for (int k = s; k < end; k++) {
                int32_t smp = nz ? g_noise[k] : prev + (((int32_t)cur - prev) * (int32_t)(counter & 0xFFF) >> 12);
                uint32_t step = step0;
                if (pm) {
                    int32_t m = (int32_t)step * (32768 + pv[k]) >> 15;
                    step = (uint32_t)(m < 0 ? 0 : m > 0x3FFF ? 0x3FFF : m);
                }
                counter += step * SPU_DIV;
                while (counter >= 0x1000) {
                    counter -= 0x1000;
                    prev = cur;
                    cur = v->buf[v->pos];
                    if (++v->pos >= 28) { v->pos = 0; block_end(v, i); }
                }
                if (need_out) vo[k] = (int16_t)(smp * env >> 15);
                g_acc[k * 2] += smp * gl >> 15;
                g_acc[k * 2 + 1] += smp * gr >> 15;
            }
            v->counter = counter;
            v->prev = prev;
            v->cur = cur;
            for (int t = (end - s) * SPU_DIV; t > 0 && v->phase;) {     /* envelope: SPU ticks of this chunk */
                if (v->env_wait >= t) { v->env_wait -= t; break; }
                t -= v->env_wait + 1;
                v->env_wait = 0;
                envelope(v, i);
            }
            if (!v->phase) {                                /* voice ended (release done or loop end) */
                if (need_out) for (int t = end; t < n; t++) vo[t] = 0;
                break;
            }
            s = end;
        }
        v->out = need_out ? vo[n - 1] : 0;
        prev_active = need_out;
    }
    g_t_voices += now_us() - tv0;

    int32_t mvl = volume(REG(0x1D80)), mvr = volume(REG(0x1D82));
    for (int s = 0; s < n; s++) {
        int32_t l = g_acc[s * 2] * mvl >> 15, r = g_acc[s * 2 + 1] * mvr >> 15;
        out[s * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[s * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

/* CD-XA music at 44.1 kHz on its own PSP channel (the PSP mixes channels in hardware). */
static void mix_xa(int16_t *out, int n)
{
    uint16_t cnt = REG(0x1DAA);
    int32_t cdl = (int16_t)REG(0x1DB0), cdr = (int16_t)REG(0x1DB2);
    int on = (cnt & 1) && (cnt & 0x8000) && (cnt & 0x4000);
    /* jitter buffer: start playing with ~2 sectors (~100 ms) buffered, stop when it runs dry (a late sector gives
     * one short pause instead of constant crackling) */
    static int playing;
    if (!playing && g_xa_w - g_xa_r >= 4032) playing = 1;
    for (int s = 0; s < n; s++) {
        int32_t l = 0, r = 0;
        if (playing && g_xa_r == g_xa_w) playing = 0;
        if (playing) {
            int16_t *x = g_xa[g_xa_r & (XA_RING - 1)];
            int16_t *y = (g_xa_r + 1 != g_xa_w) ? g_xa[(g_xa_r + 1) & (XA_RING - 1)] : x;
            int32_t f = (int32_t)(g_xa_frac >> 4);                   /* 12-bit fraction */
            int32_t xl = x[0] + ((y[0] - x[0]) * f >> 12), xr = x[1] + ((y[1] - x[1]) * f >> 12);
            if (on) { l = xl * cdl >> 15; r = xr * cdr >> 15; }
            g_xa_frac += g_xa_step;
            g_xa_r += g_xa_frac >> 16;
            g_xa_frac &= 0xFFFF;
        }
        out[s * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[s * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

#define SPU_BLOCK (BLOCK / SPU_DIV)

static int spu_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static int16_t buf[2][SPU_BLOCK * 2] __attribute__((aligned(64)));
    int r = sceAudioSRCChReserve(SPU_BLOCK, 44100 / SPU_DIV, 2);
    if (r < 0) { logf_("spu: no SRC channel (%08x)\n", (unsigned)r); return 0; }
    for (int k = 0;; k ^= 1) {
        uint64_t t0 = now_us();
        mix_spu(buf[k], SPU_BLOCK);
        {   /* statistics: mixer CPU time per ~5 s and number of active voices */
            static uint64_t acc, next;
            static uint32_t voices, blocks;
            uint64_t t1 = now_us();
            acc += t1 - t0;
            for (int i = 0; i < 24; i++) voices += g_v[i].phase != 0;
            blocks++;
            if (t1 >= next) {
                if (next) logf_("spu: mixer %u%% CPU, %u.%u voices active on average\n",
                                (unsigned)(acc * 100 / 5000000), (unsigned)(voices / blocks),
                                (unsigned)(voices * 10 / blocks % 10));
                acc = 0; voices = 0; blocks = 0; g_t_voices = 0;
                next = t1 + 5000000;
            }
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, buf[k]);
    }
    return 0;
}

static int xa_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static int16_t buf[2][BLOCK * 2] __attribute__((aligned(64)));
    int ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (ch < 0) { logf_("spu: no audio channel (%08x)\n", (unsigned)ch); return 0; }
    /* debug: audiodump.txt next to the EBOOT -> the first 3 minutes of the music output as raw 44.1 kHz audio.raw */
    char path[300];
    snprintf(path, sizeof path, "%saudiodump.txt", g_basedir);
    FILE *dump = fopen(path, "r");
    int dump_blocks = 0;
    if (dump) {
        fclose(dump);
        snprintf(path, sizeof path, "%saudio.raw", g_basedir);
        dump = fopen(path, "wb");
        dump_blocks = 44100 * 180 / BLOCK;
    }
    for (int k = 0;; k ^= 1) {
        mix_xa(buf[k], BLOCK);
        if (dump && dump_blocks > 0) {
            fwrite(buf[k], 4, BLOCK, dump);
            if (--dump_blocks == 0) { fclose(dump); dump = 0; }
        }
        sceAudioOutputBlocking(ch, PSP_AUDIO_VOLUME_MAX, buf[k]);
    }
    return 0;
}

int g_audio = 1;   /* settings.txt audio=0: no sound output (for comparisons) */
int g_autoexit;    /* settings.txt autoexit=N: quit after N seconds */

void spu_init(void)
{
    memset(g_reg, 0, sizeof g_reg);
    g_xa_step = (uint32_t)(0x10000ull * 37800 / 44100);
    if (!g_audio) { logf_("spu: audio off (settings.txt)\n"); return; }
    SceUID t = sceKernelCreateThread("spu", spu_thread, 0x12, 0x8000, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
    t = sceKernelCreateThread("xa", xa_thread, 0x12, 0x8000, PSP_THREAD_ATTR_USER, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

/* ---- registers ----------------------------------------------------------------------------------------------- */

uint32_t spu_read16(uint32_t off)
{
    off &= ~1u;
    if (off >= 0x1C00 && off < 0x1D80 && (off & 15) == 12) {     /* current ADSR volume */
        return (uint32_t)g_v[(off - 0x1C00) >> 4].env;
    }
    switch (off) {
    case 0x1D9C: return g_endx & 0xFFFF;
    case 0x1D9E: return g_endx >> 16;
    case 0x1DAE: return REG(0x1DAA) & 0x3F;                      /* SPUSTAT: mode bits, never busy */
    case 0x1DB8: return REG(0x1D80);
    case 0x1DBA: return REG(0x1D82);
    }
    return REG(off);
}

void spu_write16(uint32_t off, uint32_t v)
{
    off &= ~1u;
    v &= 0xFFFF;
    switch (off) {
    case 0x1D88: g_kon |= v; break;
    case 0x1D8A: g_kon |= (v & 0xFF) << 16; break;
    case 0x1D8C: g_koff |= v; break;
    case 0x1D8E: g_koff |= (v & 0xFF) << 16; break;
    case 0x1DA6: g_xfer = v * 8; break;
    case 0x1DA8:                                                 /* data FIFO */
        g_sram_gen++;
        g_sram[g_xfer & (RAM_SIZE - 1)] = (uint8_t)v;
        g_sram[(g_xfer + 1) & (RAM_SIZE - 1)] = (uint8_t)(v >> 8);
        g_xfer += 2;
        break;
    }
    if (off >= 0x1C00 && off < 0x2000) REG(off) = (uint16_t)v;
}

void spu_dma(uint32_t madr, uint32_t words, int to_spu)
{
    if (to_spu) g_sram_gen++;
    for (uint32_t i = 0; i < words * 4; i++) {
        uint32_t a = (madr + i) & (PSX_RAM_SIZE - 1);
        if (to_spu) g_sram[(g_xfer + i) & (RAM_SIZE - 1)] = g_ram[a];
        else g_ram[a] = g_sram[(g_xfer + i) & (RAM_SIZE - 1)];
    }
    g_xfer += words * 4;
}

/* ---- CD-XA --------------------------------------------------------------------------------------------------- */

static int32_t g_xh[2][2];                 /* decoder history per channel */

static void xa_push(int16_t l, int16_t r)
{
    if (g_xa_w - g_xa_r >= XA_RING - 1) return;     /* full: drop */
    int16_t *x = g_xa[g_xa_w & (XA_RING - 1)];
    x[0] = l;
    x[1] = r;
    g_xa_w++;
}

/* One raw 2352-byte XA-ADPCM sector (psx-spx "CDROM XA Audio ADPCM Compression"). */
void spu_xa_sector(const uint8_t *sec)
{
    static uint32_t count;
    if ((count++ % 200) == 0)
        logf_("spu: XA sector %u (file %u channel %u coding %02x), SPUCNT %04x, CD volume %04x/%04x\n",
              (unsigned)count, sec[16], sec[17], sec[19], REG(0x1DAA), REG(0x1DB0), REG(0x1DB2));
    uint8_t coding = sec[19];
    int stereo = coding & 1, rate = (coding >> 2) & 1, bits8 = (coding >> 4) & 1;
    g_xa_step = (uint32_t)(0x10000ull * (rate ? 18900 : 37800) / 44100);
    const uint8_t *data = sec + 24;
    int units = bits8 ? 4 : 8;
    static int16_t tmp[2][18 * 8 * 28];
    int nl = 0, nr = 0;
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = data + g * 128;
        for (int u = 0; u < units; u++) {
            uint8_t h = grp[4 + u];
            int shift = h & 15, f = (h >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            int32_t *hist = g_xh[ch];
            int16_t *dst = ch ? tmp[1] + nr : tmp[0] + nl;
            for (int j = 0; j < 28; j++) {
                int32_t t;
                if (bits8) t = (int32_t)(int8_t)grp[16 + j * 4 + u] << 8;
                else t = (int32_t)(int16_t)(((grp[16 + j * 4 + (u >> 1)] >> ((u & 1) * 4)) & 15) << 12);
                int32_t s = (t >> shift) + ((hist[0] * FILT[f][0] + hist[1] * FILT[f][1] + 32) >> 6);
                if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
                hist[1] = hist[0];
                hist[0] = s;
                dst[j] = (int16_t)s;
            }
            if (ch) nr += 28; else nl += 28;
        }
    }
    if (stereo) for (int i = 0; i < nl && i < nr; i++) xa_push(tmp[0][i], tmp[1][i]);
    else for (int i = 0; i < nl; i++) xa_push(tmp[0][i], tmp[0][i]);
}
