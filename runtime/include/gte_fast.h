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

/* ---- sf = 1 fast core ----------------------------------------------------------------------------------------
 * With sf = 1, (T << 12 + S) >> 12 == T + (S >> 12) exactly (floor division by 4096 of a multiple of 4096 plus S).
 * S, the sum of three 16x16-bit products, fits in 34 bits; one mult/madd/madd chain in HI/LO computes it (the
 * compiler interleaved the three int64 sums and shuffled HI/LO through registers: ~330 cycles per RTPS). The 44-bit
 * MAC overflow is exactly the signed overflow of the 32-bit T + (S >> 12). */
GT2_INLINE int32_t gtef_dot3(int32_t a, int32_t x, int32_t b, int32_t y, int32_t d, int32_t z)
{
#ifdef __psp__
    int32_t hi; uint32_t lo;
    __asm__("mult %2, %3\n\tmadd %4, %5\n\tmadd %6, %7\n\tmfhi %0\n\tmflo %1"
            : "=r"(hi), "=r"(lo) : "r"(a), "r"(x), "r"(b), "r"(y), "r"(d), "r"(z) : "hi", "lo");
    return (int32_t)(((uint32_t)hi << 20) | (lo >> 12));
#else
    return (int32_t)(((int64_t)a * x + (int64_t)b * y + (int64_t)d * z) >> 12);
#endif
}

/* T + S12 as MAC1-3 with sf = 1; sets the MAC overflow flag bits like gtef_mac_ir. ovf: -1/0/+1 (the 64-bit sign) */
GT2_INLINE int32_t gtef_mac1(int i, int32_t t, int32_t s12, uint32_t *flag, int *ovf)
{
    int32_t v = (int32_t)((uint32_t)t + (uint32_t)s12);
    *ovf = 0;
    if (((t ^ v) & (s12 ^ v)) < 0) {
        if (t >= 0) { *flag |= 1u << (31 - i); *ovf = 1; } else { *flag |= 1u << (28 - i); *ovf = -1; }
    }
    return v;
}

/* Saturations without branches (Allegrex min/max): the flag code was a third of RTPT's time, mostly taken branches. */
GT2_INLINE int32_t gtef_clamp(int32_t v, int32_t lo, int32_t hi)
{
    int32_t r = v < lo ? lo : v;
    return r > hi ? hi : r;
}

GT2_INLINE int32_t gtef_ir(int i, int32_t m, int lm, uint32_t *flag)
{
    int32_t s = gtef_clamp(m, lm ? 0 : -0x8000, 0x7FFF);
    *flag |= (uint32_t)(s != m) << (25 - i);
    return s;
}

/* q * k + off as int64 (q <= 0x1FFFF, k 16 bit): HI/LO */
GT2_INLINE int64_t gtef_madd1(int32_t q, int32_t k, int32_t off)
{
#ifdef __psp__
    int32_t hi; uint32_t lo;
    __asm__("mtlo %2\n\tmthi %3\n\tmadd %4, %5\n\tmfhi %0\n\tmflo %1"
            : "=r"(hi), "=r"(lo) : "r"(off), "r"(off >> 31), "r"(q), "r"(k) : "hi", "lo");
    return (int64_t)(((uint64_t)(uint32_t)hi << 32) | lo);
#else
    return (int64_t)q * k + off;
#endif
}

/* fl = 0: FLAG is dead (the recompiler proved the game overwrites it before reading it): its computation is left out
 * and the register keeps its old value. */
GT2_INLINE void gte_mvmva_fast2(PsxCpu *c, uint32_t insn, int fl, int fast)
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
    if (sf && fast) {                                         /* see gtef_dot3 */
        int o;
        int32_t m1 = gtef_mac1(1, t1, gtef_dot3(gtef_mat(c, base, 0, 0), v1, gtef_mat(c, base, 0, 1), v2,
                                               gtef_mat(c, base, 0, 2), v3), &flag, &o);
        int32_t m2 = gtef_mac1(2, t2, gtef_dot3(gtef_mat(c, base, 1, 0), v1, gtef_mat(c, base, 1, 1), v2,
                                               gtef_mat(c, base, 1, 2), v3), &flag, &o);
        int32_t m3 = gtef_mac1(3, t3, gtef_dot3(gtef_mat(c, base, 2, 0), v1, gtef_mat(c, base, 2, 1), v2,
                                               gtef_mat(c, base, 2, 2), v3), &flag, &o);
        c->gte_data[25] = (uint32_t)m1; c->gte_data[26] = (uint32_t)m2; c->gte_data[27] = (uint32_t)m3;
        c->gte_data[9] = (uint32_t)gtef_ir(1, m1, lm, &flag);
        c->gte_data[10] = (uint32_t)gtef_ir(2, m2, lm, &flag);
        c->gte_data[11] = (uint32_t)gtef_ir(3, m3, lm, &flag);
    } else {
    gtef_mac_ir(c, 1, ((int64_t)t1 << 12) + (int64_t)gtef_mat(c, base, 0, 0) * v1 + (int64_t)gtef_mat(c, base, 0, 1) * v2 +
                (int64_t)gtef_mat(c, base, 0, 2) * v3, sf, lm, &flag);
    gtef_mac_ir(c, 2, ((int64_t)t2 << 12) + (int64_t)gtef_mat(c, base, 1, 0) * v1 + (int64_t)gtef_mat(c, base, 1, 1) * v2 +
                (int64_t)gtef_mat(c, base, 1, 2) * v3, sf, lm, &flag);
    gtef_mac_ir(c, 3, ((int64_t)t3 << 12) + (int64_t)gtef_mat(c, base, 2, 0) * v1 + (int64_t)gtef_mat(c, base, 2, 1) * v2 +
                (int64_t)gtef_mat(c, base, 2, 2) * v3, sf, lm, &flag);
    }
    if (!fl) return;
    if (flag & GTE_FLAG_ERR) flag |= 0x80000000u;
    c->gte_ctrl[31] = flag;
}

GT2_INLINE void gte_mvmva_fast(PsxCpu *c, uint32_t insn, int fl) { gte_mvmva_fast2(c, insn, fl, 1); }

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

/* q * k + off as a 64-bit HI:LO pair (q <= 0x1FFFF, k 16 bit) */
GT2_INLINE void gtef_madd_hl(int32_t q, int32_t k, int32_t off, int32_t *hi, uint32_t *lo)
{
#ifdef __psp__
    __asm__("mtlo %2\n\tmthi %3\n\tmadd %4, %5\n\tmfhi %0\n\tmflo %1"
            : "=r"(*hi), "=r"(*lo) : "r"(off), "r"(off >> 31), "r"(q), "r"(k) : "hi", "lo");
#else
    int64_t v = (int64_t)q * k + off;
    *hi = (int32_t)(v >> 32); *lo = (uint32_t)v;
#endif
}

/* MAC0 = low word; overflow flag when the value does not fit 32 bits */
GT2_INLINE void gtef_mac0_hl(PsxCpu *c, int32_t hi, uint32_t lo, uint32_t *flag)
{
    *flag |= (uint32_t)(hi != ((int32_t)lo >> 31)) << (hi < 0 ? 15 : 16);
    c->gte_data[24] = lo;
}

/* one vertex of RTPS/RTPT with sf = 1, bit-exact with gtef_rtp */
GT2_INLINE void gtef_rtp1(PsxCpu *c, int n, int lm, int last, uint32_t *flag)
{
    uint32_t *D = c->gte_data, *C = c->gte_ctrl;
    uint32_t vxy = D[n * 2];
    int32_t x = (int16_t)(vxy & 0xFFFF), y = (int16_t)(vxy >> 16), z = (int16_t)(D[n * 2 + 1] & 0xFFFF);
    uint32_t r0 = C[0], r1 = C[1], r2 = C[2], r3 = C[3], r4 = C[4];
    int o1, o2, o3;
    int32_t m1 = gtef_mac1(1, (int32_t)C[5], gtef_dot3((int16_t)r0, x, (int16_t)(r0 >> 16), y, (int16_t)r1, z), flag, &o1);
    int32_t m2 = gtef_mac1(2, (int32_t)C[6], gtef_dot3((int16_t)(r1 >> 16), x, (int16_t)r2, y, (int16_t)(r2 >> 16), z), flag, &o2);
    int32_t m3 = gtef_mac1(3, (int32_t)C[7], gtef_dot3((int16_t)r3, x, (int16_t)(r3 >> 16), y, (int16_t)r4, z), flag, &o3);
    (void)o1; (void)o2;
    D[25] = (uint32_t)m1; D[26] = (uint32_t)m2; D[27] = (uint32_t)m3;
    int32_t ir1 = gtef_ir(1, m1, lm, flag), ir2 = gtef_ir(2, m2, lm, flag);
    D[9] = (uint32_t)ir1; D[10] = (uint32_t)ir2;
    {   /* IR3: saturated per lm, the flag from MAC3 >> 12 against -0x8000..0x7FFF regardless of lm */
        D[11] = (uint32_t)gtef_clamp(m3, lm ? 0 : -0x8000, 0x7FFF);
        *flag |= (uint32_t)((uint32_t)(m3 + 0x8000) > 0xFFFFu) << 22;
    }
    {   /* push SZ: clamp of the (unwrapped) MAC3 >> 12 */
        uint32_t sz;
        if (__builtin_expect(o3 != 0, 0)) { sz = o3 > 0 ? 0xFFFF : 0; *flag |= 1u << 18; }
        else { sz = (uint32_t)gtef_clamp(m3, 0, 0xFFFF); *flag |= (uint32_t)(sz != (uint32_t)m3) << 18; }
        D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = sz;
    }
    uint32_t q = gtef_divide(C[26] & 0xFFFF, D[19], flag);
    int32_t xh, yh; uint32_t xl, yl;
    gtef_madd_hl((int32_t)q, ir1, (int32_t)C[24], &xh, &xl);
    gtef_madd_hl((int32_t)q, ir2, (int32_t)C[25], &yh, &yl);
    gtef_mac0_hl(c, xh, xl, flag);
    gtef_mac0_hl(c, yh, yl, flag);
    {   /* push SXY: MAC0 >> 16, |MAC0| < 2^47 */
        int32_t rx = (int32_t)(((uint32_t)xh << 16) | (xl >> 16)), ry = (int32_t)(((uint32_t)yh << 16) | (yl >> 16));
        int32_t ix = gtef_clamp(rx, -0x400, 0x3FF), iy = gtef_clamp(ry, -0x400, 0x3FF);
        *flag |= ((uint32_t)(ix != rx) << 14) | ((uint32_t)(iy != ry) << 13);
        D[12] = D[13]; D[13] = D[14];
        D[14] = ((uint32_t)ix & 0xFFFF) | ((uint32_t)iy << 16);
    }
    if (g_widescreen && g_hires)
        ge_mark_3d_precise(D[14], (float)(int32_t)xl * (1.0f / 65536.0f), (float)(int32_t)yl * (1.0f / 65536.0f),
                           (float)(int32_t)D[19]);
    if (last) {
        int32_t ph; uint32_t pl;
        gtef_madd_hl((int32_t)q, (int16_t)C[27], (int32_t)C[28], &ph, &pl);
        gtef_mac0_hl(c, ph, pl, flag);
        int32_t r = (int32_t)(((uint32_t)ph << 20) | (pl >> 12)), i0 = gtef_clamp(r, 0, 0x1000);
        *flag |= (uint32_t)(i0 != r) << 12;
        D[8] = (uint32_t)i0;
    }
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

/* reference version (any sf), kept for sf = 0 and the self-test in gte.c gte_bench */
GT2_INLINE void gte_rtp_ref(PsxCpu *c, uint32_t insn, int three, int fl)
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
    if (!fl) return;
    if (flag & GTE_FLAG_ERR) flag |= 0x80000000u;
    c->gte_ctrl[31] = flag;
}

GT2_INLINE void gte_rtp_fast(PsxCpu *c, uint32_t insn, int three, int fl)
{
    if (!((insn >> 19) & 1)) { gte_rtp_ref(c, insn, three, fl); return; }
    int lm = (insn >> 10) & 1;
    uint32_t flag = 0;
    if (three) {
        gtef_rtp1(c, 0, lm, 0, &flag);
        gtef_rtp1(c, 1, lm, 0, &flag);
        gtef_rtp1(c, 2, lm, 1, &flag);
    } else {
        gtef_rtp1(c, 0, lm, 1, &flag);
    }
    if (!fl) return;
    if (flag & GTE_FLAG_ERR) flag |= 0x80000000u;
    c->gte_ctrl[31] = flag;
}

/* Out-of-line copies of the bigger fast paths (gte.c). GT2_GTE_OUTLINE (generated code): calls instead of inline
 * expansion - an inline RTPT is ~300 instructions, and the renderer functions grew to 66 KB with them (the PSP's
 * instruction cache is 16 KB). */
void gte_rtps_o(PsxCpu *c, uint32_t insn);
void gte_rtpt_o(PsxCpu *c, uint32_t insn);
void gte_mvmva_o(PsxCpu *c, uint32_t insn);
void gte_rtps_nf(PsxCpu *c, uint32_t insn);       /* FLAG not computed (see gte_mvmva_fast) */
void gte_rtpt_nf(PsxCpu *c, uint32_t insn);
void gte_mvmva_nf(PsxCpu *c, uint32_t insn);

GT2_INLINE void gte_cmd(PsxCpu *c, uint32_t insn)
{
    switch (insn & 63) {
#ifdef GT2_GTE_OUTLINE
    case 0x01: gte_rtps_o(c, insn); return;
    case 0x30: gte_rtpt_o(c, insn); return;
    case 0x12: gte_mvmva_o(c, insn); return;
#else
    case 0x01: gte_rtp_fast(c, insn, 0, 1); return;
    case 0x30: gte_rtp_fast(c, insn, 1, 1); return;
    case 0x12: gte_mvmva_fast(c, insn, 1); return;
#endif
    case 0x06: gte_nclip_fast(c); return;
    default: gte_cmd_slow(c, insn); return;
    }
}

/* Same, for commands whose FLAG result is never read (recomp.py liveness): the slow commands still compute it. */
GT2_INLINE void gte_cmd_nf(PsxCpu *c, uint32_t insn)
{
    switch (insn & 63) {
#ifdef GT2_GTE_OUTLINE
    case 0x01: gte_rtps_nf(c, insn); break;
    case 0x30: gte_rtpt_nf(c, insn); break;
    case 0x12: gte_mvmva_nf(c, insn); break;
#else
    case 0x01: gte_rtp_fast(c, insn, 0, 0); break;
    case 0x30: gte_rtp_fast(c, insn, 1, 0); break;
    case 0x12: gte_mvmva_fast(c, insn, 0); break;
#endif
    default: gte_cmd(c, insn); return;
    }
#ifdef GT2_FLAG_POISON
    c->gte_ctrl[31] = 0xFFFu;               /* impossible FLAG (bits 0-11 are always 0): a read reports it */
#endif
}
