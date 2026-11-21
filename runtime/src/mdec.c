/* MDEC: the PS1 macroblock decoder (run-length coded DCT -> RGB), used by GT2 for compressed still images.
 * Implemented as a plain software decoder behind the register/DMA interface the game's libpress uses:
 *   cmd 1 = decode macroblocks, cmd 2 = set quantization tables, cmd 3 = set IDCT scale table.
 * Input (DMA0 / writes to 1F801820) is buffered until the command has all its parameter words; output (DMA1 /
 * reads of 1F801820) is decoded lazily, one macroblock at a time. Algorithms follow psx-spx. */
#include <string.h>
#include "rt.h"
#include "mdec.h"

#define IN_MAX 0x10000                     /* parameter words per command (16-bit count) */

static uint32_t g_cmd;                     /* current command word */
static uint32_t g_remain;                  /* parameter words still expected */
static uint32_t g_in[IN_MAX];
static uint32_t g_in_len;
static uint8_t g_qt[128];                  /* luma (0-63) and chroma (64-127) quantization, zigzag order */
static int16_t g_scale[64];
static int32_t g_sc8[64];                  /* g_scale[i] / 8 (C division, as psx-spx's IDCT), set by cmd 3 */
static inline int32_t div8192(int32_t v) { return v >= 0 ? v >> 13 : -((-v) >> 13); }   /* v / 0x2000, C semantics */
static int g_decoding;                     /* cmd 1 complete: output available */
static uint32_t g_rd;                      /* input read position in halfwords */
static uint32_t g_out[192];                /* one decoded macroblock (16x16x24 bit at most) */
static uint32_t g_out_len, g_out_pos;
static uint32_t g_pend_madr, g_pend_words; /* DMA1 started before the input was complete */
static int g_pending;
static uint32_t g_blocks;                  /* statistics */

static const uint8_t ZAGZIG[64] = {        /* coefficient index (zigzag order) -> position in the 8x8 block */
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
    21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60,
    61, 54, 47, 55, 62, 63};

void mdec_reset(void)
{
    g_cmd = 0;
    g_remain = 0;
    g_in_len = 0;
    g_decoding = 0;
    g_out_len = g_out_pos = 0;
}

static int next_half(uint16_t *h)
{
    uint32_t w = g_rd >> 1;
    if (w >= g_in_len) return 0;
    *h = (uint16_t)(g_in[w] >> ((g_rd & 1) * 16));
    g_rd++;
    return 1;
}

static int s10(uint32_t v) { return (int)((int32_t)(v << 22) >> 22); }

/* One run-length coded 8x8 block -> IDCT output (-128..127 range, unclamped). Returns 0 when the input ends. */
static int decode_block(int16_t *out, const uint8_t *qt)
{
    int32_t blk[64];
    uint16_t n;
    do { if (!next_half(&n)) return 0; } while (n == 0xFE00);
    memset(blk, 0, sizeof blk);
    int q = n >> 10, k = 0;
    unsigned cols = 0;                            /* columns y (blk[y + z*8]) with a nonzero coefficient */
    int32_t val = s10(n & 0x3FF) * qt[0];
    for (;;) {
        if (q == 0) val = s10(n & 0x3FF) * 2;
        if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
        int pos = q ? ZAGZIG[k] : k;
        blk[pos] = val;
        if (val) cols |= 1u << (pos & 7);
        if (!next_half(&n)) break;
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = (s10(n & 0x3FF) * qt[k] * q + 4) / 8;
    }
    /* IDCT (psx-spx): two passes of out[x + y*8] = (sum_z in[y + z*8] * (scale[x + z*8] / 8) + 0xFFF) / 0x2000, each
     * transposing. Same products and rounding as the plain loops, but only over what is nonzero: a movie block has a
     * few coefficients, so pass 1 runs only for the columns that have one, and pass 2 only sums over the rows pass 1
     * produced (the others are 0: (0 + 0xFFF) / 0x2000 = 0). 0.33 -> ~0.1 ms per macroblock (PPSSPP). */
    int32_t tmp[64];
    memset(tmp, 0, sizeof tmp);
    int ys[8], ny = 0;
    for (int y = 0; y < 8; y++) {
        if (!(cols >> y & 1)) continue;
        ys[ny++] = y;
        int zs[8], nz = 0;
        for (int z = 0; z < 8; z++) if (blk[y + z * 8]) zs[nz++] = z;
        for (int x = 0; x < 8; x++) {
            int32_t sum = 0;
            for (int i = 0; i < nz; i++) sum += blk[y + zs[i] * 8] * g_sc8[x + zs[i] * 8];
            tmp[x + y * 8] = div8192(sum + 0xFFF);
        }
    }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            int32_t sum = 0;
            for (int i = 0; i < ny; i++) sum += tmp[y + ys[i] * 8] * g_sc8[x + ys[i] * 8];
            out[x + y * 8] = (int16_t)div8192(sum + 0xFFF);
        }
    g_blocks++;
    return 1;
}

static int clamp8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

/* Decode the next macroblock into g_out. */
static int decode_macroblock(void)
{
    int depth = (g_cmd >> 27) & 3, is_signed = (g_cmd >> 26) & 1, bit15 = (g_cmd >> 25) & 1;
    int x80 = is_signed ? 0 : 0x80;
    g_out_len = g_out_pos = 0;
    if (depth < 2) {                                      /* monochrome: one Y block, 8x8 */
        int16_t y[64];
        if (!decode_block(y, g_qt)) return 0;
        uint8_t *o = (uint8_t *)g_out;
        if (depth == 1) {
            for (int i = 0; i < 64; i++) o[i] = (uint8_t)(clamp8(y[i]) ^ x80);
            g_out_len = 16;
        } else {
            for (int i = 0; i < 32; i++)
                o[i] = (uint8_t)((((clamp8(y[2 * i]) ^ x80) >> 4) & 15) | ((((clamp8(y[2 * i + 1]) ^ x80) >> 4) & 15) << 4));
            g_out_len = 8;
        }
        return 1;
    }
    int16_t cr[64], cb[64], yb[4][64];
    if (!decode_block(cr, g_qt + 64) || !decode_block(cb, g_qt + 64)) return 0;
    for (int i = 0; i < 4; i++) if (!decode_block(yb[i], g_qt)) return 0;
    uint8_t *o8 = (uint8_t *)g_out;
    uint16_t *o16 = (uint16_t *)g_out;
    int16_t cR[64], cG[64], cB[64];               /* chroma terms once per chroma sample (shared by 2x2 pixels) */
    for (int c = 0; c < 64; c++) {
        int R = cr[c], B = cb[c];
        cG[c] = (int16_t)((-88 * B - 183 * R) >> 8);
        cR[c] = (int16_t)((359 * R) >> 8);
        cB[c] = (int16_t)((454 * B) >> 8);
    }
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int c = (x >> 1) + (y >> 1) * 8;
            int R = cR[c], G = cG[c], B = cB[c];
            int Y = yb[(x >> 3) + (y >> 3) * 2][(x & 7) + (y & 7) * 8];
            int r = clamp8(Y + R) ^ x80, g = clamp8(Y + G) ^ x80, b = clamp8(Y + B) ^ x80;
            r &= 0xFF; g &= 0xFF; b &= 0xFF;
            if (depth == 2) {
                uint8_t *p = o8 + (y * 16 + x) * 3;
                p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
            } else {
                o16[y * 16 + x] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (bit15 << 15));
            }
        }
    g_out_len = depth == 2 ? 192 : 128;
    return 1;
}

static uint64_t g_dec_us;                  /* statistics: decode time (mdec_stats) */
static uint32_t g_dec_mbs;
static uint32_t out_word(void)
{
    if (g_out_pos >= g_out_len) {
        uint64_t t0 = now_us();
        int ok = g_decoding && decode_macroblock();
        g_dec_us += now_us() - t0;
        if (!ok) { g_decoding = 0; return 0; }
        g_dec_mbs++;
    }
    return g_out[g_out_pos++];
}

static void command_complete(void)
{
    uint32_t op = g_cmd >> 29;
    if (op == 1) {
        g_decoding = 1;
        g_rd = 0;
        g_out_len = g_out_pos = 0;
        if (g_pending) {
            g_pending = 0;
            mdec_dma_out(g_pend_madr, g_pend_words);
            io_dma_done(1);
        }
    } else if (op == 2) {
        memcpy(g_qt, g_in, (g_cmd & 1) ? 128 : 64);
    } else if (op == 3) {
        memcpy(g_scale, g_in, 128);
        for (int i = 0; i < 64; i++) g_sc8[i] = g_scale[i] / 8;
    }
}

static void write_word(uint32_t v)
{
    if (g_remain) {
        if (g_in_len < IN_MAX) g_in[g_in_len++] = v;
        if (--g_remain == 0) command_complete();
        return;
    }
    g_cmd = v;
    g_in_len = 0;
    g_decoding = 0;
    switch (v >> 29) {
    case 1: g_remain = v & 0xFFFF; break;
    case 2: g_remain = (v & 1) ? 32 : 16; break;
    case 3: g_remain = 32; break;
    default: g_remain = 0; break;
    }
    if (!g_remain && (v >> 29) >= 1 && (v >> 29) <= 3) command_complete();
}

void mdec_write(uint32_t reg, uint32_t v)
{
    if (reg == 0) { write_word(v); return; }
    if (v & 0x80000000u) mdec_reset();                   /* control: reset */
}

uint32_t mdec_read(uint32_t reg)
{
    if (reg == 0) return out_word();
    uint32_t st = ((g_cmd >> 25) & 15) << 23;
    if (g_out_pos >= g_out_len && !g_decoding) st |= 0x80000000u;   /* output FIFO empty */
    if (g_remain) st |= 0x20000000u | ((g_remain - 1) & 0xFFFF);   /* busy, waiting for parameters */
    else st |= 0xFFFF;
    return st;
}

void mdec_dma_in(uint32_t madr, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) write_word(*(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)));
}

int mdec_dma_out(uint32_t madr, uint32_t words)
{
    if (!g_decoding && g_remain && (g_cmd >> 29) == 1) {   /* input still arriving: finish later */
        g_pending = 1;
        g_pend_madr = madr;
        g_pend_words = words;
        return 0;
    }
    for (uint32_t i = 0; i < words; i++) *(uint32_t *)(g_ram + ((madr + 4 * i) & 0x1FFFFC)) = out_word();
    static int logged;
    if (logged < 8) {
        logged++;
        logf_("mdec: out %u words, cmd %08x, %u blocks decoded so far\n", (unsigned)words, (unsigned)g_cmd,
              (unsigned)g_blocks);
    }
    return 1;
}

/* every 2 s from rt_poll (core.c) while movies play */
void mdec_stats(void)
{
    if (!g_dec_mbs) return;
    logf_("mdec: %u macroblocks in 2 s, decode %u ms\n", (unsigned)g_dec_mbs, (unsigned)(g_dec_us / 1000));
    g_dec_mbs = 0; g_dec_us = 0;
}
