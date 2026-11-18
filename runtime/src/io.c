/* PS1 I/O area 0x1F801000-0x1F802FFF: interrupt controller, DMA, timers, GPU/SPU/CD ports.
 * Unhandled registers behave like plain memory and are logged on first access. */
#include <string.h>
#include "rt.h"
#include "gpu.h"
#include "cdrom.h"
#include "sio.h"
#include "card.h"
#include "spu.h"
#include "prof.h"

uint32_t g_i_stat, g_i_mask;
/* indexed by address - 0x1F800000, so offsets below read like the hardware addresses */
static uint8_t g_io[0x3000] __attribute__((aligned(4)));
static uint8_t g_seen[0x3000];
uint32_t g_vblank_count;
uint32_t g_gpu_lists;          /* GPU linked-list DMAs (~ game frames drawn) */
uint64_t g_gpu_us;             /* time spent executing GPU display lists (rasterizer) */

#define IO32(off) (*(uint32_t *)(g_io + (off)))
#define IO16(off) (*(uint16_t *)(g_io + (off)))

void io_init(void)
{
    memset(g_io, 0, sizeof g_io);
    IO32(0x1060) = 0x00000B88;   /* RAM_SIZE */
}

static void seen(uint32_t off, const char *rw, uint32_t v)
{
    if (g_seen[off]) return;
    g_seen[off] = 1;
    logf_("io first %s %08x = %08x\n", rw, (unsigned)(0x1F800000u + off), (unsigned)v);
}

void raise_irq(int bit) { g_i_stat |= 1u << bit; }

void io_vblank(void)
{
    g_vblank_count++;
    card_tick();
    raise_irq(0);
    gpu_present();
}

/* ---- DMA -------------------------------------------------------------------------------------------------- */

static void dma_irq(int ch)
{
    uint32_t dicr = IO32(0x10F4);
    if (dicr & (1u << (16 + ch))) {
        dicr |= 1u << (24 + ch);
        IO32(0x10F4) = dicr;
        if ((dicr & 0x00800000u) && (dicr & 0x7F000000u)) {
            IO32(0x10F4) |= 0x80000000u;
            raise_irq(3);
        }
    }
}

static void dma_run(int ch)
{
    uint32_t base = 0x1080 + ch * 0x10;
    uint32_t madr = IO32(base) & 0x1FFFFC, bcr = IO32(base + 4), chcr = IO32(base + 8);
    switch (ch) {
    case 2: /* GPU */
        if ((chcr & 0x600) == 0x400) {          /* linked list */
            PHASE_BEGIN(PH_GPU);
            g_gpu_lists++;
            uint64_t t0 = now_us();
            uint32_t a = madr, guard = 0;
            for (;;) {
                uint32_t hdr = *(uint32_t *)(g_ram + a);
                uint32_t n = hdr >> 24;
                if (a + 4 + 4 * n <= PSX_RAM_SIZE) gpu_packet((const uint32_t *)(g_ram + a + 4), n);
                else for (uint32_t i = 0; i < n; i++) gpu_gp0(*(uint32_t *)(g_ram + ((a + 4 + 4 * i) & 0x1FFFFC)));
                if ((hdr & 0x800000) || ++guard > 0x40000) break;
                a = hdr & 0x1FFFFC;
            }
            g_gpu_us += now_us() - t0;
            PHASE_END();
        } else {                                /* block / sync mode */
            uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1);
            if (chcr & 1) {
                for (uint32_t i = 0; i < words; i++) gpu_gp0(*(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)));
            } else {
                for (uint32_t i = 0; i < words; i++) *(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)) = gpu_read();
            }
        }
        break;
    case 3: /* CD-ROM */
        cdrom_dma(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1));
        break;
    case 4: /* SPU: sound RAM transfer (block mode: BS words x BA blocks) */
        spu_dma(madr, (bcr & 0xFFFF) * ((bcr >> 16) ? (bcr >> 16) : 1), chcr & 1);
        break;
    case 6: { /* OTC: reverse-clear ordering table */
        uint32_t n = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
        uint32_t a = madr;
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t *)(g_ram + a) = (i == n - 1) ? 0x00FFFFFFu : ((a - 4) & 0x1FFFFC);
            a = (a - 4) & 0x1FFFFC;
        }
        break;
    }
    default:
        logf_("DMA%d start madr=%08x bcr=%08x chcr=%08x (ignored)\n", ch, (unsigned)madr, (unsigned)bcr, (unsigned)chcr);
        break;
    }
    IO32(base + 8) = chcr & ~0x11000000u;
    dma_irq(ch);
}

/* ---- timers ------------------------------------------------------------------------------------------- */

static uint32_t timer_count(int t)
{
    uint64_t us = now_us();
    uint32_t mode = IO32(0x1104 + t * 0x10);
    uint32_t v;
    if (t == 1 && (mode & 0x100)) v = (uint32_t)(us / 63);           /* hblank: ~15734 Hz */
    else if (t == 2 && (mode & 0x200)) v = (uint32_t)(us * 33869 / 8000); /* sysclk/8 */
    else v = (uint32_t)(us * 33869 / 1000);                           /* sysclk 33.8688 MHz */
    uint32_t target = IO32(0x1108 + t * 0x10) & 0xFFFF;
    if ((mode & 0x8) && target) v %= target + 1;
    return v & 0xFFFF;
}

/* ---- register access ---------------------------------------------------------------------------------- */

static uint32_t io_read(uint32_t a, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x1F801000u || p >= 0x1F803000u) {
        if (p >= 0x1FC00000u && p < 0x1FC80000u) return 0;   /* BIOS ROM */
        logf_("read%d from unmapped %08x\n", size * 8, (unsigned)a);
        return 0;
    }
    uint32_t off = p - 0x1F800000u;
    uint32_t v;
    if (off >= 0x1800 && off < 0x1804) return cdrom_read(off - 0x1800);
    if (off >= 0x1040 && off < 0x1050) return sio_read(off - 0x1040, size);
    if (off >= 0x1C00 && off < 0x2000) {
        if (size == 4) return spu_read16(off) | (spu_read16(off + 2) << 16);
        v = spu_read16(off);
        return size == 1 ? (v >> ((off & 1) * 8)) & 0xFF : v;
    }
    switch (off & ~3u) {
    case 0x1070: v = g_i_stat; break;
    case 0x1074: v = g_i_mask; break;
    case 0x1810: v = gpu_read(); break;
    case 0x1814: v = gpu_status() | ((g_vblank_count & 1) << 31); break;
    case 0x1100: case 0x1110: case 0x1120: v = timer_count((off - 0x1100) >> 4); break;
    default:
        v = size == 4 ? IO32(off & ~3u) : size == 2 ? IO16(off & ~1u) : g_io[off];
        seen(off, "read", v);
        return v;
    }
    if (size == 2) v = (off & 2) ? v >> 16 : v & 0xFFFF;
    else if (size == 1) v = (v >> ((off & 3) * 8)) & 0xFF;
    return v;
}

static void io_write(uint32_t a, uint32_t v, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p == 0x1FFE0130u) return;                  /* cache control */
    if (p < 0x1F801000u || p >= 0x1F803000u) {
        logf_("write%d to unmapped %08x = %08x\n", size * 8, (unsigned)a, (unsigned)v);
        return;
    }
    uint32_t off = p - 0x1F800000u;
    if (off >= 0x1800 && off < 0x1804) { cdrom_write(off - 0x1800, v); return; }
    if (off >= 0x1040 && off < 0x1050) { sio_write(off - 0x1040, v, size); return; }
    if (off >= 0x1C00 && off < 0x2000) {
        if (size == 4) { spu_write16(off, v & 0xFFFF); spu_write16(off + 2, v >> 16); }
        else spu_write16(off, v);
        return;
    }
    switch (off & ~3u) {
    case 0x1070: g_i_stat &= v; return;           /* acknowledge */
    case 0x1074: g_i_mask = v & 0x7FF; return;
    case 0x1810: gpu_gp0(v); return;
    case 0x1814: gpu_gp1(v); return;
    case 0x10F4: {                                 /* DICR: writing 1 to flag bits clears them */
        uint32_t d = IO32(0x10F4);
        d = (d & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
        if ((d & 0x00800000u) && (d & 0x7F000000u)) d |= 0x80000000u;
        IO32(0x10F4) = d;
        return;
    }
    }
    if (size == 4) IO32(off & ~3u) = v;
    else if (size == 2) IO16(off & ~1u) = (uint16_t)v;
    else g_io[off] = (uint8_t)v;
    if (off >= 0x1080 && off < 0x10F0 && (off & 0xF) == 8 && (v & 0x01000000u)) {
        int ch = (off - 0x1080) >> 4;
        if (IO32(0x10F0) & (8u << (ch * 4))) dma_run(ch);
        return;
    }
    seen(off, "write", v);
}

uint32_t io_r8(uint32_t a) { return io_read(a, 1); }
uint32_t io_r16(uint32_t a) { return io_read(a, 2); }
uint32_t io_r32(uint32_t a) { return io_read(a, 4); }
void io_w8(uint32_t a, uint32_t v) { io_write(a, v, 1); }
void io_w16(uint32_t a, uint32_t v) { io_write(a, v, 2); }
void io_w32(uint32_t a, uint32_t v) { io_write(a, v, 4); }
