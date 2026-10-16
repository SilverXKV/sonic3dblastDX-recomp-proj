/* Optional software CRT look for the Windows front end: scanlines, a little horizontal softening and (when the picture is
 * large enough) a phosphor mask.  Pure C, no OS calls, so it is the same code on every platform.  The caller scales the
 * game picture into `dst` with it instead of letting the OS do the stretch; when the filter is off this is never called. */
#include <stdlib.h>
#include <string.h>

typedef unsigned int  u32;
typedef unsigned char u8;

static int c_w, c_h, c_dw, c_dh, c_str = -1;          /* the sizes the tables were built for */
static int *c_col0, *c_colf;              /* per output column: left source pixel and 0..255 blend to the next one */
static int *c_colm;                       /* per output column: 3 channel multipliers (mask * gain), 8.8 */
static int *c_rowsrc, *c_roww;            /* per output row: source line and brightness 0..256 */
static u32 *c_line;                       /* h lines of dw pixels: horizontally filtered + masked */

static void crt_free(void) {
    free(c_col0); free(c_colf); free(c_colm); free(c_rowsrc); free(c_roww); free(c_line);
    c_col0 = c_colf = c_colm = c_rowsrc = c_roww = NULL; c_line = NULL; c_w = c_h = c_dw = c_dh = 0; c_str = -1;
}

static int crt_build(int w, int h, int dw, int dh, int strength) {
    crt_free();
    c_col0 = (int *)malloc(sizeof(int) * dw); c_colf = (int *)malloc(sizeof(int) * dw); c_colm = (int *)malloc(sizeof(int) * 3 * dw);
    c_rowsrc = (int *)malloc(sizeof(int) * dh); c_roww = (int *)malloc(sizeof(int) * dh);
    c_line = (u32 *)malloc(sizeof(u32) * (size_t)dw * h);
    if (!c_col0 || !c_colf || !c_colm || !c_rowsrc || !c_roww || !c_line) { crt_free(); return 0; }
    double sx = (double)w / dw, sy = (double)h / dh;
    double rpl = (double)dh / h;                         /* output rows per source line */
    int mask = dw >= 3 * w;                              /* phosphor triads only when the picture is big enough to show them */
    /* strength 0..100 (50 = default): how dark the gap between two lines gets; no gap at all when the picture is too small */
    double amp = rpl < 1.5 ? 0.0 : strength / 100.0 * (rpl < 4.0 ? 1.0 : 1.2);
    if (amp > 1.0) amp = 1.0;
    double gain = (mask ? 1.30 : 1.12) * (amp > 0 ? 1.0 + 0.4 * (amp - 0.5) : 0.95);      /* the more is cut away, the more the rest is lifted */
    for (int x = 0; x < dw; x++) {
        double u = (x + 0.5) * sx - 0.5; if (u < 0) u = 0;
        int i = (int)u; if (i > w - 1) i = w - 1;
        c_col0[x] = i; c_colf[x] = (int)((u - i) * 256.0); if (c_colf[x] > 255) c_colf[x] = 255;
        for (int c = 0; c < 3; c++) {
            double m = mask ? ((x % 3) == c ? 1.0 : 0.62) : 1.0;
            c_colm[x * 3 + c] = (int)(m * gain * 256.0);
        }
    }
    for (int y = 0; y < dh; y++) {
        double u = (y + 0.5) * sy;                       /* source line coordinate of this output row's centre */
        int l = (int)u; if (l > h - 1) l = h - 1;
        double f = u - l;
        double centre = rpl < 3.0 ? 0.25 : 0.5;          /* with 2 rows per line one is bright and one dark */
        double d = (f - centre) / 0.5;
        double wt = 1.0 - amp * (d * d > 1.0 ? 1.0 : d * d);
        c_rowsrc[y] = l; c_roww[y] = (int)(wt * 256.0 + 0.5);
    }
    c_w = w; c_h = h; c_dw = dw; c_dh = dh; c_str = strength;
    return 1;
}

/* Scales `src` (w x h pixels, `stride` pixels per row, 0x00RRGGBB) into `dst` (dw x dh pixels, no padding) with the CRT look.
 * Returns 0 if memory could not be allocated (the caller then draws the plain picture). */
int crt_apply(const u32 *src, int stride, int w, int h, u32 *dst, int dw, int dh, int strength) {
    if (w < 1 || h < 1 || dw < w || dh < h) return 0;
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    if (w != c_w || h != c_h || dw != c_dw || dh != c_dh || strength != c_str) if (!crt_build(w, h, dw, dh, strength)) return 0;
    for (int y = 0; y < h; y++) {
        const u32 *s = src + (size_t)y * stride; u32 *o = c_line + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            u32 a = s[c_col0[x]], b = s[c_col0[x] + 1 < w ? c_col0[x] + 1 : w - 1];
            int f = c_colf[x];
            u32 rb = ((((a & 0xFF00FFu) * (u32)(256 - f)) + ((b & 0xFF00FFu) * (u32)f)) >> 8) & 0xFF00FFu;
            u32 g  = ((((a & 0x00FF00u) * (u32)(256 - f)) + ((b & 0x00FF00u) * (u32)f)) >> 8) & 0x00FF00u;
            int r = (int)(((rb >> 16) & 255) * c_colm[x * 3] >> 8), gg = (int)((g >> 8) * c_colm[x * 3 + 1] >> 8), bb = (int)((rb & 255) * c_colm[x * 3 + 2] >> 8);
            if (r > 255) r = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            o[x] = ((u32)r << 16) | ((u32)gg << 8) | (u32)bb;
        }
    }
    for (int y = 0; y < dh; y++) {
        const u32 *l = c_line + (size_t)c_rowsrc[y] * dw; u32 *o = dst + (size_t)y * dw; u32 wt = (u32)c_roww[y];
        for (int x = 0; x < dw; x++) {
            u32 p = l[x];
            o[x] = ((((p & 0xFF00FFu) * wt) >> 8) & 0xFF00FFu) | ((((p & 0x00FF00u) * wt) >> 8) & 0x00FF00u);
        }
    }
    return 1;
}
