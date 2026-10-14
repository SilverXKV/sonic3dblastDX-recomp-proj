#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include "funcs.h"

u32 D[8], A[8], m68k_other_sp;
u8 fX, fN, fZ, fV, fC, m68k_sr_hi;
s64 m68k_cycle, m68k_next_event;
int m68k_irq_level;
u8 rom_data[0x400000];
u8 ram_data[0x10000];
jmp_buf s3d_quit_jmp;

extern int call_addr_rom(u32 a, u32 *ret);
extern const struct ramimg { u32 ram; u32 rom; u32 len; } ramimgs[];
extern const int nramimgs;
extern const struct hbiimg { u32 rom; u32 len; } hbiimgs[];
extern const int nhbiimgs;

#ifdef S3D_TRACE_CALLS
u32 trc_ring[8192]; s64 trc_cyc[8192]; unsigned trc_pos;
#endif
#define DUMMY_RET 0x00DEAD00u

void m68k_fatal(const char *msg, u32 pc) {
    fprintf(stderr, "FATAL: %s (pc/arg %06X)  cycle=%lld line=%d frame=%d\n", msg, pc, (long long)m68k_cycle, vdp_line, vdp_frame);
    fprintf(stderr, " D:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", D[i]);
    fprintf(stderr, "\n A:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", A[i]);
    fprintf(stderr, "\n");
    for (int r = 0; r < 8; r += 2) { if (r == 0 || 1) { fprintf(stderr, " [A%d]:", r); for (int k = 0; k < 16; k++) fprintf(stderr, " %04X", rd16(A[r] + k * 2)); fprintf(stderr, "\n"); } }
#ifdef S3D_TRACE_CALLS
    { FILE *tf = fopen("c_trace.txt", "w"); for (int i = 0; i < 8192; i++) { unsigned k = trc_pos - 8192 + i; if ((int)k < 0) continue; fprintf(tf, "%lld %06X\n", (long long)trc_cyc[k & 8191], trc_ring[k & 8191]); } fclose(tf); }
#endif
    fflush(stderr);
    abort();
}

/* dispatch a code address (ROM function or RAM-resident copy of ROM code) */
int call_addr(u32 pc, u32 *ret) {
    pc &= 0xFFFFFF;
    if (pc < 0x400000) return call_addr_rom(pc, ret);
    if (pc >= 0xE00000) {
        u32 o = pc & 0xFFFF;
        for (int i = 0; i < nramimgs; i++)
            if (ramimgs[i].ram == (pc | 0xFF0000u) || ramimgs[i].ram == pc) {
                if (!memcmp(ram_data + o, rom_data + ramimgs[i].rom, ramimgs[i].len))
                    return call_addr_rom(ramimgs[i].rom, ret);
            }
        for (int i = 0; i < nhbiimgs; i++) {
            if (o + hbiimgs[i].len <= 0x10000 && !memcmp(ram_data + o, rom_data + hbiimgs[i].rom, hbiimgs[i].len))
                return call_addr_rom(hbiimgs[i].rom, ret);
        }
    }
    return 0;
}

void ramcode_check(u32 ramaddr) {
    for (int i = 0; i < nramimgs; i++)
        if (ramimgs[i].ram == ramaddr) {
            if (memcmp(ram_data + (ramaddr & 0xFFFF), rom_data + ramimgs[i].rom, ramimgs[i].len))
                m68k_fatal("RAM code differs from ROM image", ramaddr);
            return;
        }
    m68k_fatal("unknown RAM code address", ramaddr);
}

/* run code starting at pc until something returns to the dummy return address */
static void run_from(u32 pc) {
    for (;;) {
        u32 r;
        if (!call_addr(pc, &r)) m68k_fatal("jump to unknown code address", pc);
        if (r == DUMMY_RET) return;
        pc = r;          /* function returned somewhere else: continue there (e.g. the VBI rts trampoline) */
    }
}

void m68k_exception(int vec) {
    u32 sr = m68k_get_sr();
    m68k_set_sr((sr | 0x2000) & 0xFFFF);          /* enter supervisor mode (swaps stack if needed) */
    push32(DUMMY_RET);
    push16((u16)sr);
    CYC(44);
    run_from(rd32(vec * 4));
}

void m68k_call_vector(int vec) { run_from(rd32(vec * 4)); }

void m68k_zero_divide(void) { m68k_exception(5); }

void m68k_service_irq(void) {
    int lvl = m68k_irq_level;
    if (lvl <= IPL()) return;
    hw_irq_ack(lvl);
    u32 sr = m68k_get_sr();
    m68k_set_sr(((sr | 0x2000) & ~0x0700u) | ((u32)lvl << 8));
    push32(DUMMY_RET);
    push16((u16)sr);
    CYC(44);
    run_from(rd32((24 + lvl) * 4));
}

void s3d_quit(void) { longjmp(s3d_quit_jmp, 1); }

void s3d_start(void) {
    memset(ram_data, 0, sizeof ram_data);
    m68k_cycle = 0; m68k_next_event = 0; m68k_irq_level = 0;
    m68k_sr_hi = 0x27;
    A[7] = rd32(0);
    m68k_other_sp = 0;
    if (setjmp(s3d_quit_jmp)) return;
    run_from(rd32(4));
}

#ifdef S3D_WATCH
u32 watch_lo = 0xFFFFFFFF, watch_hi = 0;
void watch_hit(u32 a, u32 v, int sz) {
    static int n;
    { const char *wf = getenv("S3D_WATCHFROM"); if (wf && vdp_frame < atoi(wf)) return; } if (n++ > 60) return;
    fprintf(stderr, "WATCH wr%d %06X = %X  frame %d line %d  in f_%06X (cycle %lld)\n", sz, a & 0xFFFFFF, v, vdp_frame, vdp_line, trc_ring[(trc_pos - 1) & 8191], (long long)m68k_cycle);
}
#endif
