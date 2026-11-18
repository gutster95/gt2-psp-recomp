/* Inline fast paths for the most frequent GTE commands (included by gt2rt.h).
 *
 * The command word is a constant at every call site in the generated code, so after inlining the compiler folds the
 * decoding (sf, lm, matrix, vector, translation) away and keeps only the arithmetic for that one variant. Semantics
 * are identical to gte.c (mul_mat_vec + macs_to_irs, NCLIP); everything else goes to gte_cmd_slow(). */
#pragma once

void gte_cmd_slow(PsxCpu *c, uint32_t insn);

#define GTE_FLAG_ERR 0x7F87E000u

GT2_INLINE int32_t gtef_mat(const PsxCpu *c, int base, int row, int col)
{
    int idx = row * 3 + col;
    uint32_t w = c->gte_ctrl[base + idx / 2];
    return (idx & 1) ? (int16_t)(w >> 16) : (int16_t)(w & 0xFFFF);
}

GT2_INLINE void gtef_mac_ir(PsxCpu *c, int i, int64_t v, int sf, int lm, uint32_t *flag)
{
    if (v > 0x7FFFFFFFFFFLL) *flag |= 1u << (31 - i);
    else if (v < -0x80000000000LL) *flag |= 1u << (28 - i);
    int32_t m = (int32_t)(sf ? (v >> 12) : v);
    c->gte_data[24 + i] = (uint32_t)m;                       /* MAC1-3 */
    int32_t lo = lm ? 0 : -0x8000;
    if (m < lo) { m = lo; *flag |= 1u << (25 - i); }
    else if (m > 0x7FFF) { m = 0x7FFF; *flag |= 1u << (25 - i); }
    c->gte_data[8 + i] = (uint32_t)m;                        /* IR1-3 */
}

GT2_INLINE void gte_mvmva_fast(PsxCpu *c, uint32_t insn)
{
    int sf = (insn >> 19) & 1, lm = (insn >> 10) & 1, mx = (insn >> 17) & 3, vv = (insn >> 15) & 3, tv = (insn >> 13) & 3;
    if (mx == 3) { gte_cmd_slow(c, insn); return; }          /* garbage matrix: rare */
    int32_t v1, v2, v3, t1 = 0, t2 = 0, t3 = 0;
    if (vv < 3) {
        uint32_t xy = c->gte_data[vv * 2];
        v1 = (int16_t)(xy & 0xFFFF); v2 = (int16_t)(xy >> 16); v3 = (int16_t)(c->gte_data[vv * 2 + 1] & 0xFFFF);
    } else {
        v1 = (int16_t)c->gte_data[9]; v2 = (int16_t)c->gte_data[10]; v3 = (int16_t)c->gte_data[11];
    }
    if (tv < 3) {
        int b = tv == 0 ? 5 : tv == 1 ? 13 : 21;             /* TR, BK, FC */
        t1 = (int32_t)c->gte_ctrl[b]; t2 = (int32_t)c->gte_ctrl[b + 1]; t3 = (int32_t)c->gte_ctrl[b + 2];
    }
    int base = mx == 0 ? 0 : mx == 1 ? 8 : 16;               /* RT, L, LR */
    uint32_t flag = 0;
    gtef_mac_ir(c, 1, ((int64_t)t1 << 12) + (int64_t)gtef_mat(c, base, 0, 0) * v1 + (int64_t)gtef_mat(c, base, 0, 1) * v2 +
                (int64_t)gtef_mat(c, base, 0, 2) * v3, sf, lm, &flag);
    gtef_mac_ir(c, 2, ((int64_t)t2 << 12) + (int64_t)gtef_mat(c, base, 1, 0) * v1 + (int64_t)gtef_mat(c, base, 1, 1) * v2 +
                (int64_t)gtef_mat(c, base, 1, 2) * v3, sf, lm, &flag);
    gtef_mac_ir(c, 3, ((int64_t)t3 << 12) + (int64_t)gtef_mat(c, base, 2, 0) * v1 + (int64_t)gtef_mat(c, base, 2, 1) * v2 +
                (int64_t)gtef_mat(c, base, 2, 2) * v3, sf, lm, &flag);
    if (flag & GTE_FLAG_ERR) flag |= 0x80000000u;
    c->gte_ctrl[31] = flag;
}

GT2_INLINE void gte_nclip_fast(PsxCpu *c)
{
    uint32_t s0 = c->gte_data[12], s1 = c->gte_data[13], s2 = c->gte_data[14];
    int32_t x0 = (int16_t)(s0 & 0xFFFF), y0 = (int16_t)(s0 >> 16), x1 = (int16_t)(s1 & 0xFFFF), y1 = (int16_t)(s1 >> 16);
    int32_t x2 = (int16_t)(s2 & 0xFFFF), y2 = (int16_t)(s2 >> 16);
    int64_t v = (int64_t)x0 * y1 + (int64_t)x1 * y2 + (int64_t)x2 * y0 - (int64_t)x0 * y2 - (int64_t)x1 * y0 -
                (int64_t)x2 * y1;
    uint32_t flag = 0;
    if (v > 0x7FFFFFFFLL) flag = (1u << 16) | 0x80000000u;
    else if (v < -0x80000000LL) flag = (1u << 15) | 0x80000000u;
    c->gte_data[24] = (uint32_t)(int32_t)v;                   /* MAC0 */
    c->gte_ctrl[31] = flag;
}

/* ---- RTPS / RTPT -------------------------------------------------------------------------------------------- */

extern uint8_t g_gte_unr[0x101];          /* UNR division table (gte.c) */
extern int g_hires, g_widescreen;
void ge_mark_3d_precise(uint32_t xy, float x, float y, float z);

GT2_INLINE uint32_t gtef_divide(uint32_t h, uint32_t sz3, uint32_t *flag)
{
    if (h < sz3 * 2) {
        int z = __builtin_clz(sz3) - 16;
        uint32_t n = h << z, d = sz3 << z;
        uint32_t u = g_gte_unr[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
    }
    *flag |= 1u << 17;
    return 0x1FFFF;
}

GT2_INLINE void gtef_mac0(PsxCpu *c, int64_t v, uint32_t *flag)
{
    if (v > 0x7FFFFFFFLL) *flag |= 1u << 16;
    else if (v < -0x80000000LL) *flag |= 1u << 15;
    c->gte_data[24] = (uint32_t)(int32_t)v;
}

/* one vertex of RTPS/RTPT, same semantics as gte.c rtp() */
GT2_INLINE void gtef_rtp(PsxCpu *c, int n, int sf, int lm, int last, uint32_t *flag)
{
    uint32_t *D = c->gte_data, *C = c->gte_ctrl;
    uint32_t vxy = D[n * 2];
    int32_t x = (int16_t)(vxy & 0xFFFF), y = (int16_t)(vxy >> 16), z = (int16_t)(D[n * 2 + 1] & 0xFFFF);
    int64_t m1 = ((int64_t)(int32_t)C[5] << 12) + (int64_t)gtef_mat(c, 0, 0, 0) * x + (int64_t)gtef_mat(c, 0, 0, 1) * y +
                 (int64_t)gtef_mat(c, 0, 0, 2) * z;
    int64_t m2 = ((int64_t)(int32_t)C[6] << 12) + (int64_t)gtef_mat(c, 0, 1, 0) * x + (int64_t)gtef_mat(c, 0, 1, 1) * y +
                 (int64_t)gtef_mat(c, 0, 1, 2) * z;
    int64_t m3 = ((int64_t)(int32_t)C[7] << 12) + (int64_t)gtef_mat(c, 0, 2, 0) * x + (int64_t)gtef_mat(c, 0, 2, 1) * y +
                 (int64_t)gtef_mat(c, 0, 2, 2) * z;
    gtef_mac_ir(c, 1, m1, sf, lm, flag);
    gtef_mac_ir(c, 2, m2, sf, lm, flag);
    {   /* MAC3, IR3 with the saturation-flag quirk (flag from MAC3 >> 12) */
        if (m3 > 0x7FFFFFFFFFFLL) *flag |= 1u << 28;
        else if (m3 < -0x80000000000LL) *flag |= 1u << 25;
        int32_t v = (int32_t)(sf ? (m3 >> 12) : m3);
        D[27] = (uint32_t)v;
        int32_t lo = lm ? 0 : -0x8000;
        int32_t sat = v < lo ? lo : v > 0x7FFF ? 0x7FFF : v;
        int32_t chk = (int32_t)(m3 >> 12);
        if (chk < -0x8000 || chk > 0x7FFF) *flag |= 1u << 22;
        D[11] = (uint32_t)sat;
    }
    {   /* push SZ */
        int64_t v = m3 >> 12;
        if (v < 0) { v = 0; *flag |= 1u << 18; } else if (v > 0xFFFF) { v = 0xFFFF; *flag |= 1u << 18; }
        D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = (uint32_t)v;
    }
    uint32_t q = gtef_divide(C[26] & 0xFFFF, D[19], flag);
    int64_t px = (int64_t)(int32_t)q * (int16_t)D[9];
    if (g_widescreen && g_hires) px = (px * 3) >> 2;   /* 16:9: the 4:3 field of view fills 3/4 of the width */
    int64_t sx = px + (int32_t)C[24];
    int64_t sy = (int64_t)(int32_t)q * (int16_t)D[10] + (int32_t)C[25];
    gtef_mac0(c, sx, flag);
    gtef_mac0(c, sy, flag);
    {   /* push SXY */
        int32_t ix = (int32_t)(sx >> 16), iy = (int32_t)(sy >> 16);
        if (ix < -0x400) { ix = -0x400; *flag |= 1u << 14; } else if (ix > 0x3FF) { ix = 0x3FF; *flag |= 1u << 14; }
        if (iy < -0x400) { iy = -0x400; *flag |= 1u << 13; } else if (iy > 0x3FF) { iy = 0x3FF; *flag |= 1u << 13; }
        D[12] = D[13]; D[13] = D[14];
        D[14] = ((uint32_t)ix & 0xFFFF) | ((uint32_t)iy << 16);
    }
    if (g_widescreen && g_hires)
        ge_mark_3d_precise(D[14], (float)(int32_t)sx * (1.0f / 65536.0f), (float)(int32_t)sy * (1.0f / 65536.0f),
                           (float)(int32_t)D[19]);
    if (last) {
        int64_t p = (int64_t)(int32_t)q * (int16_t)C[27] + (int32_t)C[28];
        gtef_mac0(c, p, flag);
        int64_t i0 = p >> 12;
        if (i0 < 0) { i0 = 0; *flag |= 1u << 12; } else if (i0 > 0x1000) { i0 = 0x1000; *flag |= 1u << 12; }
        D[8] = (uint32_t)i0;
    }
}

GT2_INLINE void gte_rtp_fast(PsxCpu *c, uint32_t insn, int three)
{
    int sf = (insn >> 19) & 1, lm = (insn >> 10) & 1;
    uint32_t flag = 0;
    if (three) {
        gtef_rtp(c, 0, sf, lm, 0, &flag);
        gtef_rtp(c, 1, sf, lm, 0, &flag);
        gtef_rtp(c, 2, sf, lm, 1, &flag);
    } else {
        gtef_rtp(c, 0, sf, lm, 1, &flag);
    }
    if (flag & GTE_FLAG_ERR) flag |= 0x80000000u;
    c->gte_ctrl[31] = flag;
}

GT2_INLINE void gte_cmd(PsxCpu *c, uint32_t insn)
{
    switch (insn & 63) {
    case 0x01: gte_rtp_fast(c, insn, 0); return;
    case 0x30: gte_rtp_fast(c, insn, 1); return;
    case 0x06: gte_nclip_fast(c); return;
    case 0x12: gte_mvmva_fast(c, insn); return;
    default: gte_cmd_slow(c, insn); return;
    }
}
