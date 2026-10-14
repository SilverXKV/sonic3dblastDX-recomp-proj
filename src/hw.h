#ifndef HW_H
#define HW_H
#include "m68k_rt.h"
#include <setjmp.h>

/* VDP state exported for the renderer */
extern u8  vdp_vram[0x10000];
extern u8  vdp_vram_hi[0x10000];
extern u16 vdp_cram[64];
extern u16 vdp_vsram[40];
extern u8  vdp_reg[32];
extern int vdp_line;            /* current scanline */
extern int vdp_pal;             /* 1 = PAL */
extern int vdp_frame;
extern u8  pad_state[2];        /* active-high host input: U D L R B C A S, bit0..7 */

void vdp_render_line(int line); /* implemented by the renderer (or a stub) */
void host_frame(void);          /* end of frame: present video, flush audio, poll input */
void hw_init(void);
void hw_irq_ack(int level);
void m68k_exception(int vec);
void s3d_start(void);           /* reset and run */
void s3d_quit(void);
int  call_addr(u32 pc, u32 *ret);
extern jmp_buf s3d_quit_jmp;

/* audio hooks (Z80 / YM2612 / PSG) */
void z80_run(s64 cycles68k);
#endif
