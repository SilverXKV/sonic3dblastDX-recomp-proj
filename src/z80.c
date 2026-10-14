/* Zilog Z80 interpreter for the Mega Drive sound CPU. */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>

extern u8 *hw_z80_ram(void);
extern u32 hw_z80_bank(void);
extern void ym_write(int port, u8 v);
extern u8 ym_read(int port);
extern void psg_write(u8 v);

static struct {
    u8 a, f, b, c, d, e, h, l;
    u8 a_, f_, b_, c_, d_, e_, h_, l_;
    u16 ix, iy, sp, pc;
    u8 i, r;
    u8 iff1, iff2, im, halted;
    int ei_delay;
    int int_line;
} Z;

static s64 z80_cycles;          /* total z80 cycles executed */
static double z80_frac;

static u8 z80_rd(u16 a) {
    if (a < 0x4000) return hw_z80_ram()[a & 0x1FFF];
    if (a < 0x6000) return ym_read(a & 3);
    if (a < 0x8000) return 0xFF;
    return rd8(((hw_z80_bank() & 0x1FF) << 15) | (a & 0x7FFF));
}
static void z80_wr(u16 a, u8 v) {
    if (a < 0x4000) { hw_z80_ram()[a & 0x1FFF] = v; return; }
    if (a < 0x6000) { ym_write(a & 3, v); return; }
    if (a < 0x6100) { hw_wr8(0xA06000, v); return; }
    if (a >= 0x7F00 && a < 0x8000) { if ((a & 0xFF) == 0x11 || (a & 0xFF) == 0x13 || (a & 0xFF) == 0x15 || (a & 0xFF) == 0x17) psg_write(v); return; }
    if (a >= 0x8000) wr8(((hw_z80_bank() & 0x1FF) << 15) | (a & 0x7FFF), v);
}
static inline u16 rd16z(u16 a) { return z80_rd(a) | (z80_rd(a + 1) << 8); }
static inline void wr16z(u16 a, u16 v) { z80_wr(a, v); z80_wr(a + 1, v >> 8); }

#define FS 0x80
#define FZ 0x40
#define FY 0x20
#define FH 0x10
#define FX 0x08
#define FP 0x04
#define FN 0x02
#define FC 0x01

static u8 parity(u8 v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (v & 1) ? 0 : FP; }
static u8 szp[256];
static int tinit;

void z80_reset_cpu(void) {
    if (!tinit) { for (int i = 0; i < 256; i++) szp[i] = (i & 0x80) | (i ? 0 : FZ) | (i & 0x28) | parity(i); tinit = 1; }
    memset(&Z, 0, sizeof Z);
    Z.sp = 0xFFFF; Z.a = Z.f = 0xFF;
    Z.pc = 0;
}

#define BC ((Z.b << 8) | Z.c)
#define DE ((Z.d << 8) | Z.e)
#define HL ((Z.h << 8) | Z.l)
#define AF ((Z.a << 8) | Z.f)

static inline u8 fetch(void) { return z80_rd(Z.pc++); }
static inline u16 fetch16(void) { u16 v = rd16z(Z.pc); Z.pc += 2; return v; }
static inline void push(u16 v) { Z.sp -= 2; wr16z(Z.sp, v); }
static inline u16 pop(void) { u16 v = rd16z(Z.sp); Z.sp += 2; return v; }

static u8 *reg8(int r, int pfx) {   /* r: 0 B 1 C 2 D 3 E 4 H 5 L 7 A ; pfx: 0 none, 1 IX, 2 IY (H/L -> IXH/IXL) */
    static u16 dummy;
    (void)dummy;
    switch (r) {
    case 0: return &Z.b; case 1: return &Z.c; case 2: return &Z.d; case 3: return &Z.e;
    case 4: if (pfx == 1) return (u8 *)&Z.ix + 1; if (pfx == 2) return (u8 *)&Z.iy + 1; return &Z.h;
    case 5: if (pfx == 1) return (u8 *)&Z.ix; if (pfx == 2) return (u8 *)&Z.iy; return &Z.l;
    case 7: return &Z.a;
    }
    return NULL;
}

static u16 getrp(int p, int pfx) { switch (p) { case 0: return BC; case 1: return DE; case 2: return pfx == 1 ? Z.ix : pfx == 2 ? Z.iy : HL; default: return Z.sp; } }
static void setrp(int p, u16 v, int pfx) {
    switch (p) {
    case 0: Z.b = v >> 8; Z.c = v; break; case 1: Z.d = v >> 8; Z.e = v; break;
    case 2: if (pfx == 1) Z.ix = v; else if (pfx == 2) Z.iy = v; else { Z.h = v >> 8; Z.l = v; } break;
    default: Z.sp = v;
    }
}
static u16 getrp2(int p, int pfx) { return p == 3 ? AF : getrp(p, pfx); }
static void setrp2(int p, u16 v, int pfx) { if (p == 3) { Z.a = v >> 8; Z.f = v; } else setrp(p, v, pfx); }

static int cond(int c) {
    switch (c) {
    case 0: return !(Z.f & FZ); case 1: return Z.f & FZ; case 2: return !(Z.f & FC); case 3: return Z.f & FC;
    case 4: return !(Z.f & FP); case 5: return Z.f & FP; case 6: return !(Z.f & FS); default: return Z.f & FS;
    }
}

static u8 alu(int op, u8 a, u8 b) {
    int r;
    switch (op) {
    case 0: r = a + b; Z.f = (r & 0xA8) | ((r & 0xFF) ? 0 : FZ) | (((a ^ b ^ r) & 0x10)) | ((~(a ^ b) & (a ^ r) & 0x80) ? FP : 0) | ((r >> 8) & 1); return r;
    case 1: { int c = Z.f & 1; r = a + b + c; Z.f = (r & 0xA8) | ((r & 0xFF) ? 0 : FZ) | (((a ^ b ^ r) & 0x10)) | ((~(a ^ b) & (a ^ r) & 0x80) ? FP : 0) | ((r >> 8) & 1); return r; }
    case 2: r = a - b; Z.f = (r & 0xA8) | ((r & 0xFF) ? 0 : FZ) | (((a ^ b ^ r) & 0x10)) | (((a ^ b) & (a ^ r) & 0x80) ? FP : 0) | FN | ((r >> 8) & 1); return r;
    case 3: { int c = Z.f & 1; r = a - b - c; Z.f = (r & 0xA8) | ((r & 0xFF) ? 0 : FZ) | (((a ^ b ^ r) & 0x10)) | (((a ^ b) & (a ^ r) & 0x80) ? FP : 0) | FN | ((r >> 8) & 1); return r; }
    case 4: r = a & b; Z.f = szp[r & 0xFF] | FH; return r;
    case 5: r = a ^ b; Z.f = szp[r & 0xFF]; return r;
    case 6: r = a | b; Z.f = szp[r & 0xFF]; return r;
    default: r = a - b; Z.f = (b & 0x28) | (r & 0x80) | ((r & 0xFF) ? 0 : FZ) | (((a ^ b ^ r) & 0x10)) | (((a ^ b) & (a ^ r) & 0x80) ? FP : 0) | FN | ((r >> 8) & 1); return a;
    }
}
static u8 inc8(u8 v) { u8 r = v + 1; Z.f = (Z.f & FC) | (r & 0xA8) | (r ? 0 : FZ) | ((v & 15) == 15 ? FH : 0) | (v == 0x7F ? FP : 0); return r; }
static u8 dec8(u8 v) { u8 r = v - 1; Z.f = (Z.f & FC) | (r & 0xA8) | (r ? 0 : FZ) | ((v & 15) == 0 ? FH : 0) | (v == 0x80 ? FP : 0) | FN; return r; }
static u16 add16(u16 a, u16 b) {
    u32 r = a + b;
    Z.f = (Z.f & (FS | FZ | FP)) | ((r >> 8) & 0x28) | (((a ^ b ^ r) >> 8) & 0x10) | ((r >> 16) & 1);
    return r;
}
static u16 adc16(u16 a, u16 b) {
    u32 c = Z.f & 1, r = a + b + c;
    Z.f = ((r >> 8) & 0xA8) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & 0x10) | ((~(a ^ b) & (a ^ r) & 0x8000) ? FP : 0) | ((r >> 16) & 1);
    return r;
}
static u16 sbc16(u16 a, u16 b) {
    u32 c = Z.f & 1, r = a - b - c;
    Z.f = ((r >> 8) & 0xA8) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ b ^ r) >> 8) & 0x10) | (((a ^ b) & (a ^ r) & 0x8000) ? FP : 0) | FN | ((r >> 16) & 1);
    return r;
}
static u8 rot(int op, u8 v) {
    u8 c, r;
    switch (op) {
    case 0: c = v >> 7; r = (v << 1) | c; break;                  /* rlc */
    case 1: c = v & 1; r = (v >> 1) | (c << 7); break;            /* rrc */
    case 2: c = v >> 7; r = (v << 1) | (Z.f & 1); break;          /* rl */
    case 3: c = v & 1; r = (v >> 1) | ((Z.f & 1) << 7); break;    /* rr */
    case 4: c = v >> 7; r = v << 1; break;                        /* sla */
    case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;          /* sra */
    case 6: c = v >> 7; r = (v << 1) | 1; break;                  /* sll */
    default: c = v & 1; r = v >> 1; break;                        /* srl */
    }
    Z.f = szp[r] | c;
    return r;
}
static void daa(void) {
    u8 a = Z.a, c = Z.f & FC, h = Z.f & FH, n = Z.f & FN, adj = 0;
    if (h || (a & 15) > 9) adj |= 6;
    if (c || a > 0x99) { adj |= 0x60; c = FC; }
    u8 r = n ? a - adj : a + adj;
    Z.f = szp[r] | c | n | (((a ^ r) & 0x10) ? FH : 0);
    Z.a = r;
}

static int exec_cb(int pfx, int disp_valid, u16 ea) {
    u8 op = fetch();
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    u8 v;
    int cyc;
    if (pfx) { v = z80_rd(ea); cyc = 23; }
    else if (z == 6) { ea = HL; v = z80_rd(ea); cyc = 12; }
    else { v = *reg8(z, 0); cyc = 8; }
    u8 r = v;
    switch (x) {
    case 0: r = rot(y, v); break;
    case 1: Z.f = (Z.f & FC) | FH | ((v & (1 << y)) ? 0 : (FZ | FP)) | ((v & (1 << y)) & 0x80) | ((pfx || z == 6) ? ((ea >> 8) & 0x28) : (v & 0x28)); return pfx ? 20 : (z == 6 ? 12 : 8);
    case 2: r = v & ~(1 << y); break;
    default: r = v | (1 << y); break;
    }
    if (pfx) { z80_wr(ea, r); if (z != 6) *reg8(z, 0) = r; }
    else if (z == 6) z80_wr(ea, r);
    else *reg8(z, 0) = r;
    (void)disp_valid;
    return cyc;
}

static int exec_ed(void) {
    u8 op = fetch();
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x == 1) {
        switch (z) {
        case 0: { u8 v = 0xFF; Z.f = (Z.f & FC) | szp[v]; if (y != 6) *reg8(y, 0) = v; return 12; }
        case 1: return 12;
        case 2: setrp(p, q ? adc16(HL, getrp(p, 0)) : sbc16(HL, getrp(p, 0)), 0); if (p == 2) {} return 15;
        case 3: { u16 a = fetch16(); if (q) setrp(p, rd16z(a), 0); else wr16z(a, getrp(p, 0)); return 20; }
        case 4: { u8 v = Z.a; u8 r = 0 - v; Z.f = (r & 0xA8) | (r ? 0 : FZ) | ((v & 15) ? FH : 0) | (v == 0x80 ? FP : 0) | FN | (v ? FC : 0); Z.a = r; return 8; }
        case 5: { u16 a = pop(); Z.pc = a; if (q == 0 || 1) Z.iff1 = Z.iff2; return 14; }
        case 6: Z.im = (y & 3) == 2 ? 1 : (y & 3) == 3 ? 2 : 0; return 8;
        case 7:
            switch (y) {
            case 0: Z.i = Z.a; return 9; case 1: Z.r = Z.a; return 9;
            case 2: Z.a = Z.i; Z.f = (Z.f & FC) | szp[Z.a] & ~FP | (Z.iff2 ? FP : 0); return 9;
            case 3: Z.a = Z.r; Z.f = (Z.f & FC) | szp[Z.a] & ~FP | (Z.iff2 ? FP : 0); return 9;
            case 4: { u8 m = z80_rd(HL); z80_wr(HL, (Z.a << 4) | (m >> 4)); Z.a = (Z.a & 0xF0) | (m & 15); Z.f = (Z.f & FC) | szp[Z.a]; return 18; }
            case 5: { u8 m = z80_rd(HL); z80_wr(HL, (m << 4) | (Z.a & 15)); Z.a = (Z.a & 0xF0) | (m >> 4); Z.f = (Z.f & FC) | szp[Z.a]; return 18; }
            default: return 8;
            }
        }
    }
    if (x == 2 && y >= 4 && z < 4) {
        int cyc = 16;
        switch (z) {
        case 0: { u8 v = z80_rd(HL); z80_wr(DE, v);
            u16 hl = HL + (y & 1 ? -1 : 1), de = DE + (y & 1 ? -1 : 1), bc = BC - 1;
            Z.h = hl >> 8; Z.l = hl; Z.d = de >> 8; Z.e = de; Z.b = bc >> 8; Z.c = bc;
            u8 n = v + Z.a; Z.f = (Z.f & (FS | FZ | FC)) | (bc ? FP : 0) | (n & 8) | ((n & 2) << 4);
            if ((y & 2) && bc) { Z.pc -= 2; cyc = 21; } } break;
        case 1: { u8 v = z80_rd(HL); u8 r = Z.a - v;
            u16 hl = HL + (y & 1 ? -1 : 1), bc = BC - 1;
            Z.h = hl >> 8; Z.l = hl; Z.b = bc >> 8; Z.c = bc;
            u8 h = ((Z.a ^ v ^ r) & 0x10) ? FH : 0; u8 n = r - (h ? 1 : 0);
            Z.f = (Z.f & FC) | (r & 0x80) | (r ? 0 : FZ) | h | (bc ? FP : 0) | FN | (n & 8) | ((n & 2) << 4);
            if ((y & 2) && bc && r) { Z.pc -= 2; cyc = 21; } } break;
        case 2: { u8 v = 0xFF; z80_wr(HL, v); u16 hl = HL + (y & 1 ? -1 : 1); Z.h = hl >> 8; Z.l = hl; Z.b--; Z.f = (szp[Z.b] & ~FP) | FN;
            if ((y & 2) && Z.b) { Z.pc -= 2; cyc = 21; } } break;
        case 3: { u8 v = z80_rd(HL); (void)v; u16 hl = HL + (y & 1 ? -1 : 1); Z.h = hl >> 8; Z.l = hl; Z.b--; Z.f = (szp[Z.b] & ~FP) | FN;
            if ((y & 2) && Z.b) { Z.pc -= 2; cyc = 21; } } break;
        }
        return cyc;
    }
    return 8;   /* NOP-equivalent for undefined ED opcodes */
}

static int exec_one(void) {
    int pfx = 0;
    u8 op;
    int cyc = 0;
    for (;;) {
        op = fetch();
        Z.r = (Z.r & 0x80) | ((Z.r + 1) & 0x7F);
        if (op == 0xDD) { pfx = 1; cyc += 4; continue; }
        if (op == 0xFD) { pfx = 2; cyc += 4; continue; }
        break;
    }
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    u16 hlv = pfx == 1 ? Z.ix : pfx == 2 ? Z.iy : HL;
    u16 ea = 0;
    int memop = 0;
    /* (HL) / (IX+d) operand resolution for opcodes that use r=6 */
    #define MEMEA() do { if (pfx) { s8 d = (s8)fetch(); ea = hlv + d; cyc += 8; } else ea = HL; memop = 1; } while (0)
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: return cyc + 4;
            case 1: { u8 t; t = Z.a; Z.a = Z.a_; Z.a_ = t; t = Z.f; Z.f = Z.f_; Z.f_ = t; return cyc + 4; }
            case 2: { s8 d = (s8)fetch(); if (--Z.b) { Z.pc += d; return cyc + 13; } return cyc + 8; }
            case 3: { s8 d = (s8)fetch(); Z.pc += d; return cyc + 12; }
            default: { s8 d = (s8)fetch(); if (cond(y - 4)) { Z.pc += d; return cyc + 12; } return cyc + 7; }
            }
        case 1:
            if (!q) { setrp(p, fetch16(), pfx); return cyc + 10; }
            { u16 r = add16(hlv, getrp(p, pfx)); if (pfx == 1) Z.ix = r; else if (pfx == 2) Z.iy = r; else { Z.h = r >> 8; Z.l = r; } return cyc + 11; }
        case 2:
            switch (y) {
            case 0: z80_wr(BC, Z.a); return cyc + 7;
            case 1: Z.a = z80_rd(BC); return cyc + 7;
            case 2: z80_wr(DE, Z.a); return cyc + 7;
            case 3: Z.a = z80_rd(DE); return cyc + 7;
            case 4: { u16 a = fetch16(); wr16z(a, hlv); return cyc + 16; }
            case 5: { u16 a = fetch16(); u16 v = rd16z(a); if (pfx == 1) Z.ix = v; else if (pfx == 2) Z.iy = v; else { Z.h = v >> 8; Z.l = v; } return cyc + 16; }
            case 6: { u16 a = fetch16(); z80_wr(a, Z.a); return cyc + 13; }
            default: { u16 a = fetch16(); Z.a = z80_rd(a); return cyc + 13; }
            }
        case 3: { u16 v = getrp(p, pfx) + (q ? -1 : 1); setrp(p, v, pfx); return cyc + 6; }
        case 4: case 5:
            if (y == 6) { MEMEA(); u8 v = z80_rd(ea); v = z == 4 ? inc8(v) : dec8(v); z80_wr(ea, v); return cyc + 11; }
            { u8 *r = reg8(y, pfx); *r = z == 4 ? inc8(*r) : dec8(*r); return cyc + 4; }
        case 6:
            if (y == 6) { MEMEA(); u8 v = fetch(); z80_wr(ea, v); return cyc + 10; }
            { *reg8(y, pfx) = fetch(); return cyc + 7; }
        default:
            switch (y) {
            case 0: { u8 c = Z.a >> 7; Z.a = (Z.a << 1) | c; Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | c; return 4; }
            case 1: { u8 c = Z.a & 1; Z.a = (Z.a >> 1) | (c << 7); Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | c; return 4; }
            case 2: { u8 c = Z.a >> 7; Z.a = (Z.a << 1) | (Z.f & 1); Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | c; return 4; }
            case 3: { u8 c = Z.a & 1; Z.a = (Z.a >> 1) | ((Z.f & 1) << 7); Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | c; return 4; }
            case 4: daa(); return 4;
            case 5: Z.a = ~Z.a; Z.f = (Z.f & (FS | FZ | FP | FC)) | (Z.a & 0x28) | FH | FN; return 4;
            case 6: Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | FC; return 4;
            default: Z.f = (Z.f & (FS | FZ | FP)) | (Z.a & 0x28) | ((Z.f & FC) ? FH : FC); return 4;
            }
        }
    case 1:
        if (z == 6 && y == 6) { Z.halted = 1; return cyc + 4; }
        if (y == 6) { MEMEA(); z80_wr(ea, *reg8(z, 0)); return cyc + 7; }
        if (z == 6) { MEMEA(); *reg8(y, 0) = z80_rd(ea); return cyc + 7; }
        *reg8(y, pfx) = *reg8(z, pfx);
        return cyc + 4;
    case 2:
        if (z == 6) { MEMEA(); Z.a = alu(y, Z.a, z80_rd(ea)); return cyc + 7; }
        Z.a = alu(y, Z.a, *reg8(z, pfx));
        return cyc + 4;
    default:
        switch (z) {
        case 0: if (cond(y)) { Z.pc = pop(); return cyc + 11; } return cyc + 5;
        case 1:
            if (!q) { setrp2(p, pop(), pfx); return cyc + 10; }
            switch (p) {
            case 0: Z.pc = pop(); return cyc + 10;
            case 1: { u8 t;
                t = Z.b; Z.b = Z.b_; Z.b_ = t; t = Z.c; Z.c = Z.c_; Z.c_ = t; t = Z.d; Z.d = Z.d_; Z.d_ = t;
                t = Z.e; Z.e = Z.e_; Z.e_ = t; t = Z.h; Z.h = Z.h_; Z.h_ = t; t = Z.l; Z.l = Z.l_; Z.l_ = t; return cyc + 4; }
            case 2: Z.pc = hlv; return cyc + 4;
            default: Z.sp = hlv; return cyc + 6;
            }
        case 2: { u16 a = fetch16(); if (cond(y)) Z.pc = a; return cyc + 10; }
        case 3:
            switch (y) {
            case 0: Z.pc = fetch16(); return cyc + 10;
            case 1: {
                if (pfx) { s8 d = (s8)fetch(); ea = hlv + d; return cyc + exec_cb(1, 1, ea) - 4; }
                return cyc + exec_cb(0, 0, 0);
            }
            case 2: fetch(); return cyc + 11;                       /* OUT (n),A */
            case 3: fetch(); Z.a = 0xFF; return cyc + 11;           /* IN A,(n) */
            case 4: { u16 v = rd16z(Z.sp); wr16z(Z.sp, hlv); if (pfx == 1) Z.ix = v; else if (pfx == 2) Z.iy = v; else { Z.h = v >> 8; Z.l = v; } return cyc + 19; }
            case 5: { u8 t = Z.d; Z.d = Z.h; Z.h = t; t = Z.e; Z.e = Z.l; Z.l = t; return cyc + 4; }
            case 6: Z.iff1 = Z.iff2 = 0; return cyc + 4;
            default: Z.iff1 = Z.iff2 = 1; Z.ei_delay = 1; return cyc + 4;
            }
        case 4: { u16 a = fetch16(); if (cond(y)) { push(Z.pc); Z.pc = a; return cyc + 17; } return cyc + 10; }
        case 5:
            if (!q) { push(getrp2(p, pfx)); return cyc + 11; }
            switch (p) {
            case 0: { u16 a = fetch16(); push(Z.pc); Z.pc = a; return cyc + 17; }
            case 2: return cyc + exec_ed();
            default: return cyc + 4;          /* DD/FD prefix handled above */
            }
        case 6: Z.a = alu(y, Z.a, fetch()); return cyc + 7;
        default: push(Z.pc); Z.pc = y * 8; return cyc + 11;
        }
    }
}

/* run the Z80 for the given number of 68000 cycles' worth of time */
void z80_run(s64 cycles68k) {
    extern int hw_z80_running(void);
    z80_frac += cycles68k * (3579545.0 / 7670454.0);
    int n = (int)z80_frac;
    z80_frac -= n;
    if (!hw_z80_running()) { z80_cycles += n; return; }
    int left = n;
    while (left > 0) {
        int c;
        if (Z.int_line && Z.iff1 && !Z.ei_delay) {
            Z.halted = 0;
            Z.iff1 = Z.iff2 = 0;
            if (Z.im == 2) { push(Z.pc); Z.pc = rd16z((Z.i << 8) | 0xFF); c = 19; }
            else { push(Z.pc); Z.pc = 0x38; c = 13; }
            left -= c; continue;
        }
        if (Z.halted) { c = 4; }
        else {
            int d = Z.ei_delay;
            c = exec_one();
            if (d) Z.ei_delay = 0;
        }
        left -= c;
    }
    z80_cycles += n - left;     /* left <= 0: slight overrun carried by ignoring */
    z80_frac += left;           /* give back the overrun */
}
void z80_set_int(int v) { Z.int_line = v; }


int dbg_z80_pc(void) { return Z.pc; }
int dbg_z80_state(void) { return Z.halted | (Z.iff1 << 1) | (Z.im << 2); }
