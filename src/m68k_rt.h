/*
 * m68k_rt.h -- runtime support for statically translated 68000 code.
 *
 * The translator emits plain C that manipulates the globals below.  Flags are
 * kept explicitly (X N Z V C as separate bytes); a later pass removes dead flag
 * writes.  Memory goes through the rd and wr helpers, which handle ROM, RAM and the Mega Drive
 * hardware window.  Interrupts are delivered at CHECK() points (calls, backward
 * branches, and any other place the generator puts one).
 */
#ifndef M68K_RT_H
#define M68K_RT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* ---------------------------------------------------------------- CPU state */
extern u32 D[8];
extern u32 A[8];            /* A[7] is the active stack pointer */
extern u32 m68k_other_sp;   /* the inactive one (USP while in supervisor mode) */
extern u8  fX, fN, fZ, fV, fC;
extern u8  m68k_sr_hi;      /* T . S . . I2 I1 I0 -- the system byte */

#define SR_S      0x20
#define IPL()     ((m68k_sr_hi) & 7)

static inline u32 m68k_get_ccr(void) {
    return (fX << 4) | (fN << 3) | (fZ << 2) | (fV << 1) | fC;
}
static inline void m68k_set_ccr(u32 v) {
    fX = (v >> 4) & 1; fN = (v >> 3) & 1; fZ = (v >> 2) & 1; fV = (v >> 1) & 1; fC = v & 1;
}
static inline u32 m68k_get_sr(void) { return ((u32)m68k_sr_hi << 8) | m68k_get_ccr(); }
static inline void m68k_set_sr(u32 v) {
    u8 hi = (v >> 8) & 0xA7;
    if ((hi ^ m68k_sr_hi) & SR_S) {           /* S toggled: swap stack pointers */
        u32 t = A[7]; A[7] = m68k_other_sp; m68k_other_sp = t;
    }
    m68k_sr_hi = hi;
    m68k_set_ccr(v);
}

/* -------------------------------------------------------------- time/events */
extern s64 m68k_cycle;          /* cycles consumed so far (68000 clock) */
extern s64 m68k_next_event;     /* hardware wants attention at this cycle */
extern int m68k_irq_level;      /* highest pending interrupt level, 0 = none */
void hw_events(void);           /* hardware: catch up to m68k_cycle, may raise IRQs */
void m68k_service_irq(void);    /* deliver pending interrupts above the mask */

#define CYC(n) (m68k_cycle += (n))
#define CHECK() do { \
        if (m68k_cycle >= m68k_next_event) hw_events(); \
        if (m68k_irq_level > IPL()) m68k_service_irq(); \
    } while (0)

/* instruction-boundary poll: service hardware events and interrupts exactly where a real 68000 would */
#define POLL() do { \
        if (m68k_cycle >= m68k_next_event || m68k_irq_level > IPL()) { \
            if (m68k_cycle >= m68k_next_event) hw_events(); \
            if (m68k_irq_level > IPL()) m68k_service_irq(); } \
    } while (0)

#ifdef S3D_TRACE_CALLS
extern u32 trc_ring[8192]; extern s64 trc_cyc[8192]; extern unsigned trc_pos;
#define TRC(a) (trc_cyc[trc_pos & 8191] = m68k_cycle, trc_ring[trc_pos++ & 8191] = (a))
#elif defined(S3D_FCOUNT)
extern unsigned dbg_fc[0x80000];
#define TRC(a) (dbg_fc[((a) & 0xFFFFF) >> 1]++)
#else
#define TRC(a) ((void)0)
#endif

/* ----------------------------------------------------------------- memory */
extern u8  rom_data[0x400000];  /* the player's ROM (loaded at start-up) */
extern u8  ram_data[0x10000];   /* work RAM $FF0000-$FFFFFF (mirrored through $E00000) */

/* 16:9 support */
extern int wide_px, wide_fill, wide_dma_hi, wide_cm; extern u32 wide_a6s; extern u8 wide_scratch[128]; void wide_latch(void); void wide_ingame_tick(void); void wide_clear(void); void wide_ss_start(void); void wide_ss_end(void); void wide_hud_fix(u32 end);

/* hardware window: everything that is not ROM or work RAM */
u8   hw_rd8(u32 a);
u16  hw_rd16(u32 a);
u32  hw_rd32(u32 a);
void hw_wr8(u32 a, u8 v);
void hw_wr16(u32 a, u16 v);
void hw_wr32(u32 a, u32 v);

static inline u8 rd8(u32 a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) return rom_data[a];
    if (a >= 0xE00000) return ram_data[a & 0xFFFF];
    return hw_rd8(a);
}
#ifdef S3D_CAMHACK
extern int dbg_cam_on; extern u16 dbg_cam_x, dbg_cam_y;
#define CAMHACK16(a) if (dbg_cam_on && (((a) & 0xFFFF) == 0x896 || ((a) & 0xFFFF) == 0x89A) && (a) >= 0xE00000) return dbg_cam_x; if (dbg_cam_on && (((a) & 0xFFFF) == 0x898 || ((a) & 0xFFFF) == 0x89C) && (a) >= 0xE00000) return dbg_cam_y;
#else
#define CAMHACK16(a)
#endif
static inline u16 rd16(u32 a) {
    a &= 0xFFFFFF;
    CAMHACK16(a)
    if (a < 0x400000) return (u16)((rom_data[a] << 8) | rom_data[a + 1]);
    if (a >= 0xE00000) { a &= 0xFFFF; return (u16)((ram_data[a] << 8) | ram_data[(a + 1) & 0xFFFF]); }
    return hw_rd16(a);
}
static inline u32 rd32(u32 a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) return ((u32)rom_data[a] << 24) | ((u32)rom_data[a + 1] << 16) | ((u32)rom_data[a + 2] << 8) | rom_data[a + 3];
    if (a >= 0xE00000) {
        a &= 0xFFFF;
        return ((u32)ram_data[a] << 24) | ((u32)ram_data[(a + 1) & 0xFFFF] << 16) |
               ((u32)ram_data[(a + 2) & 0xFFFF] << 8) | ram_data[(a + 3) & 0xFFFF];
    }
    return hw_rd32(a);
}
#ifdef S3D_WATCH
extern u32 watch_lo, watch_hi; void watch_hit(u32 a, u32 v, int sz);
#define WCHK(a, v, sz) do { u32 a_ = (a) & 0xFFFF; if (a_ >= watch_lo && a_ <= watch_hi && ((a) & 0xFFFFFF) >= 0xE00000) watch_hit(a, v, sz); } while (0)
#else
#define WCHK(a, v, sz) ((void)0)
#endif
static inline void wr8(u32 a, u8 v) {
    WCHK(a, v, 1);
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { ram_data[a & 0xFFFF] = v; return; }
    if (a < 0x400000) return;                /* ROM: ignore */
    hw_wr8(a, v);
}
static inline void wr16(u32 a, u16 v) {
    WCHK(a, v, 2);
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { a &= 0xFFFF; ram_data[a] = v >> 8; ram_data[(a + 1) & 0xFFFF] = (u8)v; return; }
    if (a < 0x400000) return;
    hw_wr16(a, v);
}
static inline void wr32(u32 a, u32 v) {
    WCHK(a, v, 4);
    a &= 0xFFFFFF;
    if (a >= 0xE00000) {
        a &= 0xFFFF;
        ram_data[a] = v >> 24; ram_data[(a + 1) & 0xFFFF] = v >> 16;
        ram_data[(a + 2) & 0xFFFF] = v >> 8; ram_data[(a + 3) & 0xFFFF] = (u8)v;
        return;
    }
    if (a < 0x400000) return;
    hw_wr32(a, v);
}

/* stack helpers */
static inline void push16(u16 v) { A[7] -= 2; wr16(A[7], v); }
static inline void push32(u32 v) { A[7] -= 4; wr32(A[7], v); }
static inline u16  pop16(void)   { u16 v = rd16(A[7]); A[7] += 2; return v; }
static inline u32  pop32(void)   { u32 v = rd32(A[7]); A[7] += 4; return v; }

/* ------------------------------------------------------------ sign helpers */
#define SX8(v)  ((u32)(s32)(s8)(v))
#define SX16(v) ((u32)(s32)(s16)(v))

/* register partial writes */
#define SETB(r, v) ((r) = ((r) & 0xFFFFFF00u) | ((u32)(v) & 0xFFu))
#define SETW(r, v) ((r) = ((r) & 0xFFFF0000u) | ((u32)(v) & 0xFFFFu))

/* ------------------------------------------------------------- conditions */
#define CC_T   1
#define CC_F   0
#define CC_HI  (!fC && !fZ)
#define CC_LS  (fC || fZ)
#define CC_CC  (!fC)
#define CC_CS  (fC)
#define CC_NE  (!fZ)
#define CC_EQ  (fZ)
#define CC_VC  (!fV)
#define CC_VS  (fV)
#define CC_PL  (!fN)
#define CC_MI  (fN)
#define CC_GE  (fN == fV)
#define CC_LT  (fN != fV)
#define CC_GT  (!fZ && (fN == fV))
#define CC_LE  (fZ || (fN != fV))

/* ------------------------------------------------------------ ALU templates */
/* S = suffix (b,w,l), M = value mask, G = sign bit.  All values are kept
 * zero-extended in u32 so the compiler never meets signed overflow. */
#define M68K_ALU(S, M, G) \
static inline u32 add_##S(u32 s, u32 d) { \
    u32 r = (d + s) & (M); \
    u32 sm = s & (G), dm = d & (G), rm = r & (G); \
    fC = fX = ((sm & dm) | (~rm & dm) | (~rm & sm)) ? 1 : 0; \
    fV = ((sm & dm & ~rm) | (~sm & ~dm & rm)) ? 1 : 0; \
    fN = rm ? 1 : 0; fZ = (r == 0); return r; } \
static inline u32 sub_##S(u32 s, u32 d) { \
    u32 r = (d - s) & (M); \
    u32 sm = s & (G), dm = d & (G), rm = r & (G); \
    fC = fX = ((sm & ~dm) | (rm & ~dm) | (sm & rm)) ? 1 : 0; \
    fV = ((~sm & dm & ~rm) | (sm & ~dm & rm)) ? 1 : 0; \
    fN = rm ? 1 : 0; fZ = (r == 0); return r; } \
static inline void cmp_##S(u32 s, u32 d) { \
    u32 r = (d - s) & (M); \
    u32 sm = s & (G), dm = d & (G), rm = r & (G); \
    fC = ((sm & ~dm) | (rm & ~dm) | (sm & rm)) ? 1 : 0; \
    fV = ((~sm & dm & ~rm) | (sm & ~dm & rm)) ? 1 : 0; \
    fN = rm ? 1 : 0; fZ = (r == 0); } \
static inline u32 addx_##S(u32 s, u32 d) { \
    u32 r = (d + s + fX) & (M); \
    u32 sm = s & (G), dm = d & (G), rm = r & (G); \
    fC = fX = ((sm & dm) | (~rm & dm) | (~rm & sm)) ? 1 : 0; \
    fV = ((sm & dm & ~rm) | (~sm & ~dm & rm)) ? 1 : 0; \
    fN = rm ? 1 : 0; if (r) fZ = 0; return r; } \
static inline u32 subx_##S(u32 s, u32 d) { \
    u32 r = (d - s - fX) & (M); \
    u32 sm = s & (G), dm = d & (G), rm = r & (G); \
    fC = fX = ((sm & ~dm) | (rm & ~dm) | (sm & rm)) ? 1 : 0; \
    fV = ((~sm & dm & ~rm) | (sm & ~dm & rm)) ? 1 : 0; \
    fN = rm ? 1 : 0; if (r) fZ = 0; return r; } \
static inline u32 neg_##S(u32 d) { \
    u32 r = (0 - d) & (M); \
    fC = fX = (d != 0); fV = ((d & r) & (G)) ? 1 : 0; \
    fN = (r & (G)) ? 1 : 0; fZ = (r == 0); return r; } \
static inline u32 negx_##S(u32 d) { \
    u32 r = (0 - d - fX) & (M); \
    u32 dm = d & (G), rm = r & (G); \
    fC = fX = (dm | rm) ? 1 : 0; fV = (dm & rm) ? 1 : 0; \
    fN = rm ? 1 : 0; if (r) fZ = 0; return r; } \
static inline u32 logic_##S(u32 r) { /* MOVE/AND/OR/EOR/NOT/CLR/TST result flags */ \
    r &= (M); fN = (r & (G)) ? 1 : 0; fZ = (r == 0); fV = 0; fC = 0; return r; } \
static inline void tst_##S(u32 v) { (void)logic_##S(v); }

M68K_ALU(b, 0xFFu, 0x80u)
M68K_ALU(w, 0xFFFFu, 0x8000u)
M68K_ALU(l, 0xFFFFFFFFu, 0x80000000u)

/* ADDQ/SUBQ to an address register leave the flags alone: the generator emits
 * a plain 32-bit add for that case. */

/* ------------------------------------------------------------- shifts/rotates */
/* kind: 0 asl 1 asr 2 lsl 3 lsr 4 rol 5 ror 6 roxl 7 roxr.  n is the already
 * masked count (0..63).  bits = 8/16/32. */
static inline u32 m68k_shift(int kind, int bits, u32 d, u32 n) {
    const u32 mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    const u32 sign = 1u << (bits - 1);
    u32 r = d & mask;
    int c;
    switch (kind) {
    case 0: /* ASL */
        if (n == 0) { fC = 0; fV = 0; }
        else {
            if (n >= (u32)bits) { c = (n == (u32)bits) ? (int)(d & 1) : 0; fV = (d & mask) != 0; r = 0; }
            else {
                u32 topn = (d & mask) >> (bits - 1 - n);                 /* n+1 top bits */
                u32 full = (n + 1 >= 32) ? 0xFFFFFFFFu : ((1u << (n + 1)) - 1);
                fV = (topn != 0 && topn != full);
                c = (int)(((d & mask) >> (bits - n)) & 1);
                r = (r << n) & mask;
            }
            fC = fX = c;
        }
        break;
    case 1: /* ASR */
        if (n == 0) { fC = 0; fV = 0; }
        else {
            if (n >= (u32)bits) { c = (r & sign) ? 1 : 0; r = c ? mask : 0; }
            else {
                c = (int)((r >> (n - 1)) & 1);
                u32 neg = r & sign;
                r >>= n;
                if (neg) r |= (mask << (bits - n)) & mask;
            }
            fC = fX = c; fV = 0;
        }
        break;
    case 2: /* LSL */
        if (n == 0) { fC = 0; }
        else {
            if (n > (u32)bits) { c = 0; r = 0; }
            else { c = (int)((r >> (bits - n)) & 1); r = (n == (u32)bits) ? 0 : (r << n) & mask; }
            fC = fX = c;
        }
        fV = 0;
        break;
    case 3: /* LSR */
        if (n == 0) { fC = 0; }
        else {
            if (n > (u32)bits) { c = 0; r = 0; }
            else { c = (int)((r >> (n - 1)) & 1); r = (n == (u32)bits) ? 0 : r >> n; }
            fC = fX = c;
        }
        fV = 0;
        break;
    case 4: /* ROL */
        if (n == 0) fC = 0;
        else {
            u32 k = n % bits;
            if (k) r = ((r << k) | (r >> (bits - k))) & mask;
            fC = r & 1;
        }
        fV = 0;
        break;
    case 5: /* ROR */
        if (n == 0) fC = 0;
        else {
            u32 k = n % bits;
            if (k) r = ((r >> k) | (r << (bits - k))) & mask;
            fC = (r & sign) ? 1 : 0;
        }
        fV = 0;
        break;
    case 6: /* ROXL */
        if (n == 0) fC = fX;
        else {
            u32 k = n % (bits + 1);
            for (u32 i = 0; i < k; i++) {
                int out = (r & sign) ? 1 : 0;
                r = ((r << 1) | fX) & mask;
                fX = out;
            }
            fC = fX;
        }
        fV = 0;
        break;
    default: /* ROXR */
        if (n == 0) fC = fX;
        else {
            u32 k = n % (bits + 1);
            for (u32 i = 0; i < k; i++) {
                int out = (int)(r & 1);
                r = (r >> 1) | (fX ? sign : 0);
                fX = out;
            }
            fC = fX;
        }
        fV = 0;
        break;
    }
    fN = (r & sign) ? 1 : 0;
    fZ = (r == 0);
    return r;
}

/* ------------------------------------------------------------- multiply/divide */
void m68k_zero_divide(void);   /* raises vector 5 (provided by generated code) */

static inline u32 mulu_w(u32 s, u32 d) {
    u32 r = (s & 0xFFFF) * (d & 0xFFFF);
    fN = (r >> 31) & 1; fZ = (r == 0); fV = 0; fC = 0; return r;
}
static inline u32 muls_w(u32 s, u32 d) {
    s32 r = (s32)(s16)s * (s32)(s16)d;
    fN = ((u32)r >> 31) & 1; fZ = (r == 0); fV = 0; fC = 0; return (u32)r;
}
/* returns the new 32-bit Dn; on overflow Dn is unchanged */
static inline u32 divu_w(u32 s, u32 d) {
    s &= 0xFFFF;
    if (s == 0) { m68k_zero_divide(); return d; }
    u32 q = d / s, rem = d % s;
    if (q > 0xFFFF) { fV = 1; fC = 0; fN = 1; fZ = 0; return d; }
    fN = (q >> 15) & 1; fZ = (q == 0); fV = 0; fC = 0;
    return (rem << 16) | q;
}
static inline u32 divs_w(u32 s, u32 d) {
    s32 dv = (s32)(s16)s, dd = (s32)d;
    if (dv == 0) { m68k_zero_divide(); return d; }
    s64 q = (s64)dd / dv, rem = (s64)dd % dv;
    if (q > 32767 || q < -32768) { fV = 1; fC = 0; fN = 1; fZ = 0; return d; }
    fN = ((u32)q >> 15) & 1; fZ = (q == 0); fV = 0; fC = 0;
    return ((u32)(s32)rem << 16) | ((u32)q & 0xFFFF);
}

/* ------------------------------------------------------------------ BCD */
static inline u32 abcd(u32 s, u32 d) {
    u32 res = (s & 0x0F) + (d & 0x0F) + fX;
    u32 v = ~res;
    if (res > 9) res += 6;
    res += (s & 0xF0) + (d & 0xF0);
    fC = fX = res > 0x99;
    if (fC) res -= 0xA0;
    res &= 0xFF;
    fV = (v & res & 0x80) ? 1 : 0;
    fN = (res & 0x80) ? 1 : 0;
    if (res) fZ = 0;
    return res;
}
static inline u32 sbcd(u32 s, u32 d) {
    u32 res = (d & 0x0F) - (s & 0x0F) - fX;
    if (res > 9) res -= 6;
    res += (d & 0xF0) - (s & 0xF0);
    u32 v = ~res;
    fC = fX = res > 0x99;
    if (fC) res += 0xA0;
    res &= 0xFF;
    fV = (v & res & 0x80) ? 1 : 0;
    fN = (res & 0x80) ? 1 : 0;
    if (res) fZ = 0;
    return res;
}
static inline u32 nbcd(u32 d) { return sbcd(d, 0); }

/* ------------------------------------------------------------------ bit ops */
static inline void bit_test(u32 v, u32 bit) { fZ = ((v >> bit) & 1) == 0; }

/* ------------------------------------------------------- generated-code API */
void m68k_call_vector(int vec);       /* run the handler registered for a vector */
void m68k_reset(void);
void m68k_fatal(const char *msg, u32 pc);
void ramcode_check(u32 ramaddr);   /* verify RAM-resident code still equals its ROM image */

#endif /* M68K_RT_H */
