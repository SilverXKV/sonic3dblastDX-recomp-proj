/* Mega Drive hardware: bus decode, VDP (registers, memories, DMA, status, interrupts), I/O ports,
 * Z80 bus handshake and Z80 RAM, frame/line scheduler. */
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>

u8  vdp_vram[0x10000];
u8  vdp_vram_hi[0x10000];                 /* 16:9 patch: second 64K bank holding the (larger) streamed ground-tile pool */
static int vdp_a16;
u8  wide_tilebuf[4096];                   /* 16:9 patch: staging buffer for the streamed tile rows, mapped at $610000 */
u16 vdp_cram[64];
u16 vdp_vsram[40];
u8  vdp_reg[32];
int vdp_line, vdp_pal, vdp_frame;
u8  pad_state[2];

static u8   z80_ram[0x2000];
static int  z80_busreq, z80_reset = 1;
static u32  z80_bank;
static int  hw_log = 0;

/* VDP internals */
static int  vdp_pending;       /* first half of a command written */
static u32  vdp_addr;
static u32  vdp_code;
static u16  vdp_rbuf;
static int  vdp_dma_fill;
static int  vint_pending, hint_pending;
static int  hint_counter;
static s64  line_start, line_len = 488;
static u8   io_data[3] = {0x7F, 0x7F, 0x7F}, io_ctrl[3];
static int  z80_int_cnt;

/* The Z80 is emulated lazily: it is caught up to the 68000's clock whenever the 68000 changes its bus
 * request / reset lines (so a short bus hold is not mistaken for a whole scanline) and at every line end. */
static s64 z80_sync_cycle;
static void z80_sync_to(s64 target) {
    if (target > z80_sync_cycle) { z80_run(target - z80_sync_cycle); z80_sync_cycle = target; }
}
static int lines_total(void) { return vdp_pal ? 313 : 262; }
static int active_lines(void) { return (vdp_reg[1] & 8) ? 240 : 224; }

static void update_irq(void) {
    int lvl = 0;
    if (vint_pending && (vdp_reg[1] & 0x20)) lvl = 6;
    else if (hint_pending && (vdp_reg[0] & 0x10)) lvl = 4;
    m68k_irq_level = lvl;
}
void hw_irq_ack(int level) {
    if (level == 6) vint_pending = 0;
    else if (level == 4) hint_pending = 0;
    update_irq();
}

void hw_init(void) {
    memset(vdp_vram, 0, sizeof vdp_vram);
    memset(vdp_vram_hi, 0, sizeof vdp_vram_hi);
    memset(vdp_cram, 0, sizeof vdp_cram);
    memset(vdp_vsram, 0, sizeof vdp_vsram);
    memset(vdp_reg, 0, sizeof vdp_reg);
    vdp_line = 0; vdp_frame = 0; line_start = 0;
    m68k_next_event = line_len;
    hint_counter = 0;
    { extern void z80_reset_cpu(void); extern void audio_init(void); z80_reset_cpu(); audio_init(); }
}

/* ------------------------------------------------------------------ VDP memory writes */
static void vdp_write_mem(u16 v) {
    u32 a = vdp_addr;
    switch (vdp_code & 0xF) {
    case 1:
        if (a & 1) v = (u16)((v >> 8) | (v << 8));
        if (vdp_a16) { vdp_vram_hi[a & 0xFFFE] = v >> 8; vdp_vram_hi[(a & 0xFFFE) + 1] = (u8)v; break; }
        vdp_vram[a & 0xFFFE] = v >> 8; vdp_vram[(a & 0xFFFE) + 1] = (u8)v;
        break;
    case 3: vdp_cram[(a >> 1) & 63] = v & 0x0EEE; break;
    case 5: if (((a >> 1) & 63) < 40) vdp_vsram[(a >> 1) & 63] = v & 0x7FF; break;
    default: break;
    }
    vdp_addr = (a + vdp_reg[15]) & 0xFFFF;
}
static u16 vdp_read_mem(void) {
    u32 a = vdp_addr;
    u16 v = 0;
    switch (vdp_code & 0xF) {
    case 0: v = (u16)((vdp_vram[a & 0xFFFE] << 8) | vdp_vram[(a & 0xFFFE) + 1]); break;
    case 8: v = vdp_cram[(a >> 1) & 63]; break;
    case 4: v = ((a >> 1) & 63) < 40 ? vdp_vsram[(a >> 1) & 63] : 0; break;
    }
    vdp_addr = (a + vdp_reg[15]) & 0xFFFF;
    return v;
}

static void vdp_dma_68k(void) {
    u32 len = vdp_reg[19] | (vdp_reg[20] << 8);
    if (!len) len = 0x10000;
    u32 src = ((u32)(vdp_reg[23] & 0x7F) << 17) | ((u32)vdp_reg[22] << 9) | ((u32)vdp_reg[21] << 1);
    u32 n = len;
    while (n--) {
        u16 w = rd16(src);
        vdp_write_mem(w);
        src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
    }
    m68k_cycle += len * 3;                     /* the 68000 is stalled while the DMA runs */
    vdp_reg[19] = vdp_reg[20] = 0;
    vdp_reg[21] = (src >> 1) & 0xFF; vdp_reg[22] = (src >> 9) & 0xFF;
}
static void vdp_dma_copy(void) {
    u32 len = vdp_reg[19] | (vdp_reg[20] << 8);
    if (!len) len = 0x10000;
    u32 src = vdp_reg[21] | (vdp_reg[22] << 8);
    while (len--) {
        vdp_vram[vdp_addr & 0xFFFF] = vdp_vram[src & 0xFFFF];
        src++; vdp_addr = (vdp_addr + vdp_reg[15]) & 0xFFFF;
    }
    vdp_reg[19] = vdp_reg[20] = 0;
}
static void vdp_do_fill(u16 data) {
    u32 len = vdp_reg[19] | (vdp_reg[20] << 8);
    if (!len) len = 0x10000;
    u8 b = data >> 8;
    while (len--) {
        switch (vdp_code & 0xF) {
        case 1: vdp_vram[(vdp_addr ^ 1) & 0xFFFF] = b; break;
        case 3: vdp_cram[(vdp_addr >> 1) & 63] = data & 0x0EEE; break;
        case 5: if (((vdp_addr >> 1) & 63) < 40) vdp_vsram[(vdp_addr >> 1) & 63] = data & 0x7FF; break;
        }
        vdp_addr = (vdp_addr + vdp_reg[15]) & 0xFFFF;
    }
    vdp_reg[19] = vdp_reg[20] = 0;
    vdp_dma_fill = 0;
}

static void vdp_data_w(u16 v) {
    vdp_pending = 0;
    /* After a 68000->VDP DMA command the DMA bit (CD5) stays set in the command register.  The VDP does not
     * treat further data-port writes as ordinary memory writes then (only a DMA fill uses them).  The intro's
     * per-line interrupt stream relies on this: it fires before its own destination is set up, and on hardware
     * those early writes are discarded instead of overwriting the palette that was just loaded. */
    if ((vdp_code & 0x20) && !vdp_dma_fill) return;
    if (vdp_code & 1 || (vdp_code & 0xF) == 3 || (vdp_code & 0xF) == 5) {
        vdp_write_mem(v);
        if (vdp_dma_fill) vdp_do_fill(v);
    }
}
static u16 vdp_data_r(void) {
    vdp_pending = 0;
    u16 r = vdp_rbuf;
    vdp_rbuf = vdp_read_mem();
    return r;
}

static void vdp_ctrl_w(u16 v) {
    if (vdp_pending) {
        vdp_pending = 0;
        vdp_addr = (vdp_addr & 0x3FFF) | ((u32)(v & 3) << 14);
        vdp_a16 = (v >> 2) & 1;
        vdp_code = (vdp_code & 3) | ((v >> 2) & 0x3C);
        if (vdp_code & 0x20) {                      /* DMA */
            if (vdp_reg[1] & 0x10) {
                int mode = vdp_reg[23] >> 6;
                if (mode < 2) vdp_dma_68k();
                else if (mode == 2) vdp_dma_fill = 1;
                else vdp_dma_copy();
            }
        }
        if (!(vdp_code & 1) && !(vdp_code & 0x20)) vdp_rbuf = vdp_read_mem();
        return;
    }
    if ((v & 0xC000) == 0x8000) {
        int r = (v >> 8) & 0x1F;
        vdp_reg[r] = (u8)v;
        if (hw_log) fprintf(stderr, "VDP reg %d = %02X\n", r, v & 0xFF);
        update_irq();
        return;
    }
    vdp_pending = 1;
    vdp_code = (vdp_code & 0x3C) | ((v >> 14) & 3);
    vdp_addr = (vdp_addr & 0xC000) | (v & 0x3FFF);
}

static u16 vdp_status(void) {
    vdp_pending = 0;
    s64 inl = m68k_cycle - line_start;
    u16 s = 0x3400;                                     /* fixed bits: FIFO empty (bit9) */
    s = 0x0200 | (vdp_pal ? 1 : 0);
    if (vint_pending) s |= 0x80;
    if (vdp_line >= active_lines() || !(vdp_reg[1] & 0x40)) s |= 8;
    if (inl > 400) s |= 4;
    if (vdp_line & 1) s |= 0;
    return s;
}
static u16 vdp_hv(void) {
    int v = vdp_line;
    if (v > 0xEA) v = (v - (lines_total())) & 0xFF;      /* approx wrap */
    int h40 = vdp_reg[12] & 1;
    int px = (int)(((m68k_cycle - line_start) * (h40 ? 420 : 342)) / 488);
    int h = px >> 1;
    /* the H counter skips a range at the end of the line */
    if (h40) { if (h > 0xB6) h += 0xE4 - 0xB7; }
    else     { if (h > 0x93) h += 0xE9 - 0x94; }
    return (u16)(((v & 0xFF) << 8) | (h & 0xFF));
}

/* ------------------------------------------------------------------ pads */
static u8 pad_read(int port) {
    u8 ctrl = io_ctrl[port], d = io_data[port];
    u8 th = (d | ~ctrl) & 0x40;                   /* inputs read as 1 */
    u8 p = pad_state[port];
    u8 bits;
    if (th)  bits = ~(((p >> 0) & 1) | (((p >> 1) & 1) << 1) | (((p >> 2) & 1) << 2) | (((p >> 3) & 1) << 3) | (((p >> 4) & 1) << 4) | (((p >> 5) & 1) << 5)) & 0x3F; /* U D L R B C */
    else     bits = ~(((p >> 0) & 1) | (((p >> 1) & 1) << 1) | (((p >> 6) & 1) << 4) | (((p >> 7) & 1) << 5)) & 0x3F;                                                    /* U D . . A S */
    if (!th) bits &= ~0x0C;
    u8 v = (bits & 0x3F) | th;
    return (u8)((v & ~ctrl) | (d & ctrl));
}

/* ------------------------------------------------------------------ bus decode */
static void unmapped(const char *k, u32 a, u32 v) {
    static int n;
    if (n++ < 40) fprintf(stderr, "unmapped %s %06X = %X\n", k, a, v);
}

u8 hw_rd8(u32 a) {
    if (a >= 0x600000 && a < 0x600080) return wide_scratch[a - 0x600000];     /* 16:9 patch scratch (blank map data) */
    if (a >= 0x610000 && a < 0x611000) return wide_tilebuf[a - 0x610000];
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a < 0xA04000) return z80_ram[a & 0x1FFF];
        if (a < 0xA06000) { extern u8 ym_read(int); return ym_read(a & 3); }
        return 0xFF;
    }
    if (a >= 0xA10000 && a < 0xA11000) {
        switch (a & 0x1F) {
        case 0x01: return 0xA0 | (vdp_pal ? 0x40 : 0);           /* overseas, no expansion */
        case 0x03: return pad_read(0);
        case 0x05: return pad_read(1);
        case 0x07: return io_data[2];
        case 0x09: return io_ctrl[0];
        case 0x0B: return io_ctrl[1];
        case 0x0D: return io_ctrl[2];
        }
        return 0;
    }
    if (a >= 0xA11000 && a < 0xA12000) {
        if ((a & 0xFFFE) == 0x1100) return z80_busreq ? 0 : 1;      /* 0 = bus granted */
        return 0;
    }
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1F) {
        case 0: case 1: { u16 v = vdp_data_r(); return (a & 1) ? (u8)v : v >> 8; }
        case 4: case 5: { u16 v = vdp_status(); return (a & 1) ? (u8)v : v >> 8; }
        case 8: case 9: { u16 v = vdp_hv(); return (a & 1) ? (u8)v : v >> 8; }
        }
        return 0;
    }
    unmapped("rd8", a, 0);
    return 0xFF;
}
u16 hw_rd16(u32 a) {
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1F) {
        case 0: case 2: return vdp_data_r();
        case 4: case 6: return vdp_status();
        case 8: case 10: return vdp_hv();
        }
        return 0;
    }
    if (a >= 0xA11000 && a < 0xA12000 && (a & 0xFFFE) == 0x1100) return z80_busreq ? 0 : 0x0100;
    return (u16)((hw_rd8(a) << 8) | hw_rd8(a + 1));
}
u32 hw_rd32(u32 a) { return ((u32)hw_rd16(a) << 16) | hw_rd16(a + 2); }

void hw_wr8(u32 a, u8 v) {
    if (a >= 0x610000 && a < 0x611000) { wide_tilebuf[a - 0x610000] = v; return; }
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a < 0xA04000) { z80_ram[a & 0x1FFF] = v; return; }
        if (a < 0xA06000) { /* YM2612 */ extern void ym_write(int port, u8 v); ym_write(a & 3, v); return; }
        if (a >= 0xA06000 && a < 0xA06100) { z80_bank = ((z80_bank >> 1) | ((v & 1) << 8)) & 0x1FF; return; }
        if (a >= 0xA07F00) { extern void psg_write(u8 v); if ((a & 0xFF) >= 0x11 && (a & 1)) psg_write(v); return; }
        return;
    }
    if (a >= 0xA10000 && a < 0xA11000) {
        switch (a & 0x1F) {
        case 0x03: io_data[0] = v; return;
        case 0x05: io_data[1] = v; return;
        case 0x07: io_data[2] = v; return;
        case 0x09: io_ctrl[0] = v; return;
        case 0x0B: io_ctrl[1] = v; return;
        case 0x0D: io_ctrl[2] = v; return;
        }
        return;
    }
    if (a >= 0xA11000 && a < 0xA12000) {
        if ((a & 0xFFFE) == 0x1100) { z80_sync_to(m68k_cycle); z80_busreq = v & 1; }
        else if ((a & 0xFFFE) == 0x1200) { z80_sync_to(m68k_cycle); int nr = !(v & 1); if (z80_reset && !nr) { extern void z80_reset_cpu(void); z80_reset_cpu(); } z80_reset = nr; }
        return;
    }
    if (a >= 0xA14000 && a < 0xA14004) return;                    /* TMSS */
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1F) {
        case 0: case 1: case 2: case 3: vdp_data_w((u16)(v << 8 | v)); return;
        case 4: case 5: case 6: case 7: /* byte write to control: treat as word with duplicated byte */ vdp_ctrl_w((u16)(v << 8 | v)); return;
        case 0x11: case 0x13: case 0x15: case 0x17: { extern void psg_write(u8 v); psg_write(v); return; }
        }
        return;
    }
    unmapped("wr8", a, v);
}
void hw_wr16(u32 a, u16 v) {
    if (a >= 0xC00000 && a < 0xE00000) {
        switch (a & 0x1F) {
        case 0: case 2: vdp_data_w(v); return;
        case 4: case 6: vdp_ctrl_w(v); return;
        }
        return;
    }
    if (a >= 0xA11000 && a < 0xA12000) {
        if ((a & 0xFFFE) == 0x1100) { z80_sync_to(m68k_cycle); z80_busreq = (v >> 8) & 1; }
        else if ((a & 0xFFFE) == 0x1200) { z80_sync_to(m68k_cycle); int nr = !((v >> 8) & 1); if (z80_reset && !nr) { extern void z80_reset_cpu(void); z80_reset_cpu(); } z80_reset = nr; }
        return;
    }
    if (a >= 0xA00000 && a < 0xA02000) { z80_ram[a & 0x1FFE] = v >> 8; z80_ram[(a & 0x1FFE) + 1] = (u8)v; return; }
    hw_wr8(a, v >> 8); hw_wr8(a + 1, (u8)v);
}
void hw_wr32(u32 a, u32 v) {
    if (a >= 0xC00000 && a < 0xE00000 && (a & 0x1F) < 8) { hw_wr16(a, v >> 16); hw_wr16(a + 2, (u16)v); return; }
    hw_wr16(a, v >> 16); hw_wr16(a + 2, (u16)v);
}

/* mailbox access for the Z80 core */
u8 *hw_z80_ram(void) { return z80_ram; }
int hw_z80_running(void) { return !z80_reset && !z80_busreq; }
u32 hw_z80_bank(void) { return z80_bank; }

/* ------------------------------------------------------------------ scheduler */
void hw_events(void) {
    while (m68k_cycle >= line_start + line_len) {
        line_start += line_len;
        int total = lines_total();
        int act = active_lines();
        if (vdp_line < act) vdp_render_line(vdp_line);       /* the line that just finished */
        { extern void audio_advance(s64); extern void z80_set_int(int); 
          z80_sync_to(line_start); audio_advance(line_len);
          if (z80_int_cnt > 0 && --z80_int_cnt == 0) z80_set_int(0); }
        vdp_line++;
        /* horizontal interrupt counter */
        if (vdp_line <= act) {
            if (--hint_counter < 0) { hint_counter = vdp_reg[10]; hint_pending = 1; }
        } else hint_counter = vdp_reg[10];
        if (vdp_line == act) { vint_pending = 1; { extern void z80_set_int(int); z80_set_int(1); z80_int_cnt = 1; } }
        if (vdp_line == total) {
            vdp_line = 0; vdp_frame++; { extern void wide_frame_tick(void); wide_frame_tick(); }
            hint_counter = vdp_reg[10];
            host_frame();
        }
        update_irq();
    }
    m68k_next_event = line_start + line_len;
}

