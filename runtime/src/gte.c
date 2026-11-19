/* GTE (COP2) emulation following nocash psx-spx "Geometry Transformation Engine".
 *
 * Simplification: MAC1-3 overflow flags are checked on the final 44-bit sum only (not after every partial
 * addition). Everything else (saturation, flags, UNR division, register quirks) follows the documentation. */
#include <string.h>
#include "rt.h"
#include "ge.h"
#include "prof.h"

#define D (c->gte_data)
#define C (c->gte_ctrl)

/* data register names */
enum { VXY0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ, IR0, IR1, IR2, IR3, SXY0, SXY1, SXY2, SXYP,
       SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1, MAC0, MAC1, MAC2, MAC3, IRGB, ORGB, LZCS, LZCR };
/* control register names */
enum { RT0, RT1, RT2, RT3, RT4, TRX, TRY, TRZ, L0, L1, L2, L3, L4, RBK, GBK, BBK,
       LR0, LR1, LR2, LR3, LR4, RFC, GFC, BFC, OFX, OFY, H, DQA, DQB, ZSF3, ZSF4, FLAG };

static uint32_t g_flag;
uint32_t g_gte_rtpt, g_gte_err[32], g_gte_count[64];   /* debug: RTPT count and error-flag histogram */
uint8_t g_gte_unr[0x101];   /* UNR division table, also used by the inline fast paths (gte_fast.h) */
#define g_unr g_gte_unr

static void unr_init(void)
{
    for (int i = 0; i <= 0x100; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        g_unr[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

void gte_init(void) { unr_init(); }   /* the inline fast paths need the table before the first command */

static inline int16_t lo16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static inline int16_t hi16(uint32_t v) { return (int16_t)(v >> 16); }

/* matrix element access: m = base control register (RT0, L0, LR0) */
static inline int32_t mat(PsxCpu *c, int m, int row, int col)
{
    int idx = row * 3 + col;
    uint32_t w = C[m + idx / 2];
    return (idx & 1) ? hi16(w) : lo16(w);
}

static inline int32_t vx(PsxCpu *c, int n) { return lo16(D[VXY0 + n * 2]); }
static inline int32_t vy(PsxCpu *c, int n) { return hi16(D[VXY0 + n * 2]); }
static inline int32_t vz(PsxCpu *c, int n) { return lo16(D[VZ0 + n * 2]); }

/* Performance: the PSP CPU is 32-bit. A 64-bit product of two 64-bit operands becomes a library call (__muldi3),
 * a 64-bit division even more so. All products here are 32x32->64 (one MIPS mult) via M(), shifts are constant. */
#define M(a, b) ((int64_t)(int32_t)(a) * (int32_t)(b))

/* ---- MAC / IR / colour helpers ---------------------------------------------------------------------- */

static inline int64_t mac_check(int i, int64_t v)
{
    if (v > 0x7FFFFFFFFFFLL) g_flag |= 1u << (31 - i);        /* bits 30,29,28 for MAC1..3 */
    else if (v < -0x80000000000LL) g_flag |= 1u << (28 - i);  /* bits 27,26,25 */
    return v;
}

static inline void set_mac(PsxCpu *c, int i, int64_t v, int shift)
{
    mac_check(i, v);
    D[MAC0 + i] = (uint32_t)(int32_t)(shift ? (v >> 12) : v);
}

static inline void set_mac0(PsxCpu *c, int64_t v)
{
    if (v > 0x7FFFFFFFLL) g_flag |= 1u << 16;
    else if (v < -0x80000000LL) g_flag |= 1u << 15;
    D[MAC0] = (uint32_t)(int32_t)v;
}

static inline void set_ir(PsxCpu *c, int i, int32_t v, int lm)
{
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { v = lo; g_flag |= 1u << (25 - i); }
    else if (v > 0x7FFF) { v = 0x7FFF; g_flag |= 1u << (25 - i); }
    D[IR0 + i] = (uint32_t)v;
}

static inline void set_ir0(PsxCpu *c, int64_t v)
{
    if (v < 0) { v = 0; g_flag |= 1u << 12; }
    else if (v > 0x1000) { v = 0x1000; g_flag |= 1u << 12; }
    D[IR0] = (uint32_t)v;
}

static inline void macs_to_irs(PsxCpu *c, int lm)
{
    set_ir(c, 1, (int32_t)D[MAC1], lm);
    set_ir(c, 2, (int32_t)D[MAC2], lm);
    set_ir(c, 3, (int32_t)D[MAC3], lm);
}

static inline uint32_t col_sat(int i, int32_t v)
{
    if (v < 0) { v = 0; g_flag |= 1u << (22 - i); }
    else if (v > 255) { v = 255; g_flag |= 1u << (22 - i); }
    return (uint32_t)v;
}

/* push MAC1-3 >> 4 as a colour into the RGB FIFO (code byte from RGBC) */
static inline void push_color(PsxCpu *c)
{
    uint32_t r = col_sat(1, (int32_t)D[MAC1] >> 4);
    uint32_t g = col_sat(2, (int32_t)D[MAC2] >> 4);
    uint32_t b = col_sat(3, (int32_t)D[MAC3] >> 4);
    D[RGB0] = D[RGB1];
    D[RGB1] = D[RGB2];
    D[RGB2] = r | (g << 8) | (b << 16) | (D[RGBC] & 0xFF000000u);
}

static inline void push_sz(PsxCpu *c, int64_t v)
{
    if (v < 0) { v = 0; g_flag |= 1u << 18; }
    else if (v > 0xFFFF) { v = 0xFFFF; g_flag |= 1u << 18; }
    D[SZ0] = D[SZ1]; D[SZ1] = D[SZ2]; D[SZ2] = D[SZ3];
    D[SZ3] = (uint32_t)v;
}

static inline void push_sxy(PsxCpu *c, int64_t x, int64_t y)
{
    if (x < -0x400) { x = -0x400; g_flag |= 1u << 14; } else if (x > 0x3FF) { x = 0x3FF; g_flag |= 1u << 14; }
    if (y < -0x400) { y = -0x400; g_flag |= 1u << 13; } else if (y > 0x3FF) { y = 0x3FF; g_flag |= 1u << 13; }
    D[SXY0] = D[SXY1]; D[SXY1] = D[SXY2];
    D[SXY2] = ((uint32_t)x & 0xFFFF) | ((uint32_t)y << 16);
}

static inline int clz16(uint32_t v) { return __builtin_clz(v) - 16; }   /* v != 0 */

/* H / SZ3 with the hardware's unsigned Newton-Raphson division */
static uint32_t divide(uint32_t h, uint32_t sz3)
{
    if (h < sz3 * 2) {
        int z = clz16(sz3);
        uint32_t n = h << z;
        uint32_t d = sz3 << z;
        uint32_t u = g_unr[(d - 0x7FC0) >> 7] + 0x101;
        d = (0x2000080 - d * u) >> 8;
        d = (0x0000080 + d * u) >> 8;
        uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
        return q > 0x1FFFF ? 0x1FFFF : (uint32_t)q;
    }
    g_flag |= 1u << 17;
    return 0x1FFFF;
}

/* ---- commands ------------------------------------------------------------------------------------------ */

static void rtp(PsxCpu *c, int n, int sf, int lm, int last)
{
    int shift = sf ? 12 : 0;
    int32_t x = vx(c, n), y = vy(c, n), z = vz(c, n);
    /* 16x16-bit products fit in 32 bits; only the sums with TR<<12 need 64 */
    int64_t m1 = ((int64_t)(int32_t)C[TRX] << 12) + M(mat(c, RT0, 0, 0), x) + M(mat(c, RT0, 0, 1), y) + M(mat(c, RT0, 0, 2), z);
    int64_t m2 = ((int64_t)(int32_t)C[TRY] << 12) + M(mat(c, RT0, 1, 0), x) + M(mat(c, RT0, 1, 1), y) + M(mat(c, RT0, 1, 2), z);
    int64_t m3 = ((int64_t)(int32_t)C[TRZ] << 12) + M(mat(c, RT0, 2, 0), x) + M(mat(c, RT0, 2, 1), y) + M(mat(c, RT0, 2, 2), z);
    set_mac(c, 1, m1, shift);
    set_mac(c, 2, m2, shift);
    set_mac(c, 3, m3, shift);
    set_ir(c, 1, (int32_t)D[MAC1], lm);
    set_ir(c, 2, (int32_t)D[MAC2], lm);
    /* IR3 quirk: saturation flag from MAC3 >> 12 when sf = 0 */
    {
        int32_t v = (int32_t)D[MAC3];
        int32_t lo = lm ? 0 : -0x8000;
        int32_t sat = v < lo ? lo : v > 0x7FFF ? 0x7FFF : v;
        int32_t chk = (int32_t)(m3 >> 12);
        if (chk < -0x8000 || chk > 0x7FFF) g_flag |= 1u << 22;
        D[IR3] = (uint32_t)sat;
    }
    push_sz(c, m3 >> 12);
    uint32_t q = divide(C[H] & 0xFFFF, D[SZ3]);
    int64_t px = M(q, (int16_t)D[IR1]);
    int64_t sx = px + (int32_t)C[OFX];
    int64_t sy = M(q, (int16_t)D[IR2]) + (int32_t)C[OFY];
    set_mac0(c, sx); set_mac0(c, sy);
    push_sxy(c, sx >> 16, sy >> 16);
    if (g_widescreen && g_hires)
        ge_mark_3d_precise(D[SXY2], (float)(int32_t)sx * (1.0f / 65536.0f), (float)(int32_t)sy * (1.0f / 65536.0f),
                           (float)(int32_t)D[SZ3]);
    if (last) {
        int64_t p = M(q, (int16_t)C[DQA]) + (int32_t)C[DQB];
        set_mac0(c, p);
        set_ir0(c, p >> 12);
    }
}

/* MAC = BK*0x1000 + LR*IR (light colour); used by NC* commands */
static void mul_mat_vec(PsxCpu *c, int m, int32_t v1, int32_t v2, int32_t v3, int32_t t1, int32_t t2, int32_t t3,
                        int sf, int lm)
{
    int shift = sf ? 12 : 0;
    set_mac(c, 1, ((int64_t)t1 << 12) + M(mat(c, m, 0, 0), v1) + M(mat(c, m, 0, 1), v2) + M(mat(c, m, 0, 2), v3), shift);
    set_mac(c, 2, ((int64_t)t2 << 12) + M(mat(c, m, 1, 0), v1) + M(mat(c, m, 1, 1), v2) + M(mat(c, m, 1, 2), v3), shift);
    set_mac(c, 3, ((int64_t)t3 << 12) + M(mat(c, m, 2, 0), v1) + M(mat(c, m, 2, 1), v2) + M(mat(c, m, 2, 2), v3), shift);
    macs_to_irs(c, lm);
}

static inline int32_t ir(PsxCpu *c, int i) { return (int16_t)D[IR0 + i]; }

/* depth cue: MAC = MAC + (FC - MAC) * IR0, starting from MAC1-3 in "<<12" units */
static void depth_cue_from(PsxCpu *c, int64_t m1, int64_t m2, int64_t m3, int sf, int lm)
{
    int shift = sf ? 12 : 0;
    int32_t i0 = ir(c, 0);
    /* (FC<<12 - MAC) saturated through IR with lm=0, then MAC += IR*IR0 */
    set_mac(c, 1, ((int64_t)(int32_t)C[RFC] << 12) - m1, shift);
    set_mac(c, 2, ((int64_t)(int32_t)C[GFC] << 12) - m2, shift);
    set_mac(c, 3, ((int64_t)(int32_t)C[BFC] << 12) - m3, shift);
    macs_to_irs(c, 0);
    set_mac(c, 1, M(ir(c, 1), i0) + m1, shift);
    set_mac(c, 2, M(ir(c, 2), i0) + m2, shift);
    set_mac(c, 3, M(ir(c, 3), i0) + m3, shift);
    macs_to_irs(c, lm);
}

static void ncx(PsxCpu *c, int n, int sf, int lm, int mode)   /* mode 0 NCS, 1 NCCS, 2 NCDS */
{
    mul_mat_vec(c, L0, vx(c, n), vy(c, n), vz(c, n), 0, 0, 0, sf, lm);
    mul_mat_vec(c, LR0, ir(c, 1), ir(c, 2), ir(c, 3), (int32_t)C[RBK], (int32_t)C[GBK], (int32_t)C[BBK], sf, lm);
    if (mode) {
        int32_t r = D[RGBC] & 0xFF, g = (D[RGBC] >> 8) & 0xFF, b = (D[RGBC] >> 16) & 0xFF;
        int32_t m1 = (r * ir(c, 1)) << 4, m2 = (g * ir(c, 2)) << 4, m3 = (b * ir(c, 3)) << 4;
        if (mode == 2) depth_cue_from(c, m1, m2, m3, sf, lm);
        else {
            int shift = sf ? 12 : 0;
            set_mac(c, 1, m1, shift); set_mac(c, 2, m2, shift); set_mac(c, 3, m3, shift);
            macs_to_irs(c, lm);
        }
    }
    push_color(c);
}

static void gte_cmd_impl(PsxCpu *c, uint32_t insn);

/* RT_EXTRA=-DGT2_GTE_PHASE: the outlined commands count as "GTE" in the sampling profiler (prof.c) */
#ifdef GT2_GTE_PHASE
#define GTE_PH(x) do { PHASE_BEGIN(PH_GTE); g_gte_count[insn & 63]++; x; PHASE_END(); } while (0)
#else
#define GTE_PH(x) x
#endif
void gte_rtps_o(PsxCpu *c, uint32_t insn) { GTE_PH(gte_rtp_fast(c, insn, 0, 1)); }
void gte_rtpt_o(PsxCpu *c, uint32_t insn) { GTE_PH(gte_rtp_fast(c, insn, 1, 1)); }
void gte_mvmva_o(PsxCpu *c, uint32_t insn) { GTE_PH(gte_mvmva_fast(c, insn, 1)); }
void gte_rtps_nf(PsxCpu *c, uint32_t insn) { GTE_PH(gte_rtp_fast(c, insn, 0, 0)); }
void gte_rtpt_nf(PsxCpu *c, uint32_t insn) { GTE_PH(gte_rtp_fast(c, insn, 1, 0)); }
void gte_mvmva_nf(PsxCpu *c, uint32_t insn) { GTE_PH(gte_mvmva_fast(c, insn, 0)); }
#define GTE_DEF_V(k)     void gte_mv_##k(PsxCpu *c) { const uint32_t insn = 0x##k##u; GTE_PH(gte_mvmva_fast(c, insn, 1)); }     void gte_mv_##k##_nf(PsxCpu *c) { const uint32_t insn = 0x##k##u; GTE_PH(gte_mvmva_fast(c, insn, 0)); }
GTE_MVMVA_VARIANTS(GTE_DEF_V)
void gte_rtps_k(PsxCpu *c) { const uint32_t insn = 0x4a180001u; GTE_PH(gte_rtp_fast(c, insn, 0, 1)); }
void gte_rtps_k_nf(PsxCpu *c) { const uint32_t insn = 0x4a180001u; GTE_PH(gte_rtp_fast(c, insn, 0, 0)); }
void gte_rtpt_k(PsxCpu *c) { const uint32_t insn = 0x4a280030u; GTE_PH(gte_rtp_fast(c, insn, 1, 1)); }
void gte_rtpt_k_nf(PsxCpu *c) { const uint32_t insn = 0x4a280030u; GTE_PH(gte_rtp_fast(c, insn, 1, 0)); }

void gte_flag_poison_hit(uint32_t unused)
{
    static int n;
    (void)unused;
    if (n++ < 50)
        logf_("GTE FLAG read after a FLAG-free command (recomp flagflow assumption broken) in %08x\n",
              (unsigned)g_trace[(g_trace_pos - 1) & (TRACE_SIZE - 1)]);
}

/* settings.txt membench=2: self-test of the sf = 1 fast cores against the reference paths (random registers, incl.
 * extreme values), then timing of the fast paths (ns per command, with and without FLAG) */
static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng ^ (g_rng >> 15); }
static uint32_t rnd_val(void)
{
    uint32_t r = rnd();
    switch (rnd() & 7) {
    case 0: return r;                                   /* anything */
    case 1: return 0x7FFF7FFFu ^ (r & 0x00030003u);     /* near the 16-bit maximum */
    case 2: return 0x80008000u | (r & 0x00030003u);     /* near the 16-bit minimum */
    default: return r & 0x0FFF0FFFu;                    /* typical */
    }
}
static void gte_selftest(void)
{
    static PsxCpu a, b;
    static const uint32_t insns[] = {0x4a180001u, 0x4a280030u, 0x4a180401u, 0x4a280430u, 0x4a498012u, 0x4a4a6012u,
                                     0x4a486012u, 0x4a4c6012u, 0x4a40e012u, 0x4a4be012u, 0x4a49e412u, 0x4a41a012u};
    int bad = 0;
    for (int it = 0; it < 200000 && bad < 8; it++) {
        for (int i = 0; i < 32; i++) { a.gte_data[i] = rnd_val(); a.gte_ctrl[i] = rnd_val(); }
        if (rnd() & 1) for (int i = 5; i < 8; i++) a.gte_ctrl[i] = rnd();          /* translations: full 32 bit */
        if (rnd() & 1) { a.gte_ctrl[24] = rnd(); a.gte_ctrl[25] = rnd(); a.gte_ctrl[28] = rnd(); }
        a.gte_ctrl[26] = rnd() & 0xFFFF;
        b = a;
        uint32_t insn = insns[rnd() % (sizeof insns / sizeof insns[0])];
        if ((insn & 63) == 0x12) { gte_mvmva_fast2(&a, insn, 1, 1); gte_mvmva_fast2(&b, insn, 1, 0); }
        else { gte_rtp_fast(&a, insn, (insn & 63) == 0x30, 1); gte_rtp_ref(&b, insn, (insn & 63) == 0x30, 1); }
        if (memcmp(a.gte_data, b.gte_data, sizeof a.gte_data) || a.gte_ctrl[31] != b.gte_ctrl[31]) {
            bad++;
            logf_("gte selftest MISMATCH insn %08x it %d:", (unsigned)insn, it);
            for (int i = 0; i < 32; i++)
                if (a.gte_data[i] != b.gte_data[i]) logf_(" d%d %08x/%08x", i, (unsigned)a.gte_data[i], (unsigned)b.gte_data[i]);
            logf_(" flag %08x/%08x\n", (unsigned)a.gte_ctrl[31], (unsigned)b.gte_ctrl[31]);
        }
    }
    logf_("gte selftest: %d mismatches\n", bad);
}

static void __attribute__((noinline)) gte_nop_b(PsxCpu *c, uint32_t insn) { c->gte_ctrl[31] = insn; }
static void gte_mvmva_k_b(PsxCpu *c, uint32_t insn) { (void)insn; gte_mv_4a498012_nf(c); }
static void gte_rtps_k_b(PsxCpu *c, uint32_t insn) { (void)insn; gte_rtps_k_nf(c); }
static void __attribute__((noinline)) gte_div_b(PsxCpu *c, uint32_t insn)
{
    uint32_t f = 0;
    c->gte_data[8] = gtef_divide(c->gte_ctrl[26] & 0xFFFF, (c->gte_data[1] & 0x3FF) + 400, &f);
    (void)insn;
}

void gte_bench(void)
{
    gte_selftest();
    static PsxCpu cc;
    PsxCpu *c = &cc;
    static const uint32_t m[5] = {0x00000F80u, 0x02000100u, 0x0F00FE00u, 0x00400010u, 0x00000FF0u};
    for (int i = 0; i < 5; i++) c->gte_ctrl[i] = m[i];
    c->gte_ctrl[5] = 100; c->gte_ctrl[6] = -50; c->gte_ctrl[7] = 3000;
    c->gte_ctrl[24] = 160 << 16; c->gte_ctrl[25] = 120 << 16; c->gte_ctrl[26] = 300;
    c->gte_ctrl[27] = (uint32_t)-100; c->gte_ctrl[28] = 0x1400000;
    const int N = 200000;
    void (*const fns[10])(PsxCpu *, uint32_t) = {gte_rtps_o, gte_rtps_nf, gte_rtpt_o, gte_rtpt_nf, gte_mvmva_o, gte_mvmva_nf, gte_nop_b, gte_div_b, gte_mvmva_k_b, gte_rtps_k_b};
    static const char *const names[10] = {"rtps", "rtps nf", "rtpt", "rtpt nf", "mvmva", "mvmva nf", "call only", "divide", "mvmva nf const", "rtps nf const"};
    const uint32_t insn[10] = {0x4a180001u, 0x4a180001u, 0x4a280030u, 0x4a280030u, 0x4a498012u, 0x4a498012u, 0, 0, 0, 0};
    for (int k = 0; k < 10; k++) {
        uint64_t t0 = now_us();
        for (int i = 0; i < N; i++) {
            c->gte_data[0] = (uint32_t)((i & 255) | ((i & 511) << 16)); c->gte_data[1] = (uint32_t)(i & 1023);
            c->gte_data[2] = c->gte_data[0] ^ 0x00400040u; c->gte_data[3] = 500;
            c->gte_data[4] = c->gte_data[0] ^ 0x00200010u; c->gte_data[5] = 700;
            fns[k](c, insn[k]);
        }
        uint64_t t1 = now_us();
        logf_("gtebench: %s %u ns\n", names[k], (unsigned)((t1 - t0) * 1000 / N));
    }
}

void gte_cmd_slow(PsxCpu *c, uint32_t insn)
{
    PHASE_BEGIN(PH_GTE);
    g_gte_count[insn & 63]++;
    gte_cmd_impl(c, insn);
    PHASE_END();
}

static void gte_cmd_impl(PsxCpu *c, uint32_t insn)
{
    static int init;
    if (!init) { unr_init(); init = 1; }
    int sf = (insn >> 19) & 1, lm = (insn >> 10) & 1;
    int shift = sf ? 12 : 0;
    g_flag = 0;
    switch (insn & 63) {
    case 0x01: rtp(c, 0, sf, lm, 1); break;                              /* RTPS */
    case 0x30:                                                           /* RTPT */
        rtp(c, 0, sf, lm, 0); rtp(c, 1, sf, lm, 0); rtp(c, 2, sf, lm, 1);
        break;
    case 0x06: {                                                         /* NCLIP */
        int32_t x0 = lo16(D[SXY0]), y0 = hi16(D[SXY0]), x1 = lo16(D[SXY1]), y1 = hi16(D[SXY1]);
        int32_t x2 = lo16(D[SXY2]), y2 = hi16(D[SXY2]);
        set_mac0(c, M(x0, y1) + M(x1, y2) + M(x2, y0) - M(x0, y2) - M(x1, y0) - M(x2, y1));
        break;
    }
    case 0x0C: {                                                         /* OP (outer product) */
        int32_t d1 = lo16(C[RT0]), d2 = lo16(C[RT2]), d3 = lo16(C[RT4]);
        int32_t i1 = ir(c, 1), i2 = ir(c, 2), i3 = ir(c, 3);
        set_mac(c, 1, M(i3, d2) - M(i2, d3), shift);
        set_mac(c, 2, M(i1, d3) - M(i3, d1), shift);
        set_mac(c, 3, M(i2, d1) - M(i1, d2), shift);
        macs_to_irs(c, lm);
        break;
    }
    case 0x10: {                                                         /* DPCS */
        int64_t r = D[RGBC] & 0xFF, g = (D[RGBC] >> 8) & 0xFF, b = (D[RGBC] >> 16) & 0xFF;
        depth_cue_from(c, r << 16, g << 16, b << 16, sf, lm);
        push_color(c);
        break;
    }
    case 0x2A:                                                           /* DPCT */
        for (int i = 0; i < 3; i++) {
            uint32_t col = D[RGB0];
            int64_t r = col & 0xFF, g = (col >> 8) & 0xFF, b = (col >> 16) & 0xFF;
            depth_cue_from(c, r << 16, g << 16, b << 16, sf, lm);
            push_color(c);
        }
        break;
    case 0x11:                                                           /* INTPL */
        depth_cue_from(c, ir(c, 1) << 12, ir(c, 2) << 12, ir(c, 3) << 12, sf, lm);
        push_color(c);
        break;
    case 0x12: {                                                         /* MVMVA */
        int mx = (insn >> 17) & 3, vv = (insn >> 15) & 3, tv = (insn >> 13) & 3;
        int32_t v1, v2, v3, t1 = 0, t2 = 0, t3 = 0;
        if (vv < 3) { v1 = vx(c, vv); v2 = vy(c, vv); v3 = vz(c, vv); }
        else { v1 = ir(c, 1); v2 = ir(c, 2); v3 = ir(c, 3); }
        if (tv == 0) { t1 = (int32_t)C[TRX]; t2 = (int32_t)C[TRY]; t3 = (int32_t)C[TRZ]; }
        else if (tv == 1) { t1 = (int32_t)C[RBK]; t2 = (int32_t)C[GBK]; t3 = (int32_t)C[BBK]; }
        else if (tv == 2) { t1 = (int32_t)C[RFC]; t2 = (int32_t)C[GFC]; t3 = (int32_t)C[BFC]; }
        if (mx == 3) {                                                   /* garbage matrix (hardware bug) */
            int32_t r = (D[RGBC] & 0xFF) << 4, rt13 = lo16(C[RT1]), rt22 = lo16(C[RT2]);
            set_mac(c, 1, ((int64_t)t1 << 12) - M(r, v1) + M(r, v2) + M(ir(c, 0), v3), shift);
            set_mac(c, 2, ((int64_t)t2 << 12) + M(rt13, v1 + v2 + v3), shift);
            set_mac(c, 3, ((int64_t)t3 << 12) + M(rt22, v1 + v2 + v3), shift);
            macs_to_irs(c, lm);
        } else {
            mul_mat_vec(c, mx == 0 ? RT0 : mx == 1 ? L0 : LR0, v1, v2, v3, t1, t2, t3, sf, lm);
        }
        break;
    }
    case 0x13: ncx(c, 0, sf, lm, 2); break;                              /* NCDS */
    case 0x16: ncx(c, 0, sf, lm, 2); ncx(c, 1, sf, lm, 2); ncx(c, 2, sf, lm, 2); break;   /* NCDT */
    case 0x1B: ncx(c, 0, sf, lm, 1); break;                              /* NCCS */
    case 0x3F: ncx(c, 0, sf, lm, 1); ncx(c, 1, sf, lm, 1); ncx(c, 2, sf, lm, 1); break;   /* NCCT */
    case 0x1E: ncx(c, 0, sf, lm, 0); break;                              /* NCS */
    case 0x20: ncx(c, 0, sf, lm, 0); ncx(c, 1, sf, lm, 0); ncx(c, 2, sf, lm, 0); break;   /* NCT */
    case 0x14: case 0x1C: {                                              /* CDP / CC */
        mul_mat_vec(c, LR0, ir(c, 1), ir(c, 2), ir(c, 3), (int32_t)C[RBK], (int32_t)C[GBK], (int32_t)C[BBK], sf, lm);
        int32_t r = D[RGBC] & 0xFF, g = (D[RGBC] >> 8) & 0xFF, b = (D[RGBC] >> 16) & 0xFF;
        int32_t m1 = (r * ir(c, 1)) << 4, m2 = (g * ir(c, 2)) << 4, m3 = (b * ir(c, 3)) << 4;
        if ((insn & 63) == 0x14) depth_cue_from(c, m1, m2, m3, sf, lm);
        else { set_mac(c, 1, m1, shift); set_mac(c, 2, m2, shift); set_mac(c, 3, m3, shift); macs_to_irs(c, lm); }
        push_color(c);
        break;
    }
    case 0x28:                                                           /* SQR */
        set_mac(c, 1, M(ir(c, 1), ir(c, 1)), shift);
        set_mac(c, 2, M(ir(c, 2), ir(c, 2)), shift);
        set_mac(c, 3, M(ir(c, 3), ir(c, 3)), shift);
        macs_to_irs(c, lm);
        break;
    case 0x29: {                                                         /* DCPL */
        int32_t r = D[RGBC] & 0xFF, g = (D[RGBC] >> 8) & 0xFF, b = (D[RGBC] >> 16) & 0xFF;
        depth_cue_from(c, (r * ir(c, 1)) << 4, (g * ir(c, 2)) << 4, (b * ir(c, 3)) << 4, sf, lm);
        push_color(c);
        break;
    }
    case 0x2D: {                                                         /* AVSZ3 */
        int64_t v = M((int16_t)C[ZSF3], D[SZ1] + D[SZ2] + D[SZ3]);
        set_mac0(c, v);
        int64_t o = v >> 12;
        if (o < 0) { o = 0; g_flag |= 1u << 18; } else if (o > 0xFFFF) { o = 0xFFFF; g_flag |= 1u << 18; }
        D[OTZ] = (uint32_t)o;
        break;
    }
    case 0x2E: {                                                         /* AVSZ4 */
        int64_t v = M((int16_t)C[ZSF4], D[SZ0] + D[SZ1] + D[SZ2] + D[SZ3]);
        set_mac0(c, v);
        int64_t o = v >> 12;
        if (o < 0) { o = 0; g_flag |= 1u << 18; } else if (o > 0xFFFF) { o = 0xFFFF; g_flag |= 1u << 18; }
        D[OTZ] = (uint32_t)o;
        break;
    }
    case 0x3D:                                                           /* GPF */
        set_mac(c, 1, M(ir(c, 0), ir(c, 1)), shift);
        set_mac(c, 2, M(ir(c, 0), ir(c, 2)), shift);
        set_mac(c, 3, M(ir(c, 0), ir(c, 3)), shift);
        macs_to_irs(c, lm);
        push_color(c);
        break;
    case 0x3E:                                                           /* GPL */
        set_mac(c, 1, (shift ? ((int64_t)(int32_t)D[MAC1] << 12) : (int64_t)(int32_t)D[MAC1]) + M(ir(c, 0), ir(c, 1)), shift);
        set_mac(c, 2, (shift ? ((int64_t)(int32_t)D[MAC2] << 12) : (int64_t)(int32_t)D[MAC2]) + M(ir(c, 0), ir(c, 2)), shift);
        set_mac(c, 3, (shift ? ((int64_t)(int32_t)D[MAC3] << 12) : (int64_t)(int32_t)D[MAC3]) + M(ir(c, 0), ir(c, 3)), shift);
        macs_to_irs(c, lm);
        push_color(c);
        break;
    default: {
        static uint8_t seen[64];
        if (!seen[insn & 63]) { seen[insn & 63] = 1; logf_("GTE: unknown command %02x\n", (unsigned)(insn & 63)); }
        break;
    }
    }
    if (g_flag & 0x7F87E000u) g_flag |= 0x80000000u;
    C[FLAG] = g_flag;
}

/* ---- register transfers ---------------------------------------------------------------------------------- */

uint32_t gte_read_data_slow(PsxCpu *c, int reg)
{
    switch (reg) {
    case VZ0: case VZ1: case VZ2: case IR0: case IR1: case IR2: case IR3:
        return (uint32_t)(int32_t)(int16_t)D[reg];
    case OTZ: case SZ0: case SZ1: case SZ2: case SZ3:
        return D[reg] & 0xFFFF;
    case SXYP:
        return D[SXY2];
    case IRGB: case ORGB: {
        int32_t r = (int16_t)D[IR1] >> 7, g = (int16_t)D[IR2] >> 7, b = (int16_t)D[IR3] >> 7;
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
        return (uint32_t)(r | (g << 5) | (b << 10));
    }
    }
    return D[reg];
}

void gte_write_data_slow(PsxCpu *c, int reg, uint32_t v)
{
    switch (reg) {
    case SXYP:
        D[SXY0] = D[SXY1]; D[SXY1] = D[SXY2]; D[SXY2] = v;
        return;
    case IRGB:
        D[IR1] = (v & 0x1F) << 7;
        D[IR2] = ((v >> 5) & 0x1F) << 7;
        D[IR3] = ((v >> 10) & 0x1F) << 7;
        D[IRGB] = v & 0x7FFF;
        return;
    case ORGB:
        return;
    case LZCS: {
        D[LZCS] = v;
        uint32_t x = (v & 0x80000000u) ? ~v : v;
        int n = 0;
        while (n < 32 && !(x & (0x80000000u >> n))) n++;
        D[LZCR] = (uint32_t)n;
        return;
    }
    case LZCR:
        return;
    }
    D[reg] = v;
}

uint32_t gte_read_ctrl_slow(PsxCpu *c, int reg)
{
    switch (reg) {
    case RT4: case L4: case LR4: case H: case DQA: case ZSF3: case ZSF4:
        return (uint32_t)(int32_t)(int16_t)C[reg];
    }
    return C[reg];
}

void gte_write_ctrl_slow(PsxCpu *c, int reg, uint32_t v)
{
    if (reg == FLAG) {
        v &= 0x7FFFF000u;
        if (v & 0x7F87E000u) v |= 0x80000000u;
    }
    C[reg] = v;
}

/* For the widescreen near-plane clipper: model -> view space with the current rotation/translation (sf = 1),
 * and the projection parameters (H, OFX/OFY in 16.16). */
void gte_view_space(PsxCpu *c, int x, int y, int z, int32_t out[3])
{
    for (int i = 0; i < 3; i++) {
        int64_t m = ((int64_t)(int32_t)C[TRX + i] << 12) + M(mat(c, RT0, i, 0), x) + M(mat(c, RT0, i, 1), y) +
                    M(mat(c, RT0, i, 2), z);
        out[i] = (int32_t)(m >> 12);
    }
}

void gte_screen_params(PsxCpu *c, int32_t *h, int32_t *ofx, int32_t *ofy)
{
    *h = (int32_t)(C[H] & 0xFFFF);
    *ofx = (int32_t)C[OFX];
    *ofy = (int32_t)C[OFY];
}
