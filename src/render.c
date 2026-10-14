/* Scanline renderer for the Mega Drive VDP (planes A/B, window, sprites, priorities, scroll modes,
 * shadow/highlight).  Output: 32-bit XRGB framebuffer fb[240][320]. */
#include "hw.h"
#include <stdlib.h>

int wide_px, wide_fill, wide_dma_hi, wide_cm = 39; u32 wide_a6s;
u8 wide_scratch[128];
static int wide_latched;
int wide_ss;                         /* the special stage (256 px native) is running in widescreen */
#define SS_EXT   71                  /* 256 + 2*71 = 398 = 16:9 at 224 lines */
#define SS_JUNC  70                  /* distance from screen centre where the original picture resumes (rock walls move out by this) */
#define SS_AMP   35                  /* the filler between is a mirrored stretch of the water, +-35 px around the centre */
#define FB_STRIDE 448
#define WIDE_EXT  39                 /* extra pixels each side in widescreen: 320 + 2*39 = 398 = 16:9 at 224 lines */
u32 fb[240][FB_STRIDE];
int fb_w = 320, fb_h = 224;
int vdp_wide_enable;                 /* user setting: 0 = 4:3, 1 = 16:9 */
int vdp_wide_scene = 1;              /* set by the game-state detector: this scene is laid out for a wider view */

static u32 pal_rgb[3][64];      /* normal, shadow, highlight */
static u16 pal_src[64];
static int pal_valid;

static void update_pal(void) {
    static const u8 lv[8] = {0, 36, 73, 109, 146, 182, 219, 255};
    for (int i = 0; i < 64; i++) {
        u16 c = vdp_cram[i];
        int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
        pal_rgb[0][i] = (lv[r] << 16) | (lv[g] << 8) | lv[b];
        pal_rgb[1][i] = ((lv[r] >> 1) << 16) | ((lv[g] >> 1) << 8) | (lv[b] >> 1);
        int hr = 128 + (lv[r] >> 1), hg = 128 + (lv[g] >> 1), hb = 128 + (lv[b] >> 1);
        pal_rgb[2][i] = (hr << 16) | (hg << 8) | hb;
        pal_src[i] = c;
    }
}

static inline int tile_px_plane(u32 tile, int x, int y, int hflip, int vflip, int plane_a) {
    if (vflip) y = 7 - y;
    if (hflip) x = 7 - x;
    /* 16:9 patch: streamed ground tiles (index >= 347) live in the second bank */
    u8 b = (wide_fill && plane_a && tile >= 347) ? vdp_vram_hi[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF] : vdp_vram[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF];
    return (x & 1) ? (b & 15) : (b >> 4);
}
static inline int tile_px(u32 tile, int x, int y, int hflip, int vflip) {
    if (vflip) y = 7 - y;
    if (hflip) x = 7 - x;
    u8 b = vdp_vram[(tile * 32 + y * 4 + (x >> 1)) & 0xFFFF];
    return (x & 1) ? (b & 15) : (b >> 4);
}

void vdp_render_line(int line) {
    int h40 = vdp_reg[12] & 1;
    int W = h40 ? 320 : 256;
    int act = (vdp_reg[1] & 8) ? 240 : 224;
    if (line == 0) wide_latched = (wide_fill && h40) ? 1 : (wide_ss && !h40) ? 2 : 0;
    int ss = (wide_latched == 2) && !h40;                /* special stage */
    int ext = (wide_latched == 1 && h40) ? WIDE_EXT : ss ? SS_EXT : 0;
    fb_w = W + 2 * ext; fb_h = act;
    if (line >= act) return;
    for (int i = 0; i < 64; i++) if (pal_src[i] != vdp_cram[i]) { update_pal(); break; }
    if (!pal_valid) { update_pal(); pal_valid = 1; }
    int sh = (vdp_reg[12] & 8) != 0;
    int bd = ((vdp_reg[7] >> 4) & 3) * 16 + (vdp_reg[7] & 15);
    u32 *dst = fb[line] + ext;               /* dst[x] valid for x in [-ext, W+ext) */
    if (!(vdp_reg[1] & 0x40)) {
        for (int x = -ext; x < W + ext; x++) dst[x] = pal_rgb[0][bd];
        return;
    }
    /* per-pixel layer buffer: low 6 bits colour index (pal*16+c), bit6 priority, bit7 opaque, bit 8 = sprite-pal-3-special */
    u16 lineAb[FB_STRIDE], lineBb[FB_STRIDE], lineSb[FB_STRIDE];
    u16 *lineA = lineAb + ext, *lineB = lineBb + ext, *lineS = lineSb + ext;
    int pw = (vdp_reg[16] & 3), ph = (vdp_reg[16] >> 4) & 3;
    int cw = pw == 0 ? 32 : pw == 1 ? 64 : 128;
    int chh = ph == 0 ? 32 : ph == 1 ? 64 : 128;
    u32 nameA = (vdp_reg[2] & 0x38) << 10, nameB = (vdp_reg[4] & 7) << 13;
    u32 hs = (vdp_reg[13] & 0x3F) << 10;
    int hmode = vdp_reg[11] & 3, vmode = (vdp_reg[11] >> 2) & 1;
    int hsA, hsB;
    {
        u32 o = hs;
        if (hmode == 2) o += (line & ~7) * 4;
        else if (hmode == 3) o += line * 4;
        hsA = (vdp_vram[o & 0xFFFF] << 8 | vdp_vram[(o + 1) & 0xFFFF]) & 0x3FF;
        hsB = (vdp_vram[(o + 2) & 0xFFFF] << 8 | vdp_vram[(o + 3) & 0xFFFF]) & 0x3FF;
    }
    /* window region */
    int wx = vdp_reg[17] & 0x1F, wright = (vdp_reg[17] & 0x80) != 0;
    int wy = vdp_reg[18] & 0x1F, wdown = (vdp_reg[18] & 0x80) != 0;
    int win_line = wdown ? (line >= wy * 8) : (line < wy * 8);
    u32 nameW = (vdp_reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
    int wcw = h40 ? 64 : 32;
    for (int x = -ext; x < W + ext; x++) {
        int xc = x < 0 ? 0 : x >= W ? W - 1 : x;     /* clamped x for column-based features */
        int col = xc >> 4;
        /* ---- plane B */
        int vsB = vmode ? vdp_vsram[(col * 2 + 1) % 40] : vdp_vsram[1];
        int vsA = vmode ? vdp_vsram[(col * 2) % 40] : vdp_vsram[0];
        for (int pl = 0; pl < 2; pl++) {
            int hsc = pl ? hsB : hsA, vsc = pl ? vsB : vsA;
            u32 base = pl ? nameB : nameA;
            int px = (x - hsc) & (cw * 8 - 1);
            int py = (line + vsc) & (chh * 8 - 1);
            if (ss) {
                if (pl == 0) {
                    /* road: one strip on a transparent map; do not let it wrap around into the wider picture */
                    int sh = (((hsc & 0x3FF) + 128) & 255) - 128;
                    int xx = x - sh;
                    if (xx < 0 || xx >= 256) { lineA[x] = 0; continue; }
                    px = xx;
                } else {
                    /* background: rock walls at the screen edges, mirrored water between them */
                    int u = x - 128, au = u < 0 ? -u : u, xo;
                    if (au >= SS_JUNC) { int t = au - SS_JUNC; xo = 128 + (u < 0 ? -t : t); }
                    else { int t = SS_JUNC - au, m = t % (4 * SS_AMP); xo = 128 + (m <= SS_AMP ? m : m <= 3 * SS_AMP ? 2 * SS_AMP - m : m - 4 * SS_AMP); }
                    px = (xo - hsc) & (cw * 8 - 1);
                }
            }
            int inwin = 0;
            if (pl == 0) {
                int xin = wright ? (xc >= wx * 16) : (xc < wx * 16);
                if (win_line || xin) inwin = 1;
            }
            u32 te;
            int tx, ty;
            if (inwin) {
                tx = xc >> 3; ty = line >> 3;
                u32 o = nameW + (ty * wcw + tx) * 2;
                te = (vdp_vram[o & 0xFFFF] << 8) | vdp_vram[(o + 1) & 0xFFFF];
                px = xc; py = line;
            } else {
                tx = px >> 3; ty = py >> 3;
                u32 o = base + (ty * cw + tx) * 2;
                te = (vdp_vram[o & 0xFFFF] << 8) | vdp_vram[(o + 1) & 0xFFFF];
            }
            int c = tile_px_plane(te & 0x7FF, px & 7, py & 7, (te >> 11) & 1, (te >> 12) & 1, !inwin);   /* the ROM swaps plane A/B cells (priority trick), so pool tiles can sit in either plane */
            u16 v = 0;
            if (c) v = (u16)(((te >> 13) & 3) * 16 + c) | ((te & 0x8000) ? 0x40 : 0) | 0x80;
            if (pl) lineB[x] = v; else lineA[x] = v;
        }
        lineS[x] = 0;
    }
    /* ---- sprites */
    {
        u32 st = (vdp_reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
        int maxspr = h40 ? 80 : 64, maxline = h40 ? 20 : 16, maxpix = W;
        if (ext) { maxline = 40; maxpix = 2 * W; }          /* widescreen: more of the line is visible, relax the per-line limits */
        int idx = 0, n = 0, pixels = 0, hit_any = 0, masked = 0;
        struct { int x, w, tile, pal, pri, hf, vf, row; } L[40];
        int cnt = 0;
        for (int guard = 0; guard < maxspr; guard++) {
            u32 o = st + idx * 8;
            int y = ((vdp_vram[o & 0xFFFF] << 8) | vdp_vram[(o + 1) & 0xFFFF]) & 0x3FF;
            int sz = vdp_vram[(o + 2) & 0xFFFF];
            int link = vdp_vram[(o + 3) & 0xFFFF] & 0x7F;
            int sw = ((sz >> 2) & 3) + 1, shh = (sz & 3) + 1;
            int ry = line + 128 - y;
            if (ry >= 0 && ry < shh * 8) {
                if (cnt >= maxline) break;
                u32 te = (vdp_vram[(o + 4) & 0xFFFF] << 8) | vdp_vram[(o + 5) & 0xFFFF];
                int x = ((vdp_vram[(o + 6) & 0xFFFF] << 8) | vdp_vram[(o + 7) & 0xFFFF]) & 0x1FF;
                if (ss) { int tt = te & 0x7FF; if ((tt >= 0x7C2 && tt < 0x7E0) || (tt >= 0x1B && tt < 0x23)) x -= SS_EXT; }   /* HUD: lives, RINGS, ring count */
                L[cnt].x = x; L[cnt].w = sw;
                L[cnt].tile = te & 0x7FF; L[cnt].pal = (te >> 13) & 3; L[cnt].pri = (te >> 15) & 1;
                L[cnt].hf = (te >> 11) & 1; L[cnt].vf = (te >> 12) & 1;
                L[cnt].row = ry | (shh << 8);
                cnt++;
            }
            n++;
            idx = link;
            if (!link || idx >= maxspr) break;
        }
        (void)n; (void)hit_any; (void)masked;
        /* draw in order: earlier sprites win */
        for (int s = 0; s < cnt; s++) {
            int shh = L[s].row >> 8, ry = L[s].row & 0xFF;
            if (L[s].x == 0 && s > 0) break;             /* sprite mask */
            int yy = L[s].vf ? (shh * 8 - 1 - ry) : ry;
            int trow = yy >> 3, py = yy & 7;
            for (int cx = 0; cx < L[s].w * 8; cx++) {
                if (pixels >= maxpix) goto done_sprites;
                pixels++;
                int xx = L[s].hf ? (L[s].w * 8 - 1 - cx) : cx;
                int tcol = xx >> 3, px = xx & 7;
                int t = (L[s].tile + tcol * shh + trow) & 0x7FF;
                int c = tile_px(t, px, py, 0, 0);
                int sx = L[s].x - 128 + cx;
                if (sx < -ext || sx >= W + ext) continue;
                if (c && !lineS[sx]) lineS[sx] = (u16)(L[s].pal * 16 + c) | (L[s].pri ? 0x40 : 0) | 0x80;
            }
        }
    done_sprites:;
    }
    int hide_left = (vdp_reg[0] & 0x20) != 0;
    for (int x = -ext; x < W + ext; x++) {
        u16 a = lineA[x], b = lineB[x], s = lineS[x];
        int idx = bd, pri = 0, set = 0;
        /* priority order (low -> high): B0 A0 S0 B1 A1 S1 */
        if (b & 0x80) { idx = b & 63; pri = (b >> 6) & 1; set = 1; }
        if (a & 0x80) { if (!set || !(b & 0x40) || (a & 0x40)) { idx = a & 63; pri = (a >> 6) & 1; set = 1; } }
        int sprhere = 0;
        if (s & 0x80) {
            if (!set || (s & 0x40) || (!(((a & 0x80) && (a & 0x40)) || ((b & 0x80) && (b & 0x40))))) {
                int sc = s & 63;
                if (sh && (sc == 0x3E || sc == 0x3F)) {
                    /* shadow/highlight operators: palette 3 colours 14/15 */
                    sprhere = 0;
                    if (sc == 0x3E) { dst[x] = pal_rgb[2][idx]; goto next; }
                    dst[x] = pal_rgb[1][idx]; goto next;
                }
                idx = sc; pri = (s >> 6) & 1; sprhere = 1;
            }
        }
        /* the backdrop colour is not darkened by shadow mode (the TT logo's white screen relies on this) */
        if (sh && !set && !sprhere) dst[x] = pal_rgb[0][idx];
        else if (sh && !pri && !sprhere) dst[x] = pal_rgb[1][idx];
        else dst[x] = pal_rgb[0][idx];
        if (hide_left && x < 8) dst[x] = pal_rgb[0][bd];
    next:;
    }
}
