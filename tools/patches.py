"""Source-level patches applied while translating the ROM (16:9 support).

PRE[addr]   C lines run before the instruction at addr
POST[addr]  C lines run after it
IMM[addr]   C expression replacing the immediate operand of the instruction at addr
ADJ[addr]   per-operand format strings (operand order as read) that wrap the C expression of an operand read
All of them are inert unless the runtime variable wide_px is non-zero (wide_px = extra pixels on each side)."""
PRE = {}
POST = {}
IMM = {}
ADJ = {}

W = 'wide_px'

# ------------------------------------------------------------------ tile map streaming
# stage start (full screen fill): latch the display mode for this stage
PRE[0x2518] = ['wide_latch();']

# new column written when the camera crosses an 8px boundary: right edge a bit further out, left edge likewise
IMM[0x206A] = '(%s ? 0x24 : 0x1E)' % W           # addi.w #$1E,d7   (blocks*3)
IMM[0x206E] = '(%s ? 0x180 : 0x140)' % W         # addi.w #$140,d3  (pixels)
PRE[0x2072] = ['if (%s && (s16)rd16(A[6] + 8) < 0) { SETW(D[3], D[3] - 64); SETW(D[7], D[7] - 6); }' % W]
# the map rows are 256 blocks of 3 bytes: anything outside is blank
_blank = ['if (%s && ((s16)D[7] < 0 || (s16)D[7] >= 768)) A[0] = 0x600000;' % W]
POST[0x207A] = _blank
POST[0x2182] = _blank

# new row: 15 block groups starting two blocks left of the camera instead of 11 starting at it
PRE[0x2312] = ['''if (%s) {
    u32 xb = (u16)D[3] >> 5, base = A[0] - 3 * xb;
    for (int i = 0; i < 15; i++) { int b = (int)xb - 2 + i; for (int k = 0; k < 3; k++) wide_scratch[i * 3 + k] = (b >= 0 && b < 256) ? rd8(base + 3 * b + k) : 0; }
    A[0] = 0x600000; SETW(D[3], D[3] - 64);
  }''' % W]
IMM[0x2356] = '(%s ? 14 : 10)' % W               # move.w #$A,d1

# ------------------------------------------------------------------ creatures: culling windows
# RunCreatures: (x - camX) <= bound and the on-screen test, both widened on each side
ADJ[0x1D5A2] = ['(u16)(%s + 2 * (wide_px ? wide_cm : 0))', '(u16)(%s + (wide_px ? wide_cm : 0))']
ADJ[0x1D5C6] = ['(u16)(%s + 2 * (wide_px ? wide_cm : 0))', '(u16)(%s + (wide_px ? wide_cm : 0))']

# ================================================================== v2 tile streaming layout (54-column ring)
PRE[0x60BC] = ['wide_ingame_tick();']                      # IngameRun: the stage is running this frame

IMM[0x206E] = '(%s ? 376 : 0x140)' % W                     # right-hand column to write: x + 376 (was x + 320)
PRE[0x2072] = ['''if (%s) {
    if ((s16)rd16(A[6] + 8) < 0) SETW(D[3], D[3] - 48);     /* left-hand column: x - 48 */
    SETW(D[7], ((u16)D[3] >> 5) * 3);                       /* block offset inside the map row */
  }''' % W]
IMM[0x2356] = '(%s ? 15 : 10)' % W                          # row writer: 16 block groups
PRE[0x2312] = ['''if (%s) {
    u32 xb = (u16)D[3] >> 5, base = A[0] - 3 * xb;
    for (int i = 0; i < 16; i++) { int b = (int)xb - 2 + i; for (int k = 0; k < 3; k++) wide_scratch[i * 3 + k] = (b >= 0 && b < 256) ? rd8(base + 3 * b + k) : 0; }
    A[0] = 0x600000; SETW(D[3], D[3] - 64);
  }''' % W]
IMM[0x2356] = '(%s ? 15 : 10)' % W
# tile-ring pass of the row writer: 54 cells starting 6 columns left of the camera, staged at $610000
ADJ[0x2442] = ['(u16)(%%s - 6 * 8 * (%s != 0))' % W]
IMM[0x2462] = '(%s ? 0x34 : 0x27)' % W
IMM[0x246A] = '(%s ? 0x35 : 0x28)' % W
POST[0x2478] = ['if (%s) A[3] = 0x610000;' % W]
IMM[0x2504] = '(%s ? 0xFF610000u : 0xFFFF1C3Eu)' % W
IMM[0x250E] = '(%s ? 0x360 : 0x290)' % W
POST[0x250C] = ['if (%s) { D[2] = (D[2] & 0xFFFF) | 0x10000; wide_dma_hi = 1; }' % W]     # destination in the second video-memory bank
# column writer: ring stride 54 tiles, second bank
IMM[0x22BE] = '(%s ? 0x06C00000u : 0x05200000u)' % W
IMM[0x22B8] = '(%s ? 0x40000084u : 0x40000080u)' % W
IMM[0x733C] = '(wide_dma_hi ? 7 : 3)'                       # DMA setup: keep the A16 bit of the destination (only for the row writer)
POST[0x733C] = ['wide_dma_hi = 0;']
PRE[0x55B8] = ['wide_clear();']
PRE[0x1A9D4] = ['if (wide_px) wide_hud_fix(A[0]);']

# ------------------------------------------------------------------ v5: other places that assume a 320-pixel screen
# (a) the generic sprite-piece emitter (sub_1B17A): outer/inner window, then the clipped path's right/left limits
_CM = '(wide_px ? wide_cm : 0)'
_WIN = ['(u16)(%%s + 2 * %s)' % _CM, '(u16)(%%s + %s)' % _CM]
for _a in (0x1B184, 0x1B1AC):
    ADJ[_a] = list(_WIN)
for _a in (0x1B22C, 0x1B27C):
    IMM[_a] = '(0x1C0 + wide_px)'          # cmpi.w #$1C0,d0 : piece starts right of the screen
for _a in (0x1B244, 0x1B294):
    IMM[_a] = '(0x78 - wide_px)'           # cmpi.w #$78,d5  : piece ends left of the screen
# (b) object spawners / list builders: "(x - camX) <= window" with no margin on either side
for _a in (0x90A0, 0x8022, 0x8B3A, 0xA7D0, 0xC718, 0x11CCA, 0x67EC, 0xE408, 0x1B060):
    ADJ[_a] = list(_WIN)
# (c) Sonic's shadow and a few decoration lists that clip against VDP x 0x1C0 / 0x70
IMM[0x1AFB8] = '(0x1C0 + wide_px)'
IMM[0x1AFB2] = '(0x60 - wide_px)'
IMM[0x117BE] = '(0x1C0 + wide_px)'
for _a in (0xFD1A, 0x15C00):
    IMM[_a] = '(0x1C0 + wide_px)'
for _a in (0xFD14, 0x15BFA):
    IMM[_a] = '(0x70 - wide_px)'
# (d) the creature sprite emitter (sub_1D7EA): clip limits for sprites that straddle the screen edge
IMM[0x1D7EE] = '(0x1C0 + wide_px)'         # movea.w #$1C0,a4 : right limit
for _a in (0x1D91E, 0x1D968):
    IMM[_a] = '(0x78 - wide_px)'           # cmpi.w #$78,d5   : left limit

# ------------------------------------------------------------------ v6: rings / static items (sub_1B366 + shadows sub_1B6C6)
# They live in a per-32px-column map and are only handled for the ~10 columns of a 320px screen; the columns that scroll out of that
# window get switched off. In wide mode the window starts two columns further left and ends two columns further right (clamped to the table).
# the hide-column reads use a displaced a6 for the duration of one instruction
PRE[0x1B3D6] = ['if (%s) { wide_a6s = A[6]; A[6] -= 2; }' % W]                          # move.b -1(a6),d2   (column that just left on the left)
POST[0x1B3D6] = ['if (%s) A[6] = wide_a6s;' % W]
for _a in (0x1B3F4, 0x1B3FC):                                                         # move.b $A(a6)/$B(a6),d2 (column that just left on the right)
    PRE[_a] = ['if (%s) { wide_a6s = A[6]; A[6] += 2; if (((A[6] + 0xB) & 0xFFFF) > 0xE77B) A[6] = 0xFFFFE670u - 0xA; }' % W]
    POST[_a] = ['if (%s) A[6] = wide_a6s;' % W]
PRE[0x1B42A] = ['''if (%s) {
    u32 a6n = (A[6] - 2) & 0xFFFF; int d4n = (D[4] & 0xFFFF) + 4;
    if ((int)a6n + d4n > 0xE77B) d4n = 0xE77B - (int)a6n;
    if (d4n < 0) d4n = 0;
    A[6] = 0xFFFF0000u | a6n; SETW(D[4], d4n);
  }''' % W]
POST[0x1B430] = ['if (%s) SETW(D[1], D[1] - 0x40);' % W]                              # first column starts two columns further left
POST[0x1B6F8] = ['if (%s) SETW(D[1], D[1] - 0x40);' % W]                              # same for the shadows

# ------------------------------------------------------------------ v6.1: Sonic's own sprite (sub_1ABBC)
# When Sonic is hidden (damage flicker, transport ring, ...) the game parks him at screen x = -40, which is off-screen at 320 pixels
# but inside the wider picture. In wide mode park him much further out (VDP x 0x10: non-zero so the sprite-mask rule is not triggered)
# and widen his own on-screen window to match.
IMM[0x1ABF6] = '(u16)(wide_px ? 0xFF90 : 0xFFD8)'      # move.w #$FFD8,d0   hidden position
IMM[0x1ABDE] = '(u16)(0xFFD8 - wide_px)'              # cmpi.w #$FFD8,d0   left limit of his visible window
IMM[0x1ABE4] = '(0x168 + wide_px)'                    # cmpi.w #$168,d0    right limit

# ------------------------------------------------------------------ v8: entity activation margin
# Creatures only run (and request their sprite art) while they are inside the outer window of their bounds table. The earlier wide
# margin (64 px) was larger than the extra picture (39 px), so neighbours that share one block of video memory for their art (the
# two turrets, for example) ran at the same time and overwrote each other's art, producing garbled sprites. The margin is now exactly
# the extra width (wide_cm = 39 in render.c), i.e. the same activation distance the game has at 4:3.

# ------------------------------------------------------------------ v9: special stage
# sub_1FB1A runs a special stage (a 256-pixel-wide road scene).  Its title card (sub_1D9D2) stays as it is; the stage itself is drawn
# in 16:9 by the renderer (wide_ss in render.c, see widescreen.c) from the moment the card is done until the routine returns.
PRE[0x1FB1E] = ['wide_ss_start();']
PRE[0x1FF92] = ['wide_ss_end();']
