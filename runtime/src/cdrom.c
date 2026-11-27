/* PS1 CD-ROM controller at register level (nocash psx-spx), backed by the data pack GT2.PAK that the PC tool
 * `python -m gt2psp pack` builds from the user's disc (see src/gt2psp/pack.py for the format). Sectors are served
 * as fast as the game takes them; only XA streaming keeps the real 2x rate.
 *
 * Responses are queued with a delay and delivered one at a time: the next response is only raised after the
 * game acknowledged the previous interrupt. XA-ADPCM audio sectors (music) are filtered out for now. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include "rt.h"
#include "cdrom.h"
#include "spu.h"

#define SECTOR_RAW 2352
#define LEADIN 150                 /* 2 s pregap: LBA 0 = 00:02:00 */

typedef struct { uint32_t lba, count, offset; uint8_t sub[4]; uint16_t stride, reserved; } PakRun;
static SceUID g_pak = -1;
static PakRun *g_runs;
static uint32_t g_nruns;
static uint32_t g_disc_sectors;

/* Read-ahead cache over the pack file: two buffers. Once a read gets past the middle of a buffer, a prefetch thread
 * (own file handle) loads the following 128 KB into the other one. A synchronous 128 KB memory stick read takes
 * ~15 ms - with XA music streaming (150 sectors/s) that stalled the game thread every 0.4 s, a dropped frame at
 * 60 fps. Only a seek (a new file) still reads synchronously. */
#define CACHE_SIZE (128 * 1024)
static uint8_t (*g_cache)[CACHE_SIZE];       /* 2 x CACHE_SIZE, allocated in cdrom_init */
static uint32_t g_cache_off[2] = {0xFFFFFFFFu, 0xFFFFFFFFu}, g_cache_len[2];
static SceUID g_pf_sema = -1, g_pf_fd = -1;
static volatile int g_pf_busy;             /* set by the game thread, cleared by the prefetch thread when done */
static volatile int g_pf_len;
static uint32_t g_pf_off;
static int g_pf_idx, g_pf_pending;         /* game thread: a prefetch was issued and not yet collected */
static uint32_t g_pf_waits, g_pf_sync;     /* statistics */
int g_cd_prefetch = 1;                     /* settings.txt cd_prefetch */
static int pf_thread(SceSize args, void *argp);

static uint8_t g_index;
static uint8_t g_params[16], g_nparams;
static uint8_t g_resp[16], g_nresp, g_resp_pos;
static uint8_t g_int_enable = 0x1F, g_int_flag;
static uint8_t g_stat = 0x02;      /* motor on */
static uint8_t g_mode;
static int g_muted;                /* Mute command: XA audio off */
static uint8_t g_filter_file, g_filter_chan;

static uint32_t g_setloc = LEADIN, g_pos = LEADIN;     /* MSF-absolute sector numbers */
static int g_reading;
static uint64_t g_next_sector_at;
/* Drive timing (settings cd_ack_us, cd_pause_us, cd_seek_us, cd_first_us, cd_period_us). There is no drive: a GT
 * mode load is ~50 small reads of 6 commands each, so the per-command delays set the load time (ACK 600 -> 100 us:
 * 1043 -> 686 ms; with Pause/Seek/first sector shortened 536 ms). Data sectors follow each other as soon as the
 * game has left its CD interrupt handler (see deliver_at), so the spacing defaults to 0. */
int g_cd_pause_us = 300, g_cd_seek_us = 300, g_cd_first_us = 200, g_cd_ack_us = 100;
int g_period_us = 0;
int g_cd_legacy;   /* settings.txt cd_legacy=1: the drive timing of 2026-11-20 (freeze hunt 2026-11-21) */
static uint32_t g_st_sectors;              /* statistics of the current read */
static uint64_t g_st_start;
/* load phases: data sectors with gaps below 1.5 s form one phase (wall time from the first to the last sector) */
static uint64_t g_ld_first, g_ld_last, g_ld_pak_us;
static uint32_t g_ld_sectors;
static uint32_t g_ld_cmd[32];              /* commands during the phase */
static uint64_t g_ld_delay_us;             /* response delays above the ACK (Pause, Seek, ...) */
static void load_span_end(void)
{
    if (g_ld_sectors >= 16) {
        logf_("load commands:");
        for (int i = 0; i < 32; i++) if (g_ld_cmd[i]) logf_(" %02x:%u", i, (unsigned)g_ld_cmd[i]);
        logf_(", response delays %u ms\n", (unsigned)(g_ld_delay_us / 1000));
    }
    memset(g_ld_cmd, 0, sizeof g_ld_cmd); g_ld_delay_us = 0;
    if (g_ld_sectors >= 16)
        logf_("load: %u ms, %u data sectors, memory stick reads %u ms (vblank %u)\n",
              (unsigned)((g_ld_last - g_ld_first) / 1000), (unsigned)g_ld_sectors, (unsigned)(g_ld_pak_us / 1000),
              (unsigned)g_vblank_count);
    g_ld_sectors = 0; g_ld_pak_us = 0; g_ld_first = 0;
}

static uint8_t g_sector[SECTOR_RAW];
static uint8_t g_data[SECTOR_RAW];
static uint32_t g_data_len, g_data_pos;
static uint8_t g_buf[SECTOR_RAW];  /* sector buffer: last sector delivered by the drive */
static uint32_t g_buf_len;
static int g_data_ready;           /* the sector buffer holds a sector not yet loaded into the FIFO (BFRD) */
static int g_consumed = 1;         /* the game has taken (read out or discarded) the last delivered sector */
static uint8_t g_last_hdr[8];

typedef struct { uint64_t at; uint8_t type; uint8_t n; uint8_t b[12]; } Resp;
static Resp g_q[8];
static int g_qn;

static int bcd2i(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static uint8_t i2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

void cdrom_init(void)
{
    char path[320];
    snprintf(path, sizeof path, "%spakpath.txt", g_basedir);
    FILE *pf = fopen(path, "r");          /* tests through PSPLink: read the pack from the memory stick, not USB */
    if (!pf || !fgets(path, sizeof path, pf)) snprintf(path, sizeof path, "%s%s", g_basedir, g_game == GAME_ARC ? "GT2A.PAK" : "GT2.PAK");
    if (pf) fclose(pf);
    path[strcspn(path, "\r\n")] = 0;
    if (g_game == GAME_ARC) {                 /* pakpath.txt names GT2.PAK: the Arcade pack lies next to it */
        size_t n = strlen(path);
        if (n >= 7 && !strcmp(path + n - 7, "GT2.PAK")) snprintf(path + n - 7, sizeof path - (n - 7), "GT2A.PAK");
    }
    if (g_cd_legacy) {   /* 0.75 ms sectors, 600 us ACK, 2 ms Pause/Seek, 1 ms first sector, no interrupt gating */
        g_period_us = 750; g_cd_ack_us = 600; g_cd_pause_us = 2000; g_cd_seek_us = 2000; g_cd_first_us = 1000;
        logf_("cd: legacy timing\n");
    }
    g_cache = memalign(64, 2 * CACHE_SIZE);
    g_pak = sceIoOpen(path, PSP_O_RDONLY, 0);
    struct { char magic[8]; uint32_t nruns, total; } h;
    if (g_pak < 0 || sceIoRead(g_pak, &h, sizeof h) != sizeof h || memcmp(h.magic, "GT2PAK1", 8) || h.nruns > 4096) {
        logf_("no data pack (%s)\n", path);
        if (g_pak >= 0) sceIoClose(g_pak);
        g_pak = -1;
        g_stat = 0x10;             /* shell open */
        return;
    }
    g_nruns = h.nruns;
    g_disc_sectors = h.total;
    g_runs = malloc(sizeof(PakRun) * g_nruns);
    sceIoRead(g_pak, g_runs, sizeof(PakRun) * g_nruns);
    logf_("data pack %s: %u runs, %u sectors\n", path, (unsigned)g_nruns, (unsigned)g_disc_sectors);
    g_pf_fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (g_pf_fd >= 0) {
        g_pf_sema = sceKernelCreateSema("cdpf", 0, 0, 1, 0);
        SceUID t = sceKernelCreateThread("cdpf", pf_thread, 0x14, 0x2000, PSP_THREAD_ATTR_USER, 0);
        if (g_pf_sema < 0 || t < 0 || sceKernelStartThread(t, 0, 0) < 0) g_pf_sema = -1;
    }
}

static int pf_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(g_pf_sema, 1, 0);
        int r = -1;
        if (sceIoLseek32(g_pf_fd, (int)g_pf_off, PSP_SEEK_SET) == (int)g_pf_off)
            r = sceIoRead(g_pf_fd, g_cache[g_pf_idx], CACHE_SIZE);
        g_pf_len = r;
        __sync_synchronize();
        g_pf_busy = 0;
    }
    return 0;
}

static void pf_collect(int wait)
{
    if (!g_pf_pending) return;
    if (g_pf_busy) {
        if (!wait) return;
        g_pf_waits++;
        while (g_pf_busy) sceKernelDelayThread(100);
    }
    __sync_synchronize();
    g_pf_pending = 0;
    if (g_pf_len > 0) { g_cache_off[g_pf_idx] = g_pf_off; g_cache_len[g_pf_idx] = (uint32_t)g_pf_len; }
}

static int pak_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    pf_collect(0);
    while (n) {
        int i = -1;
        for (int k = 0; k < 2; k++)
            if (off >= g_cache_off[k] && off < g_cache_off[k] + g_cache_len[k]) i = k;
        if (i < 0 && g_pf_pending && off >= g_pf_off && off < g_pf_off + CACHE_SIZE) {
            pf_collect(1);                              /* the data is on its way */
            if (g_pf_len <= 0) return 0;
            continue;
        }
        if (i < 0) {                                    /* seek: synchronous read into the buffer not just used */
            pf_collect(1);
            i = g_cache_len[0] == 0 ? 0 : g_cache_len[1] == 0 ? 1 : g_pf_idx;
            g_pf_sync++;
            g_cache_off[i] = 0xFFFFFFFFu; g_cache_len[i] = 0;
            if (sceIoLseek32(g_pak, (int)off, PSP_SEEK_SET) != (int)off) return 0;
            int r = sceIoRead(g_pak, g_cache[i], CACHE_SIZE);
            if (r <= 0) return 0;
            g_cache_off[i] = off;
            g_cache_len[i] = (uint32_t)r;
        }
        uint32_t end = g_cache_off[i] + g_cache_len[i], m = end - off < n ? end - off : n;
        memcpy(dst, g_cache[i] + (off - g_cache_off[i]), m);
        dst += m; off += m; n -= m;
        /* past the middle of a full buffer: prefetch what follows it into the other buffer */
        int o = i ^ 1;
        if (g_cd_prefetch && g_pf_sema >= 0 && !g_pf_pending && g_cache_len[i] == CACHE_SIZE && off - g_cache_off[i] >= CACHE_SIZE / 2 &&
            g_cache_off[o] != end) {
            g_cache_off[o] = 0xFFFFFFFFu; g_cache_len[o] = 0;
            g_pf_idx = o; g_pf_off = end; g_pf_pending = 1; g_pf_busy = 1;
            __sync_synchronize();
            sceKernelSignalSema(g_pf_sema, 1);
        }
    }
    return 1;
}

static uint32_t g_xa_all, g_xa_play;   /* XA-mode stream: sectors read / audio sectors played, per 2 s */

void cdrom_stats(void)
{
    logf_("cd: %u synchronous pack reads, %u prefetch waits in 2 s\n", (unsigned)g_pf_sync, (unsigned)g_pf_waits);
    if (g_xa_all) logf_("cd: XA stream %u sectors, %u audio played in 2 s (filter %u/%u)\n", (unsigned)g_xa_all,
                        (unsigned)g_xa_play, g_filter_file, g_filter_chan);
    g_xa_all = g_xa_play = 0;
    g_pf_sync = g_pf_waits = 0;
    if (g_ld_first && now_us() - g_ld_last > 1500000) load_span_end();
}

static void push(uint64_t delay_us, uint8_t type, const uint8_t *b, int n)
{
    if (g_qn >= 8) { logf_("CD response queue overflow\n"); return; }
    Resp *r = &g_q[g_qn++];
    if (delay_us > 1000) g_ld_delay_us += delay_us;
    uint64_t base = now_us();
    if (g_qn > 1 && g_q[g_qn - 2].at > base) base = g_q[g_qn - 2].at;
    r->at = base + delay_us;
    r->type = type;
    r->n = (uint8_t)n;
    memcpy(r->b, b, n);
}

static void push_stat(uint64_t d, uint8_t type) { push(d, type, &g_stat, 1); }

/* Rebuild the raw 2352-byte sector (sync, header, subheader, data) for an MSF-absolute sector number. */
static int read_sector(uint32_t msf_sector)
{
    if (g_pak < 0 || msf_sector < LEADIN || msf_sector - LEADIN >= g_disc_sectors) return 0;
    uint32_t lba = msf_sector - LEADIN;
    memset(g_sector, 0, SECTOR_RAW);
    memset(g_sector + 1, 0xFF, 10);
    g_sector[12] = i2bcd(msf_sector / 4500);
    g_sector[13] = i2bcd(msf_sector / 75 % 60);
    g_sector[14] = i2bcd(msf_sector % 75);
    g_sector[15] = 2;
    int lo = 0, hi = (int)g_nruns - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const PakRun *r = &g_runs[mid];
        if (lba < r->lba) hi = mid - 1;
        else if (lba >= r->lba + r->count) lo = mid + 1;
        else {
            uint32_t off = r->offset + (lba - r->lba) * r->stride;
            if (r->stride == 2336) return pak_read(off, g_sector + 16, 2336);
            memcpy(g_sector + 16, r->sub, 4);
            memcpy(g_sector + 20, r->sub, 4);
            return pak_read(off, g_sector + 24, 2048);
        }
    }
    return 1;                      /* sector not in the pack (padding): zeros */
}

static void drop_sector(void)       /* a new read/pause/stop: an untaken sector is discarded */
{
    g_data_ready = 0;
    g_consumed = 1;
}

static void read_stats(void)
{
    if (g_st_sectors >= 32)
        logf_("cd: read of %u data sectors took %u ms\n", (unsigned)g_st_sectors,
              (unsigned)((now_us() - g_st_start) / 1000));
    g_st_sectors = 0;
}

static void stop_reading(void)
{
    read_stats();
    drop_sector();
    g_reading = 0;
    g_stat &= ~0x60;
}

static void command(uint8_t cmd)
{
    const uint64_t ACK = (uint64_t)g_cd_ack_us;   /* us until INT3 */
    g_ld_cmd[cmd & 31]++;
    switch (cmd) {
    case 0x01: push_stat(ACK, 3); break;                                    /* Getstat */
    case 0x02:                                                              /* Setloc */
        g_setloc = (bcd2i(g_params[0]) * 60 + bcd2i(g_params[1])) * 75 + bcd2i(g_params[2]);
        push_stat(ACK, 3);
        break;
    case 0x06: case 0x1B:                                                   /* ReadN / ReadS */
        read_stats();
        drop_sector();
        g_pos = g_setloc;
        g_reading = 1;
        g_stat = (g_stat & ~0x40) | 0x22;
        push_stat(ACK, 3);
        g_next_sector_at = now_us() + (uint64_t)g_cd_first_us;
        break;
    case 0x07: g_stat |= 0x02; push_stat(ACK, 3); push_stat(50000, 2); break;  /* MotorOn */
    case 0x08: stop_reading(); push_stat(ACK, 3); g_stat &= ~0x02; push_stat(100000, 2); break;  /* Stop */
    case 0x09: push_stat(ACK, 3); stop_reading(); push_stat((uint64_t)g_cd_pause_us, 2); break;    /* Pause (a drive takes ~30 ms; GT2 pauses after every read) */
    case 0x0A:                                                              /* Init */
        g_mode = 0x20;
        stop_reading();
        g_stat = 0x02;
        push_stat(ACK, 3);
        push_stat(20000, 2);
        break;
    case 0x0B: case 0x0C: g_muted = cmd == 0x0B; push_stat(ACK, 3); break;  /* Mute / Demute */
    case 0x0D: g_filter_file = g_params[0]; g_filter_chan = g_params[1]; push_stat(ACK, 3); break;
    case 0x0E: g_mode = g_params[0]; push_stat(ACK, 3); break;              /* Setmode */
    case 0x0F: { uint8_t b[5] = {g_stat, g_mode, 0, g_filter_file, g_filter_chan}; push(ACK, 3, b, 5); break; }
    case 0x10: push(ACK, 3, g_last_hdr, 8); break;                          /* GetlocL */
    case 0x11: {                                                            /* GetlocP */
        uint32_t rel = g_pos - LEADIN, a = g_pos;
        uint8_t b[8] = {1, 1, i2bcd(rel / 4500), i2bcd(rel / 75 % 60), i2bcd(rel % 75),
                        i2bcd(a / 4500), i2bcd(a / 75 % 60), i2bcd(a % 75)};
        push(ACK, 3, b, 8);
        break;
    }
    case 0x13: { uint8_t b[3] = {g_stat, 1, 1}; push(ACK, 3, b, 3); break; }    /* GetTN */
    case 0x14: {                                                            /* GetTD */
        uint32_t t = g_params[0] == 0 ? g_disc_sectors + LEADIN : LEADIN;
        uint8_t b[3] = {g_stat, i2bcd(t / 4500), i2bcd(t / 75 % 60)};
        push(ACK, 3, b, 3);
        break;
    }
    case 0x15: case 0x16:                                                   /* SeekL / SeekP */
        stop_reading();
        g_pos = g_setloc;
        g_stat |= 0x40;
        push_stat(ACK, 3);
        g_stat &= ~0x40;
        push_stat((uint64_t)g_cd_seek_us, 2);                               /* seek done (no real drive) */
        break;
    case 0x19:                                                              /* Test */
        if (g_params[0] == 0x20) { uint8_t b[4] = {0x94, 0x09, 0x19, 0xC0}; push(ACK, 3, b, 4); }
        else { logf_("CD Test(%02x)\n", g_params[0]); push_stat(ACK, 3); }
        break;
    case 0x1A: {                                                            /* GetID */
        push_stat(ACK, 3);
        if (g_pak >= 0) { uint8_t b[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}; push(20000, 2, b, 8); }
        else { uint8_t b[2] = {0x11, 0x80}; push(20000, 5, b, 2); }
        break;
    }
    case 0x1E: push_stat(ACK, 3); push_stat(500000, 2); break;              /* ReadTOC */
    default:
        logf_("CD command %02x not implemented\n", cmd);
        { uint8_t b[2] = {(uint8_t)(g_stat | 1), 0x40}; push(ACK, 5, b, 2); }
        break;
    }
    g_nparams = 0;
}

static void update_irq(void)
{
    if (g_int_flag & g_int_enable & 0x1F) raise_irq(2);
}

static void deliver_at(uint64_t t)
{
    if (g_int_flag & 7) return;                /* previous interrupt not acknowledged */
    if (g_qn && g_q[0].at <= t) {
        Resp r = g_q[0];
        memmove(g_q, g_q + 1, sizeof(Resp) * (--g_qn));
        memcpy(g_resp, r.b, r.n);
        g_nresp = r.n;
        g_resp_pos = 0;
        g_int_flag = (g_int_flag & ~7) | r.type;
        update_irq();
        return;
    }
    /* Data sectors come as fast as the game takes them: a new one only after the previous one was consumed, so the
     * interrupt handler is never overtaken in the middle of reading a sector. */
    /* No overrun: a slow frame (or a slow PSP) must never let a new sector replace one the game has not taken yet,
     * that desynchronises libcd's interrupt handler (it then waits forever for data). */
    /* Data reads: never while the game is inside an interrupt handler. libcd's handler loops as long as the CD
     * raises interrupts; a sector that arrives before it returns keeps it inside, the game's main code (which
     * sends Pause after the last sector) never runs and the read runs away (55k sectors at 0.1 ms spacing). A real
     * drive needs 6.7 ms per sector; here a sector follows as soon as the handler has returned once, so the pace
     * is the game's own and no clock is needed (cd_period_us, default 0). XA streaming keeps its real clock. */
    extern int rt_in_irq(void);
    if (!g_cd_legacy && !(g_mode & 0x40) && rt_in_irq()) return;
    if (g_reading && t >= g_next_sector_at && !g_qn && g_consumed) {
        /* XA streaming runs on a fixed clock (75/150 sectors per second): the next sector is due one period after
         * the previous due time, not after this delivery, otherwise the stream drifts behind real time and the music
         * buffer runs dry. Data reads: g_period_us (0 = as soon as the game is out of the interrupt handler). */
        if (g_mode & 0x40) {
            uint64_t period = (g_mode & 0x80) ? 6667 : 13333;
            /* catch up after long stretches without a poll: a movie frame's MDEC decode runs the game for up to
             * ~70 ms without one, a 4-period window lost that time and the movie's XA audio ran slow with gaps */
            g_next_sector_at = (t - g_next_sector_at < 40 * period) ? g_next_sector_at + period : t + period;
        } else {
            g_next_sector_at = t + g_period_us;
        }
        if (g_mode & 0x40) g_xa_all++;
        uint64_t tr = now_us();
        int ok = read_sector(g_pos);
        tr = now_us() - tr;
        if (!ok) {
            stop_reading();
            uint8_t b[2] = {(uint8_t)(g_stat | 1), 0x10};
            push(0, 5, b, 2);
            return;
        }
        g_pos++;
        memcpy(g_last_hdr, g_sector + 12, 8);
        uint8_t submode = g_sector[18];
        if ((g_mode & 0x40) && (submode & 0x04)) {
            /* XA-ADPCM audio sector (music): goes to the SPU, no data interrupt. With the filter (mode bit 3)
             * only the selected file/channel of the interleaved stream plays. */
            /* audio sectors need nothing from the game: the ones already due follow at once (catch-up) */
            if (!((g_mode & 0x08) && (g_sector[16] != g_filter_file || g_sector[17] != g_filter_chan))) {
                g_xa_play++;
                if (!g_muted) spu_xa_sector(g_sector);
            }
            if (t >= g_next_sector_at) deliver_at(t);
            return;
        }
        /* into the sector buffer; the data FIFO is only (re)loaded by the request register */
        if (!g_st_sectors++) g_st_start = t;
        if (g_ld_first && t - g_ld_last > 1500000) load_span_end();
        if (!g_ld_first) g_ld_first = t;
        g_ld_last = t; g_ld_sectors++; g_ld_pak_us += tr;
        if (g_mode & 0x20) { memcpy(g_buf, g_sector + 12, 0x924); g_buf_len = 0x924; }
        else { memcpy(g_buf, g_sector + 24, 0x800); g_buf_len = 0x800; }
        g_data_ready = 1;
        g_consumed = 0;
        memcpy(g_resp, &g_stat, 1);
        g_nresp = 1;
        g_resp_pos = 0;
        g_int_flag = (g_int_flag & ~7) | 1;
        update_irq();
    }
}

static void deliver(void)
{
    if (g_int_flag & 7) return;
    deliver_at(now_us());
}

extern uint64_t g_poll_now;
void cdrom_tick(void) { deliver_at(g_poll_now); }

uint32_t cdrom_read(uint32_t reg)
{
    deliver();
    switch (reg) {
    case 0: {
        uint8_t s = g_index;
        if (g_nparams == 0) s |= 0x08;
        if (g_nparams < 16) s |= 0x10;
        if (g_resp_pos < g_nresp) s |= 0x20;
        if (g_data_pos < g_data_len) s |= 0x40;
        return s;
    }
    case 1: return g_resp_pos < g_nresp ? g_resp[g_resp_pos++] : 0;
    case 2:
        if (g_data_pos >= g_data_len) return 0;
        return g_data[g_data_pos++];
    case 3: return (g_index & 1) ? (0xE0 | g_int_flag) : (0xE0 | g_int_enable);
    }
    return 0;
}

void cdrom_write(uint32_t reg, uint32_t v)
{
    v &= 0xFF;
    if (reg == 0) { g_index = v & 3; return; }
    switch (reg * 4 + g_index) {
    case 1 * 4 + 0: command((uint8_t)v); break;
    case 2 * 4 + 0: if (g_nparams < 16) g_params[g_nparams++] = (uint8_t)v; break;
    case 2 * 4 + 1: g_int_enable = v & 0x1F; break;
    case 3 * 4 + 0:                                     /* request register */
        /* BFRD: 1 = load the buffered sector into the data FIFO (only if the FIFO is empty), 0 = clear FIFO */
        if (v & 0x80) {
            if (g_data_ready && g_data_pos >= g_data_len) {
                memcpy(g_data, g_buf, g_buf_len);
                g_data_len = g_buf_len;
                g_data_pos = 0;
                g_data_ready = 0;
                g_consumed = 1;
            }
        } else {
            g_data_pos = g_data_len;
            g_consumed = 1;
        }
        break;
    case 3 * 4 + 1:                                     /* interrupt flag acknowledge */
        g_int_flag &= ~(v & 0x1F);
        if (v & 0x40) g_nparams = 0;
        if ((g_int_flag & 7) == 0) { g_nresp = 0; g_resp_pos = 0; }
        deliver();
        break;
    default: break;                                     /* audio volume registers etc. */
    }
}

void cdrom_dma(uint32_t madr, uint32_t words)
{
    for (uint32_t i = 0; i < words * 4; i++) {
        uint8_t b = g_data_pos < g_data_len ? g_data[g_data_pos++] : 0;
        g_ram[(madr + i) & (PSX_RAM_SIZE - 1)] = b;
    }
}

int cdrom_read_data(uint32_t lba, uint32_t count, uint8_t *dst)
{
    for (uint32_t i = 0; i < count; i++) {
        if (!read_sector(lba + i + LEADIN)) return 0;
        memcpy(dst + i * 2048, g_sector + 24, 2048);
    }
    return 1;
}
