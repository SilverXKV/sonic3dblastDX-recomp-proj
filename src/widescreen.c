/* 16:9 support: run-time side of the widescreen patches.
 *
 * The game streams the ground tiles it needs into video memory in a ring of 41 tile columns x 29 rows.  For a
 * wider picture the ring is made 54 columns wide, which does not fit in the Mega Drive's 64K of video memory,
 * so the emulated VDP has a second 64K bank for it (selected with the otherwise unused A16 address bit).  The
 * translated code is patched (tools/patches.py) to use the larger ring; the ring layout lives in three small
 * ROM tables that are rewritten here, in memory, when a stage starts. */
#include "hw.h"

extern u8 rom_data[];
extern int vdp_wide_enable;
extern int wide_px, wide_fill;
extern int vdp_frame;

#define WIDE_EXT   39
#define SLOTS      54
#define TAB_COL    0x25C0      /* byte per map column: ring slot                    */
#define TAB_COL_N  0x428
#define TAB_ROW    0x29E8      /* word per map row: first tile index of the row's ring slots */
#define TAB_ROW_N  0x438

static u8 save_col[TAB_COL_N], save_row[TAB_ROW_N];
static int saved;
int wide_ingame_frame = -100;
int wide_stage_on;                 /* a stage is running (between its start and the next mode change) */

void wide_latch(void) {
    if (!saved) {
        for (int i = 0; i < TAB_COL_N; i++) save_col[i] = rom_data[TAB_COL + i];
        for (int i = 0; i < TAB_ROW_N; i++) save_row[i] = rom_data[TAB_ROW + i];
        saved = 1;
    }
    wide_fill = vdp_wide_enable;
    wide_stage_on = 1;
    wide_px = wide_fill ? WIDE_EXT : 0;
    wide_ingame_frame = vdp_frame;
    if (wide_fill) {
        for (int i = 0; i < TAB_COL_N; i++) rom_data[TAB_COL + i] = (u8)(i % SLOTS);
        for (int r = 0; r < TAB_ROW_N / 2; r++) {
            int v = 347 + SLOTS * (r % 29);
            rom_data[TAB_ROW + r * 2] = (u8)(v >> 8); rom_data[TAB_ROW + r * 2 + 1] = (u8)v;
        }
    } else if (saved) {
        for (int i = 0; i < TAB_COL_N; i++) rom_data[TAB_COL + i] = save_col[i];
        for (int i = 0; i < TAB_ROW_N; i++) rom_data[TAB_ROW + i] = save_row[i];
    }
}

/* ClearGameRAM runs at the start of every mode other than a stage (menus, bonus rounds, ...): back to 4:3 */
extern int wide_ss;
void wide_clear(void) { wide_fill = 0; wide_px = 0; wide_stage_on = 0; wide_ss = 0; }
/* Special stage (sub_1FB1A): widened once its title card is done, back to normal when the routine returns */
void wide_ss_start(void) { wide_ss = vdp_wide_enable; }
void wide_ss_end(void) { wide_ss = 0; }
void wide_ingame_tick(void) { }
void wide_frame_tick(void) { }

/* HUD: left-hand items move out to the new left edge, right-hand items to the new right edge.  Called once the HUD
 * sprites have been written to the sprite buffer ($FFBF98 up to 'end'); 8 bytes per sprite, x is the last word. */
void wide_hud_fix(u32 end) {
    extern u8 ram_data[];
    for (u32 a = 0xBF98; a + 8 <= (end & 0xFFFF) && a < 0xBF98 + 80 * 8; a += 8) {
        int x = (ram_data[a + 6] << 8) | ram_data[a + 7];
        if (x < 0x80 || x >= 0x1A0 + 0x100) continue;                  /* hidden / off screen */
        int col = x - 128;
        if (col < 110) x -= wide_px; else if (col >= 210) x += wide_px; else continue;
        ram_data[a + 6] = x >> 8; ram_data[a + 7] = (u8)x;
    }
}
