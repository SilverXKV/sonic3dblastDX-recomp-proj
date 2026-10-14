/* YM2612 + SN76489 sound generation.  Native output rate: 7670454/144 = 53267 Hz stereo. */
#include "hw.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>

#define YM_RATE 53267.0

/* ---------------------------------------------------------------- tables */
static int exp_tab[256];          /* amplitude for fractional attenuation */
static int logsin_tab[256];       /* quarter wave attenuation */
static int tables_ready;

static const u8 dt_tab[3][32] = {
    {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22},
};
static const u8 eg_pat[4][8] = {
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
};
static const u8 eg_pat_hi[4][8] = {          /* rates 48..: 1x pattern, doubled per group above 12 */
    {1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},
};

static void init_tables(void) {
    if (tables_ready) return;
    for (int i = 0; i < 256; i++) {
        logsin_tab[i] = (int)lrint(-log2(sin((i + 0.5) * M_PI / 512.0)) * 256.0);
        exp_tab[i] = (int)lrint(4096.0 * pow(2.0, -(double)i / 256.0));
    }
    tables_ready = 1;
}

static inline int sine_att(int ph10) {        /* returns attenuation (1/256 octave) for a 10-bit phase; bit 12 flags negative */
    int idx = ph10 & 0xFF;
    if (ph10 & 0x100) idx = 0xFF - idx;
    int att = logsin_tab[idx];
    return att | ((ph10 & 0x200) ? 0x10000 : 0);
}
static inline int att_to_out(int att, int env_att) {
    int neg = att & 0x10000;
    int a = (att & 0xFFFF) + env_att;
    int sh = a >> 8;
    int v = sh >= 13 ? 0 : (exp_tab[a & 0xFF] >> sh);
    if (v > 4095) v = 4095;
    return neg ? -v : v;
}

/* ---------------------------------------------------------------- YM2612 */
typedef struct {
    int dt1, mul, tl, ks, ar, am, d1r, d2r, sl, rr, ssg;
    u32 phase;
    int env;              /* 10-bit attenuation */
    int state;            /* 0 attack 1 decay 2 sustain 3 release 4 off */
    int key;
    int fn_block_kc;      /* cached */
    int out;
} Op;

typedef struct {
    Op op[4];             /* op[0]=OP1 op[1]=OP2 op[2]=OP3 op[3]=OP4 */
    int fnum, block, alg, fb, ams, fms, pan_l, pan_r;
    int fn3[3], blk3[3];  /* ch3 special */
    int fb_prev[2];
} Chan;

extern int dbg_ym_writes, dbg_psg_writes, dbg_keyons;
static Chan ch[6];
static u8 ym_addr[2];
static u8 ym_regs[2][256];
static int lfo_on, lfo_freq, lfo_cnt, lfo_pos;
static int dac_en, dac_val = 0;
static int ta, tb, ta_cnt, tb_cnt, ta_run, tb_run, ta_en, tb_en, ta_flag, tb_flag, ch3_mode;
static u32 eg_timer; static int eg_cnt;

static const int lfo_period[8] = {105, 75, 69, 65, 61, 43, 9, 6};   /* samples per LFO step (approx. 128 steps/cycle) */

static void ym_reset(void) {
    memset(ch, 0, sizeof ch);
    for (int c = 0; c < 6; c++) for (int o = 0; o < 4; o++) { ch[c].op[o].env = 0x3FF; ch[c].op[o].state = 4; }
    for (int c = 0; c < 6; c++) { ch[c].pan_l = ch[c].pan_r = 1; }
    memset(ym_regs, 0, sizeof ym_regs);
}

u8 ym_read(int port) {
    (void)port;
    return (u8)(ta_flag | (tb_flag << 1));
}

static int op_keycode(Chan *c, int o, int chn) {
    int fnum = c->fnum, blk = c->block;
    if (chn == 2 && ch3_mode && o != 3) {
        int k = (o == 0) ? 1 : (o == 1) ? 2 : 0;
        fnum = c->fn3[k]; blk = c->blk3[k];
    }
    int f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    int n3 = (f11 & (f10 | f9 | f8)) | ((!f11) & f10 & f9 & f8);
    return (blk << 2) | (f11 << 1) | n3;
}

static int op_phase_inc(Chan *c, int o, int chn) {
    int fnum = c->fnum, blk = c->block;
    if (chn == 2 && ch3_mode && o != 3) {
        int k = (o == 0) ? 1 : (o == 1) ? 2 : 0;
        fnum = c->fn3[k]; blk = c->blk3[k];
    }
    Op *p = &c->op[o];
    int inc = (fnum << blk) >> 1;
    int kc = op_keycode(c, o, chn);
    int dt = p->dt1 & 3;
    if (dt) {
        int d = dt_tab[dt - 1][kc];
        inc += (p->dt1 & 4) ? -d : d;
        inc &= 0x1FFFF;
    }
    if (p->mul) inc *= p->mul; else inc >>= 1;
    return inc;
}

static int eg_rate(Chan *c, int o, int chn, int r) {
    if (!r) return 0;
    int kc = op_keycode(c, o, chn);
    int rate = 2 * r + (kc >> (3 - c->op[o].ks));
    return rate > 63 ? 63 : rate;
}

static void key_on(Chan *c, int o, int chn) {
    dbg_keyons++;
    Op *p = &c->op[o];
    if (p->key) return;
    p->key = 1;
    p->phase = 0;
    int r = eg_rate(c, o, chn, p->ar);
    p->state = 0;
    if (r >= 62) { p->env = 0; p->state = 1; }
}
static void key_off(Chan *c, int o) {
    Op *p = &c->op[o];
    if (!p->key) return;
    p->key = 0;
    if (p->state != 4) p->state = 3;
}

static void ym_write_reg(int part, int reg, u8 v) {
    ym_regs[part][reg] = v;
    if (part == 0 && reg < 0x30) {
        switch (reg) {
        case 0x22: lfo_on = (v >> 3) & 1; lfo_freq = v & 7; if (!lfo_on) { lfo_pos = 0; } break;
        case 0x24: ta = (ta & 3) | (v << 2); break;
        case 0x25: ta = (ta & 0x3FC) | (v & 3); break;
        case 0x26: tb = v; break;
        case 0x27:
            ch3_mode = (v >> 6) & 3;
            if (v & 0x10) ta_flag = 0;
            if (v & 0x20) tb_flag = 0;
            ta_en = (v >> 2) & 1; tb_en = (v >> 3) & 1;
            if ((v & 1) && !ta_run) ta_cnt = 1024 - ta;
            if ((v & 2) && !tb_run) tb_cnt = (256 - tb) * 16;
            ta_run = v & 1; tb_run = (v >> 1) & 1;
            break;
        case 0x28: {
            int c = v & 7;
            if (c == 3 || c == 7) break;
            if (c >= 4) c--;
            for (int o = 0; o < 4; o++) { if (v & (0x10 << o)) key_on(&ch[c], o, c); else key_off(&ch[c], o); }
            break; }
        case 0x2A: dac_val = (int)v - 128; break;
        case 0x2B: dac_en = v >> 7; break;
        }
        return;
    }
    int cn = (reg & 3);
    if (cn == 3) return;
    int chn = cn + part * 3;
    Chan *c = &ch[chn];
    int slot = (reg >> 2) & 3;                       /* 0:OP1 1:OP3 2:OP2 3:OP4 */
    int o = (slot == 1) ? 2 : (slot == 2) ? 1 : slot;
    switch (reg & 0xF0) {
    case 0x30: c->op[o].dt1 = (v >> 4) & 7; c->op[o].mul = v & 15; break;
    case 0x40: c->op[o].tl = v & 0x7F; break;
    case 0x50: c->op[o].ks = (v >> 6) & 3; c->op[o].ar = v & 31; break;
    case 0x60: c->op[o].am = v >> 7; c->op[o].d1r = v & 31; break;
    case 0x70: c->op[o].d2r = v & 31; break;
    case 0x80: c->op[o].sl = v >> 4; c->op[o].rr = v & 15; break;
    case 0x90: c->op[o].ssg = v & 15; break;
    case 0xA0:
        switch (reg & 0xC) {
        case 0x0: c->fnum = ((ym_regs[part][reg + 4] & 7) << 8) | v; c->block = (ym_regs[part][reg + 4] >> 3) & 7; break;
        case 0x4: break;                             /* latched with the next low write */
        case 0x8: if (part == 0 && cn < 3) { Chan *c3 = &ch[2]; c3->fn3[cn] = ((ym_regs[0][0xAC + cn] & 7) << 8) | v; c3->blk3[cn] = (ym_regs[0][0xAC + cn] >> 3) & 7; } break;
        case 0xC: break;
        }
        break;
    case 0xB0:
        if ((reg & 0xC) == 0) { c->alg = v & 7; c->fb = (v >> 3) & 7; }
        else if ((reg & 0xC) == 4) { c->pan_l = (v >> 7) & 1; c->pan_r = (v >> 6) & 1; c->ams = (v >> 4) & 3; c->fms = v & 7; }
        break;
    }
}

int dbg_ym_writes, dbg_psg_writes, dbg_keyons;
void ym_write(int port, u8 v) {
    dbg_ym_writes++;
    init_tables();
    int part = (port >> 1) & 1;
    if (!(port & 1)) { ym_addr[part] = v; return; }
    ym_write_reg(part, ym_addr[part], v);
}

static const int ams_shift[4] = {8, 3, 1, 0};     /* AM depth: 0, 1.4, 5.9, 11.8 dB (approx.) */

static void eg_step(Chan *c, int chn, int o, int do_eg) {
    Op *p = &c->op[o];
    if (!do_eg) return;
    int r;
    switch (p->state) {
    case 0: r = eg_rate(c, o, chn, p->ar); break;
    case 1: r = eg_rate(c, o, chn, p->d1r); break;
    case 2: r = eg_rate(c, o, chn, p->d2r); break;
    case 3: r = 0; break;
    default: return;
    }
    if (p->state == 3) { r = 2 * (2 * p->rr + 1) + (op_keycode(c, o, chn) >> (3 - p->ks)); if (r > 63) r = 63; }
    if (!r) return;
    int grp = r >> 2;
    int inc;
    int shift = 11 - grp; if (shift < 0) shift = 0;
    if ((eg_timer & ((1u << shift) - 1)) != 0) return;
    int idx = (eg_timer >> shift) & 7;
    if (grp >= 12) inc = eg_pat_hi[r & 3][idx] << (grp - 12);
    else inc = eg_pat[r & 3][idx];
    if (!inc) return;
    switch (p->state) {
    case 0:
        if (r >= 62) { p->env = 0; }
        else p->env += ((~p->env) * inc) >> 4;
        if (p->env <= 0) { p->env = 0; p->state = 1; }
        break;
    case 1: {
        p->env += inc;
        int sl = p->sl == 15 ? 0x3E0 : (p->sl << 5);
        if (p->env >= sl) { p->env = sl; p->state = 2; }
        break; }
    case 2: p->env += inc; if (p->env >= 0x3FF) { p->env = 0x3FF; } break;
    case 3: p->env += inc; if (p->env >= 0x3FF) { p->env = 0x3FF; p->state = 4; } break;
    }
    if (p->env > 0x3FF) p->env = 0x3FF;
}

static int ym_sample_ch(int chn, int lfo_am, int *outv) {
    Chan *c = &ch[chn];
    int pinc[4], envv[4];
    for (int o = 0; o < 4; o++) {
        pinc[o] = op_phase_inc(c, o, chn);
        Op *p = &c->op[o];
        int e = p->env + (p->tl << 3);
        if (p->am && c->ams) e += (lfo_am >> ams_shift[c->ams]) ;
        if (e > 0x3FF) e = 0x3FF;
        envv[o] = e << 2;
    }
    int ph[4];
    for (int o = 0; o < 4; o++) ph[o] = (c->op[o].phase >> 10) & 0x3FF;
    int o1, o2, o3, o4;
    int fbm = c->fb ? ((c->fb_prev[0] + c->fb_prev[1]) >> (9 - c->fb)) : 0;
    o1 = att_to_out(sine_att((ph[0] + fbm) & 0x3FF), envv[0]);
    c->fb_prev[1] = c->fb_prev[0]; c->fb_prev[0] = o1;
#define OP(n, m) att_to_out(sine_att((ph[n] + ((m) >> 1)) & 0x3FF), envv[n])
    int out = 0;
    switch (c->alg) {
    case 0: o2 = OP(1, o1); o3 = OP(2, o2); o4 = OP(3, o3); out = o4; break;
    case 1: o2 = OP(1, 0); o3 = OP(2, o1 + o2); o4 = OP(3, o3); out = o4; break;
    case 2: o2 = OP(1, 0); o3 = OP(2, o2); o4 = OP(3, o1 + o3); out = o4; break;
    case 3: o2 = OP(1, o1); o3 = OP(2, 0); o4 = OP(3, o2 + o3); out = o4; break;
    case 4: o2 = OP(1, o1); o3 = OP(2, 0); o4 = OP(3, o3); out = o2 + o4; break;
    case 5: o2 = OP(1, o1); o3 = OP(2, o1); o4 = OP(3, o1); out = o2 + o3 + o4; break;
    case 6: o2 = OP(1, o1); o3 = OP(2, 0); o4 = OP(3, 0); out = o2 + o3 + o4; break;
    default: o2 = OP(1, 0); o3 = OP(2, 0); o4 = OP(3, 0); out = o1 + o2 + o3 + o4; break;
    }
    for (int o = 0; o < 4; o++) c->op[o].phase = (c->op[o].phase + pinc[o]) & 0xFFFFF;
    (void)o4;
    if (out > 8191) out = 8191; else if (out < -8192) out = -8192;
    *outv = out;
    return out;
}

static int psg_vol_tab[16];
static struct { int tone[4]; int vol[4]; int cnt[4]; int out[4]; u32 lfsr; int latch; int type; double acc; } psg;

void psg_write(u8 v) {
    dbg_psg_writes++;
    if (v & 0x80) {
        psg.latch = (v >> 4) & 7;
        int c = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[c] = v & 15;
        else if (c < 3) psg.tone[c] = (psg.tone[c] & 0x3F0) | (v & 15);
        else { psg.tone[3] = v & 7; psg.lfsr = 0x8000; }
    } else {
        int c = psg.latch >> 1;
        if (psg.latch & 1) psg.vol[c] = v & 15;
        else if (c < 3) psg.tone[c] = (psg.tone[c] & 15) | ((v & 0x3F) << 4);
        else { psg.tone[3] = v & 7; psg.lfsr = 0x8000; }
    }
}

static void psg_init(void) {
    for (int i = 0; i < 16; i++) psg_vol_tab[i] = i == 15 ? 0 : (int)(2000.0 * pow(10.0, -0.1 * i));
    for (int c = 0; c < 4; c++) { psg.vol[c] = 15; psg.out[c] = 1; psg.cnt[c] = 0; }
    psg.lfsr = 0x8000;
}

static int psg_sample(void) {
    psg.acc += 223722.0 / YM_RATE;
    int ticks = (int)psg.acc; psg.acc -= ticks;
    while (ticks--) {
        for (int c = 0; c < 3; c++) {
            if (--psg.cnt[c] <= 0) { psg.cnt[c] = psg.tone[c] ? psg.tone[c] : 1; psg.out[c] ^= 1; }
        }
        if (--psg.cnt[3] <= 0) {
            int t = psg.tone[3] & 3;
            psg.cnt[3] = t == 0 ? 16 : t == 1 ? 32 : t == 2 ? 64 : (psg.tone[2] ? psg.tone[2] : 1);
            psg.out[3] ^= 1;
            if (psg.out[3]) {
                int fb = (psg.tone[3] & 4) ? ((psg.lfsr ^ (psg.lfsr >> 3)) & 1) : (psg.lfsr & 1);
                psg.lfsr = (psg.lfsr >> 1) | (fb << 15);
            }
        }
    }
    int s = 0;
    for (int c = 0; c < 3; c++) s += (psg.out[c] ? 1 : -1) * psg_vol_tab[psg.vol[c]];
    s += ((psg.lfsr & 1) ? 1 : -1) * psg_vol_tab[psg.vol[3]];
    return s;
}

/* ---------------------------------------------------------------- output buffer */
#define OUTMAX 8192
s16 audio_out[OUTMAX * 2];
int audio_count;
static double sample_acc;
static int audio_inited;
int audio_enabled = 1;

void audio_init(void) {
    init_tables(); ym_reset(); psg_init();
    audio_inited = 1; audio_count = 0;
}

static void gen_sample(void) {
    int l = 0, r = 0;
    /* envelope generator runs every 3rd sample */
    int do_eg = 0;
    if (++eg_cnt >= 3) { eg_cnt = 0; eg_timer++; do_eg = 1; }
    /* LFO */
    int lfo_am = 0;
    if (lfo_on) {
        if (++lfo_cnt >= lfo_period[lfo_freq]) { lfo_cnt = 0; lfo_pos = (lfo_pos + 1) & 127; }
        int t = lfo_pos < 64 ? lfo_pos : 127 - lfo_pos;   /* triangle 0..63 */
        lfo_am = (t * 2) ;                                 /* 0..126 attenuation */
    }
    for (int cn = 0; cn < 6; cn++) {
        Chan *c = &ch[cn];
        for (int o = 0; o < 4; o++) eg_step(c, cn, o, do_eg);
        int v;
        if (cn == 5 && dac_en) v = dac_val * 64;
        else ym_sample_ch(cn, lfo_am, &v);
        v = v >> 4;  if (v > 255) v = 255; else if (v < -256) v = -256;
        if (cn == 5 && dac_en) v = dac_val * 2;
        if (c->pan_l) l += v;
        if (c->pan_r) r += v;
    }
    /* timers */
    if (ta_run && --ta_cnt <= 0) { ta_cnt = 1024 - ta; if (ta_en) ta_flag = 1; }
    if (tb_run && --tb_cnt <= 0) { tb_cnt = (256 - tb) * 16; if (tb_en) tb_flag = 1; }
    int ps = psg_sample();
    l = l * 24 + ps; r = r * 24 + ps;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    if (audio_count < OUTMAX) { audio_out[audio_count * 2] = (s16)l; audio_out[audio_count * 2 + 1] = (s16)r; audio_count++; }
}

void audio_advance(s64 cycles68k) {
    if (!audio_inited) audio_init();
    sample_acc += cycles68k / 144.0;
    while (sample_acc >= 1.0) { sample_acc -= 1.0; gen_sample(); }
}
