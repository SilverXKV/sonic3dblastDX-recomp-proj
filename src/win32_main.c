/* Windows host for the translated Sonic 3D Blast Director's Cut.
 *   video : GDI (StretchDIBits), integer/aspect scaling, F11 fullscreen
 *   audio : waveOut, 48 kHz stereo, also paces the emulation
 *   input : keyboard + XInput pad (loaded dynamically, optional)
 *   display: 4:3 (original) or native 16:9, chosen in the Options window / F9, remembered in s3d.ini
 *   options: one Options window with Display / Controls / Audio tabs (modeless: the game keeps running behind it)
 * No external libraries.  Link: user32 gdi32 winmm comdlg32.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include "hw.h"

extern u32 fb[240][448];
extern int fb_w, fb_h;
extern s16 audio_out[];
extern int audio_count;

/* ------------------------------------------------------------------ MD5 (to verify the ROM) */
typedef struct { u32 s[4]; u64 n; u8 buf[64]; } Md5;
static const u32 K5[64] = {
 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
 0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
 0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
 0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const u8 R5[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                          4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
static void md5_block(Md5 *m, const u8 *p) {
    u32 w[16], a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
    for (int i = 0; i < 16; i++) w[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((u32)p[i*4+3] << 24);
    for (int i = 0; i < 64; i++) {
        u32 f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7*i) & 15; }
        u32 t = d; d = c; c = b;
        u32 x = a + f + K5[i] + w[g];
        b = b + ((x << R5[i]) | (x >> (32 - R5[i])));
        a = t;
    }
    m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}
static void md5(const u8 *data, size_t len, char *hex) {
    Md5 m = {{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, 0, {0}};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) md5_block(&m, data + i);
    u8 tail[128] = {0};
    size_t r = len - i;
    memcpy(tail, data + i, r);
    tail[r] = 0x80;
    size_t tl = (r < 56) ? 64 : 128;
    u64 bits = (u64)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 8 + k] = (u8)(bits >> (8 * k));
    md5_block(&m, tail);
    if (tl == 128) md5_block(&m, tail + 64);
    for (int k = 0; k < 16; k++) sprintf(hex + k * 2, "%02x", (m.s[k >> 2] >> (8 * (k & 3))) & 0xFF);
}

#define ROM_MD5  "742dd5c98d143ff6716800dee6a25dc5"
#define ROM_SIZE 4191682

/* ------------------------------------------------------------------ globals */
static HWND g_hwnd;
static int g_quit, g_fullscreen, g_fast, g_active = 1;
static BITMAPINFO g_bmi;
static WINDOWPLACEMENT g_wp = { sizeof(WINDOWPLACEMENT) };
static LARGE_INTEGER g_freq, g_t0;
static int g_frames_title;
extern int vdp_wide_enable, wide_fill, wide_stage_on;
static char g_ini[MAX_PATH];
static HMENU g_menu;
#define IDM_ASPECT43   101
#define IDM_ASPECT169  102
#define IDM_FULLSCREEN 103
#define IDM_EXIT       104
#define IDM_ABOUT      105
#define IDM_INTSCALE   106
#define IDM_CONTROLS   107
#define IDM_OPTIONS    108
static void opt_sync(void);
static int g_vol = 100;           /* master volume 0..100 */
static int g_gain = 4096;         /* g_vol as a 12-bit gain (squared curve) */
static int g_do_fs;               /* full screen requested from the Options window: done once it has closed */
static void set_volume(int v) { if (v < 0) v = 0; if (v > 100) v = 100; g_vol = v; g_gain = v * v * 4096 / 10000; }
static int g_intscale = 1;        /* prefer whole-number scaling when it fills most of the window */

static void ini_path(void) {
    char p[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    while (n > 0 && p[n - 1] != '\\' && p[n - 1] != '/') n--;
    p[n] = 0;
    snprintf(g_ini, sizeof g_ini, "%ss3d.ini", p);
}
static void ini_load(void) {
    char v[32];
    GetPrivateProfileStringA("Display", "Aspect", "16:9", v, sizeof v, g_ini);
    vdp_wide_enable = (strcmp(v, "4:3") != 0);
    g_intscale = GetPrivateProfileIntA("Display", "IntegerScale", 1, g_ini) != 0;
    set_volume(GetPrivateProfileIntA("Audio", "Volume", 100, g_ini));
}
static void ini_save(void) {
    WritePrivateProfileStringA("Display", "Aspect", vdp_wide_enable ? "16:9" : "4:3", g_ini);
    WritePrivateProfileStringA("Display", "IntegerScale", g_intscale ? "1" : "0", g_ini);
    { char v[16]; snprintf(v, sizeof v, "%d", g_vol); WritePrivateProfileStringA("Audio", "Volume", v, g_ini); }
}
static void menu_sync(void) { opt_sync(); }
static void resize_for_aspect(void) {
    if (g_fullscreen || IsZoomed(g_hwnd) || IsIconic(g_hwnd)) return;
    RECT r = {0, 0, vdp_wide_enable ? 796 : 640, 448};
    AdjustWindowRect(&r, GetWindowLong(g_hwnd, GWL_STYLE), g_menu != NULL);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void set_aspect(int wide) {
    vdp_wide_enable = wide;
    menu_sync(); ini_save(); resize_for_aspect();
    g_frames_title = 999;          /* refresh the title now */
}

/* ------------------------------------------------------------------ video */
static void present(HDC dc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    /* Square pixels.  Use a whole-number scale when it fills most of the window, otherwise fit the window. */
    int w = fb_w, h = fb_h;
    int s = cw / w < ch / h ? cw / w : ch / h;
    int dw, dh;
    int fit_w, fit_h;
    if (cw * h < ch * w) { fit_w = cw; fit_h = cw * h / w; } else { fit_h = ch; fit_w = ch * w / h; }
    if (g_intscale && s >= 1 && (w * s) * 100 >= fit_w * 88) { dw = w * s; dh = h * s; }
    else { dw = fit_w; dh = fit_h; }
    int x = (cw - dw) / 2, y = (ch - dh) / 2;
    if (x > 0 || y > 0) {
        HBRUSH br = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT r1 = {0, 0, cw, y}, r2 = {0, y + dh, cw, ch}, r3 = {0, y, x, y + dh}, r4 = {x + dw, y, cw, y + dh};
        FillRect(dc, &r1, br); FillRect(dc, &r2, br); FillRect(dc, &r3, br); FillRect(dc, &r4, br);
    }
    SetStretchBltMode(dc, COLORONCOLOR);
    /* The DIB height must equal the source height: with a taller buffer GDI measures the source rectangle from the
     * bottom of the bitmap, which cut the top 16 lines off in 224-line mode. The stride stays 320 pixels. */
    g_bmi.bmiHeader.biWidth = 448; g_bmi.bmiHeader.biHeight = -h;
    StretchDIBits(dc, x, y, dw, dh, 0, 0, w, h, fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
}

static void toggle_fullscreen(void) {
    DWORD st = GetWindowLong(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowPlacement(g_hwnd, &g_wp);
        if (GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(g_hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            SetMenu(g_hwnd, NULL);
            ShowCursor(FALSE);
            g_fullscreen = 1;
        }
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        SetMenu(g_hwnd, g_menu);
        ShowCursor(TRUE);
        g_fullscreen = 0;
    }
}


/* ------------------------------------------------------------------ key bindings (Controls menu / F8, saved in s3d.ini) */
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_A, A_B, A_C, A_START, A_FAST, NACT };
static const char *const act_name[NACT] = { "Up", "Down", "Left", "Right", "Button A", "Button B", "Button C", "Start", "Fast forward (hold)" };
static const char *const act_ini[NACT]  = { "Up", "Down", "Left", "Right", "A", "B", "C", "Start", "Fast" };
static const u8 act_bit[NACT] = { 0x01, 0x02, 0x04, 0x08, 0x40, 0x10, 0x20, 0x80, 0 };   /* Mega Drive pad bits */
static const int def_key[NACT][2] = {
    { VK_UP, 'W' }, { VK_DOWN, 'S' }, { VK_LEFT, 'A' }, { VK_RIGHT, 'D' },
    { 'Z', 'J' }, { 'X', 'K' }, { 'C', 'L' }, { VK_RETURN, VK_SPACE }, { VK_TAB, 0 } };
static const int def_pad[NACT] = { 0x0001, 0x0002, 0x0004, 0x0008, 0x4000, 0x1000, 0x2000, 0x0010, 0 };
static int g_key[NACT][2];
static int g_pad[NACT];
#define PADBIT_LT 0x10000
#define PADBIT_RT 0x20000

static void bind_defaults(void) {
    for (int a = 0; a < NACT; a++) { g_key[a][0] = def_key[a][0]; g_key[a][1] = def_key[a][1]; g_pad[a] = def_pad[a]; }
}
static void bind_load(void) {
    bind_defaults();
    for (int a = 0; a < NACT; a++) {
        char k[40];
        snprintf(k, sizeof k, "%s1", act_ini[a]); g_key[a][0] = GetPrivateProfileIntA("Controls", k, g_key[a][0], g_ini);
        snprintf(k, sizeof k, "%s2", act_ini[a]); g_key[a][1] = GetPrivateProfileIntA("Controls", k, g_key[a][1], g_ini);
        snprintf(k, sizeof k, "%sPad", act_ini[a]); g_pad[a] = GetPrivateProfileIntA("Controls", k, g_pad[a], g_ini);
    }
}
static void bind_save(void) {
    for (int a = 0; a < NACT; a++) {
        char k[40], v[16];
        snprintf(k, sizeof k, "%s1", act_ini[a]); snprintf(v, sizeof v, "%d", g_key[a][0]); WritePrivateProfileStringA("Controls", k, v, g_ini);
        snprintf(k, sizeof k, "%s2", act_ini[a]); snprintf(v, sizeof v, "%d", g_key[a][1]); WritePrivateProfileStringA("Controls", k, v, g_ini);
        snprintf(k, sizeof k, "%sPad", act_ini[a]); snprintf(v, sizeof v, "%d", g_pad[a]); WritePrivateProfileStringA("Controls", k, v, g_ini);
    }
}
static void key_name(int vk, char *out, int n) {
    if (!vk) { snprintf(out, n, "(none)"); return; }
    UINT sc = MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE: case VK_NUMLOCK: lp |= 1 << 24; break;
    }
    if (!sc || !GetKeyNameTextA(lp, out, n)) snprintf(out, n, "Key %d", vk);
}
static void pad_name(int m, char *out, int n) {
    static const struct { int b; const char *s; } t[] = {
        {0x0001, "D-pad Up"}, {0x0002, "D-pad Down"}, {0x0004, "D-pad Left"}, {0x0008, "D-pad Right"}, {0x0010, "Start"}, {0x0020, "Back"},
        {0x0040, "L-stick click"}, {0x0080, "R-stick click"}, {0x0100, "LB"}, {0x0200, "RB"}, {0x1000, "A"}, {0x2000, "B"},
        {0x4000, "X"}, {0x8000, "Y"}, {PADBIT_LT, "LT"}, {PADBIT_RT, "RT"} };
    if (!m) { snprintf(out, n, "(none)"); return; }
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].b == m) { snprintf(out, n, "Pad %s", t[i].s); return; }
    snprintf(out, n, "Pad %d", m);
}

/* --- the Controls window: a small modal window with one button per binding; click one, then press the key / pad button */
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad gp; } XState;
typedef DWORD (WINAPI *XGetStateFn)(DWORD, XState *);
static XGetStateFn xinput_get;
static int pad_mask_now(void) {
    XState s;
    if (!xinput_get || xinput_get(0, &s) != 0) return 0;
    int b = s.gp.wButtons;
    if (s.gp.bLT > 100) b |= PADBIT_LT;
    if (s.gp.bRT > 100) b |= PADBIT_RT;
    return b;
}

#define IDC_SLOT0   1000        /* + action*3 + column */
#define IDC_RESET   1100
#define IDC_CLOSE   1101
#define IDC_R43     1110
#define IDC_R169    1111
#define IDC_INT     1112
#define IDC_FS      1113
#define IDC_VOL     1120
#define IDC_TAB     1130
enum { PG_DISPLAY, PG_CONTROLS, PG_AUDIO, NPG };
#define MAXPG 64
static HWND g_cw, g_tab, g_slot[NACT][3], g_pg[NPG][MAXPG], g_r43, g_r169, g_chk_int, g_vol_tb, g_vol_txt;
static int g_pgn[NPG];
static int g_page = PG_DISPLAY;  /* tab that was last shown */
static int g_cap = -1;           /* slot being captured: action*3 + column */
static int g_cap_base;           /* pad buttons held when the capture started (ignored until released) */

static void cw_refresh(void) {
    if (!g_cw) return;
    for (int a = 0; a < NACT; a++) for (int c = 0; c < 3; c++) {
        char t[64];
        if (g_cap == a * 3 + c) snprintf(t, sizeof t, c < 2 ? "press a key..." : "press a pad button...");
        else if (c < 2) key_name(g_key[a][c], t, sizeof t);
        else pad_name(g_pad[a], t, sizeof t);
        SetWindowTextA(g_slot[a][c], t);
    }
}
static void opt_sync(void) {               /* bring the Options window's controls in line with the settings */
    if (!g_cw) return;
    SendMessage(g_r43, BM_SETCHECK, vdp_wide_enable ? BST_UNCHECKED : BST_CHECKED, 0);
    SendMessage(g_r169, BM_SETCHECK, vdp_wide_enable ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g_chk_int, BM_SETCHECK, g_intscale ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g_vol_tb, TBM_SETPOS, TRUE, g_vol);
    char t[32]; snprintf(t, sizeof t, g_vol ? "%d%%" : "%d%%  (silent)", g_vol);
    SetWindowTextA(g_vol_txt, t);
}
static void cw_assign_key(int vk) {
    int a = g_cap / 3, c = g_cap % 3;
    if (vk == VK_BACK || vk == VK_DELETE) vk = 0;      /* clear the slot */
    if (vk) for (int i = 0; i < NACT; i++) for (int j = 0; j < 2; j++) if (g_key[i][j] == vk) g_key[i][j] = 0;
    g_key[a][c] = vk;
    g_cap = -1; bind_save(); cw_refresh();
}
static void cw_assign_pad(int m) {
    int a = g_cap / 3;
    if (m) for (int i = 0; i < NACT; i++) if (g_pad[i] == m) g_pad[i] = 0;
    g_pad[a] = m;
    g_cap = -1; bind_save(); cw_refresh();
}
static void show_page(int pg) {
    g_page = pg;
    for (int p = 0; p < NPG; p++) for (int i = 0; i < g_pgn[p]; i++) ShowWindow(g_pg[p][i], p == pg ? SW_SHOW : SW_HIDE);
    g_cap = -1; cw_refresh();
}
static LRESULT CALLBACK cw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h, 1); g_cw = NULL; g_cap = -1; return 0;
    case WM_TIMER:
        if (g_cap >= 0 && g_cap % 3 == 2) {
            int now = pad_mask_now();
            g_cap_base &= now;                          /* released buttons stop being ignored */
            int fresh = now & ~g_cap_base;
            if (fresh) { int bit = fresh & -fresh; cw_assign_pad(bit); }
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(g_tab));
        return 0; }
    case WM_HSCROLL:
        if ((HWND)l == g_vol_tb) { set_volume((int)SendMessage(g_vol_tb, TBM_GETPOS, 0, 0)); opt_sync(); if (LOWORD(w) == TB_ENDTRACK) ini_save(); }   /* heard live, saved when the slider is let go */
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= IDC_SLOT0 && id < IDC_SLOT0 + NACT * 3) {
            g_cap = id - IDC_SLOT0; g_cap_base = pad_mask_now();
            if (g_cap % 3 == 2 && !xinput_get) g_cap = -1;
            cw_refresh(); SetFocus(h); return 0;
        }
        switch (id) {
        case IDC_RESET: bind_defaults(); g_cap = -1; bind_save(); cw_refresh(); return 0;
        case IDC_R43: set_aspect(0); return 0;
        case IDC_R169: set_aspect(1); return 0;
        case IDC_INT: g_intscale = SendMessage(g_chk_int, BM_GETCHECK, 0, 0) == BST_CHECKED; ini_save(); InvalidateRect(g_hwnd, NULL, FALSE); return 0;
        case IDC_FS: g_do_fs = 1; DestroyWindow(h); return 0;       /* the Options window closes first, then full screen is toggled */
        case IDC_CLOSE: case IDCANCEL: DestroyWindow(h); return 0;
        }
        break; }
    }
    return DefWindowProcA(h, m, w, l);
}
static int fix_vk(int vk, LPARAM l) {
    int ext = (l >> 24) & 1, sc = (l >> 16) & 0xFF;
    if (vk == VK_SHIFT) return sc == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return vk;
}
/* Called for every message pulled from the queue: the Options window is modeless, so its keyboard handling lives here.
 * Returns 1 when the message was consumed. */
static HWND g_hw;
static int options_filter(MSG *msg) {
    if (g_hw && (msg->hwnd == g_hw || IsChild(g_hw, msg->hwnd))) return IsDialogMessage(g_hw, msg) ? 1 : 0;
    if (!g_cw) return 0;
    if (msg->hwnd != g_cw && !IsChild(g_cw, msg->hwnd)) return 0;
    if (g_cap >= 0) {                                   /* waiting for a key / pad button to bind */
        if (g_cap % 3 < 2 && (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN)) {
            int vk = fix_vk((int)msg->wParam, msg->lParam);
            if (vk == VK_ESCAPE) { g_cap = -1; cw_refresh(); }
            else cw_assign_key(vk);
            return 1;
        }
        if (msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP || msg->message == WM_CHAR || msg->message == WM_SYSCHAR) return 1;
        if (g_cap % 3 == 2 && msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) { g_cap = -1; cw_refresh(); return 1; }
    }
    return IsDialogMessage(g_cw, msg) ? 1 : 0;
}
static HFONT g_font, g_font_link;
static void make_fonts(void) {
    if (g_font) return;
    HDC sdc = GetDC(NULL); int dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    int ht = -MulDiv(10, dpi, 72);
    g_font = CreateFontA(ht, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_font_link = CreateFontA(ht, 0, 0, 0, FW_NORMAL, 0, 1, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!g_font_link) g_font_link = g_font;
}
static void init_common_controls(void) {
    typedef BOOL (WINAPI *InitFn)(const INITCOMMONCONTROLSEX *);
    HMODULE cc = LoadLibraryA("comctl32.dll");
    InitFn f = cc ? (InitFn)GetProcAddress(cc, "InitCommonControlsEx") : NULL;
    INITCOMMONCONTROLSEX ic; ic.dwSize = sizeof ic; ic.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    if (f) f(&ic);
}
/* The Options window: Display / Controls / Audio tabs.  page < 0 = the tab that was open last. */
static void options_window(int page) {
    static int reg;
    if (page < 0) page = g_page;
    if (g_cw) { SetForegroundWindow(g_cw); TabCtrl_SetCurSel(g_tab, page); show_page(page); return; }
    HINSTANCE hi = GetModuleHandle(NULL);
    init_common_controls(); make_fonts();
    if (!reg) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = cw_proc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "S3DOptions"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassA(&wc); reg = 1;
    }
    const int CW = 664, CH = 520;
    RECT r = {0, 0, CW, CH};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr; GetWindowRect(g_hwnd, &pr);
    int x = pr.left + 30, y = pr.top + 30;
    g_cw = CreateWindowExA(WS_EX_DLGMODALFRAME, "S3DOptions", "Options", WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                           x, y, r.right - r.left, r.bottom - r.top, g_hwnd, NULL, hi, NULL);
    if (!g_cw) return;
    HFONT f = g_font;
    memset(g_pgn, 0, sizeof g_pgn);
    g_tab = CreateWindowA("SysTabControl32", "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 8, 8, CW - 16, 464, g_cw, (HMENU)(UINT_PTR)IDC_TAB, hi, NULL);
    SendMessage(g_tab, WM_SETFONT, (WPARAM)f, TRUE);
    { static const char *const names[NPG] = { "Display", "Controls", "Audio" };
      for (int i = 0; i < NPG; i++) { TCITEMA ti; memset(&ti, 0, sizeof ti); ti.mask = TCIF_TEXT; ti.pszText = (LPSTR)names[i]; SendMessage(g_tab, TCM_INSERTITEMA, i, (LPARAM)&ti); } }
    /* MK: a child of the Options window that belongs to tab `pg` */
    #define MK(hw, pg, cls, txt, st, X, Y, Wd, Ht, id) do { hw = CreateWindowA(cls, txt, WS_CHILD | (st), X, Y, Wd, Ht, g_cw, (HMENU)(UINT_PTR)(id), hi, NULL); SendMessage(hw, WM_SETFONT, (WPARAM)f, TRUE); g_pg[pg][g_pgn[pg]++] = hw; } while (0)
    HWND t;
    const int ox = 24;
    /* --- Display */
    MK(t, PG_DISPLAY, "STATIC", "Picture", 0, ox, 56, 300, 22, 0);
    MK(g_r43, PG_DISPLAY, "BUTTON", "4:3  (Original)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, ox + 12, 84, 320, 26, IDC_R43);
    MK(g_r169, PG_DISPLAY, "BUTTON", "16:9  (Native Widescreen)", BS_AUTORADIOBUTTON, ox + 12, 114, 320, 26, IDC_R169);
    MK(t, PG_DISPLAY, "STATIC", "The change is applied when the next stage starts.", 0, ox + 12, 148, 560, 22, 0);
    MK(t, PG_DISPLAY, "STATIC", "Window", 0, ox, 196, 300, 22, 0);
    MK(g_chk_int, PG_DISPLAY, "BUTTON", "Whole-number scaling", BS_AUTOCHECKBOX | WS_TABSTOP, ox + 12, 224, 360, 26, IDC_INT);
    MK(t, PG_DISPLAY, "BUTTON", "Full screen   (F11)", BS_PUSHBUTTON | WS_TABSTOP, ox + 12, 262, 200, 32, IDC_FS);
    /* --- Controls */
    MK(t, PG_CONTROLS, "STATIC", "Click a box, then press the key (or pad button) you want.  Backspace / Delete clears a key.", 0, ox, 50, 610, 22, 0);
    MK(t, PG_CONTROLS, "STATIC", "Action", 0, ox, 78, 150, 22, 0);
    MK(t, PG_CONTROLS, "STATIC", "Key 1", 0, ox + 162, 78, 140, 22, 0);
    MK(t, PG_CONTROLS, "STATIC", "Key 2", 0, ox + 314, 78, 140, 22, 0);
    MK(t, PG_CONTROLS, "STATIC", "Gamepad button", 0, ox + 466, 78, 140, 22, 0);
    for (int a = 0; a < NACT; a++) {
        int yy = 102 + a * 33;
        MK(t, PG_CONTROLS, "STATIC", act_name[a], 0, ox, yy + 5, 160, 22, 0);
        for (int c = 0; c < 3; c++) MK(g_slot[a][c], PG_CONTROLS, "BUTTON", "", BS_PUSHBUTTON | WS_TABSTOP, ox + 162 + c * 152, yy, 146, 29, IDC_SLOT0 + a * 3 + c);
    }
    { int by = 102 + NACT * 33 + 8;
      MK(t, PG_CONTROLS, "BUTTON", "Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, ox, by, 160, 32, IDC_RESET);
      MK(t, PG_CONTROLS, "STATIC", "Directions also work with the left stick. The game ignores the controls while this window is open.", 0, ox, by + 40, 610, 22, 0); }
    /* --- Audio */
    MK(t, PG_AUDIO, "STATIC", "Master volume", 0, ox, 56, 300, 22, 0);
    g_vol_tb = CreateWindowA("msctls_trackbar32", "", WS_CHILD | TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, ox + 12, 84, 440, 38, g_cw, (HMENU)(UINT_PTR)IDC_VOL, hi, NULL);
    g_pg[PG_AUDIO][g_pgn[PG_AUDIO]++] = g_vol_tb;
    SendMessage(g_vol_tb, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessage(g_vol_tb, TBM_SETTICFREQ, 10, 0);
    SendMessage(g_vol_tb, TBM_SETPAGESIZE, 0, 10);
    SendMessage(g_vol_tb, TBM_SETLINESIZE, 0, 1);
    MK(g_vol_txt, PG_AUDIO, "STATIC", "100%", 0, ox + 466, 92, 130, 22, 0);
    /* --- always visible */
    t = CreateWindowA("BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, CW - 8 - 120, 480, 120, 32, g_cw, (HMENU)(UINT_PTR)IDC_CLOSE, hi, NULL);
    SendMessage(t, WM_SETFONT, (WPARAM)f, TRUE);
    #undef MK
    g_cap = -1;
    TabCtrl_SetCurSel(g_tab, page);
    opt_sync();
    show_page(page);
    SetTimer(g_cw, 1, 30, NULL);
    SetForegroundWindow(g_cw);
}

/* ------------------------------------------------------------------ Help window: the function hotkeys + a link */
#define IDC_LINK 1140
#define HELP_URL "https://github.com/SilverXKV"
static HWND g_link;
static LRESULT CALLBACK hw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_hw = NULL; g_link = NULL; return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_link) { SetTextColor((HDC)w, RGB(0, 0, 238)); SetBkMode((HDC)w, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_SETCURSOR:
        if ((HWND)w == g_link) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_LINK: {
            typedef HINSTANCE (WINAPI *ShellExecFn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
            HMODULE sh = LoadLibraryA("shell32.dll");
            ShellExecFn se = sh ? (ShellExecFn)GetProcAddress(sh, "ShellExecuteA") : NULL;
            if (se) se(h, "open", HELP_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0; }
        case IDC_CLOSE: case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}
static void help_window(void) {
    static int reg;
    if (g_hw) { SetForegroundWindow(g_hw); return; }
    HINSTANCE hi = GetModuleHandle(NULL);
    make_fonts();
    if (!reg) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = hw_proc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "S3DHelp"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassA(&wc); reg = 1;
    }
    const int CW = 470, CH = 322;
    RECT r = {0, 0, CW, CH};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT pr; GetWindowRect(g_hwnd, &pr);
    g_hw = CreateWindowExA(WS_EX_DLGMODALFRAME, "S3DHelp", "Help", WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
                           pr.left + 40, pr.top + 40, r.right - r.left, r.bottom - r.top, g_hwnd, NULL, hi, NULL);
    if (!g_hw) return;
    #define MKH(hw, cls, txt, st, X, Y, Wd, Ht, id) do { hw = CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | (st), X, Y, Wd, Ht, g_hw, (HMENU)(UINT_PTR)(id), hi, NULL); SendMessage(hw, WM_SETFONT, (WPARAM)g_font, TRUE); } while (0)
    HWND t;
    static const char *const keys[][2] = {
        { "F8", "Options  (Controls tab)" },
        { "F9", "Switch 4:3 / 16:9" },
        { "F11  or  Alt+Enter", "Full screen" },
        { "Tab  (hold)", "Fast forward  (default key)" },
        { "Esc", "Quit" } };
    MKH(t, "STATIC", "Function hotkeys", 0, 22, 16, 420, 24, 0);
    for (int i = 0; i < 5; i++) {
        MKH(t, "STATIC", keys[i][0], 0, 36, 52 + i * 32, 170, 24, 0);
        MKH(t, "STATIC", keys[i][1], 0, 214, 52 + i * 32, 240, 24, 0);
    }
    MKH(t, "STATIC", "More information:", 0, 22, 232, 150, 24, 0);
    MKH(g_link, "STATIC", HELP_URL, SS_NOTIFY, 172, 232, 280, 24, IDC_LINK);
    SendMessage(g_link, WM_SETFONT, (WPARAM)g_font_link, TRUE);
    MKH(t, "BUTTON", "Close", BS_DEFPUSHBUTTON | WS_TABSTOP, CW - 22 - 110, 272, 110, 32, IDC_CLOSE);
    #undef MKH
    SetForegroundWindow(g_hw);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: case WM_DESTROY: g_quit = 1; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); present(dc); EndPaint(h, &ps); return 0; }
    case WM_ACTIVATE: g_active = LOWORD(w) != WA_INACTIVE; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_ESCAPE) { g_quit = 1; return 0; }
        if (w == VK_F8) { if (!(l & (1 << 30))) options_window(PG_CONTROLS); return 0; }
        if (w == VK_F9) { if (!(l & (1 << 30))) set_aspect(!vdp_wide_enable); return 0; }
        if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) { if (!(l & (1 << 30))) toggle_fullscreen(); return 0; }
        if (m == WM_SYSKEYDOWN && w != VK_F10) break;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_OPTIONS: options_window(-1); return 0;
        case IDM_ABOUT: help_window(); return 0;
        }
        break;
    case WM_SYSCOMMAND: if ((w & 0xFFF0) == SC_KEYMENU) return 0; break;
    }
    return DefWindowProc(h, m, w, l);
}

/* ------------------------------------------------------------------ input */
static void init_input(void) {
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput1_3.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) xinput_get = (XGetStateFn)GetProcAddress(m, "XInputGetState");
}
#define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
static void poll_input(void) {
    u8 p = 0;
    g_fast = 0;
    if (g_active && !g_cw) {
        int pm = pad_mask_now(), have_pad = xinput_get != NULL;
        for (int a = 0; a < NACT; a++) {
            int on = (g_key[a][0] && KD(g_key[a][0])) || (g_key[a][1] && KD(g_key[a][1])) || (g_pad[a] && (pm & g_pad[a]));
            if (a == A_FAST) g_fast = on; else if (on) p |= act_bit[a];
        }
        if (have_pad) {
            XState s;
            if (xinput_get(0, &s) == 0) {
                if (s.gp.ly > 16000) p |= 1;
                if (s.gp.ly < -16000) p |= 2;
                if (s.gp.lx < -16000) p |= 4;
                if (s.gp.lx > 16000) p |= 8;
            }
        }
    }
    pad_state[0] = p;
}

/* ------------------------------------------------------------------ audio */
#define OUT_RATE 48000
#define NBUF 4
#define BUFSAMP 960
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static s16 g_abuf[NBUF][BUFSAMP * 2];
static int g_wo_ok;
#define RINGN 16384
static s16 g_ring[RINGN * 2];
static int g_rhead, g_rcount;
static double g_rpos;               /* fractional read position into the native stream */
static s16 g_prevL, g_prevR;

static void audio_open(void) {
    WAVEFORMATEX f; memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = OUT_RATE; f.wBitsPerSample = 16;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = OUT_RATE * 4;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    for (int i = 0; i < NBUF; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_abuf[i]; g_hdr[i].dwBufferLength = BUFSAMP * 4;
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].dwFlags |= WHDR_DONE;
    }
    g_wo_ok = 1;
}
static void ring_put(s16 l, s16 r) {
    if (g_rcount >= RINGN) return;
    int i = (g_rhead + g_rcount) % RINGN;
    g_ring[i * 2] = l; g_ring[i * 2 + 1] = r; g_rcount++;
}
static void resample_in(void) {
    const double step = 53267.0 / OUT_RATE;       /* native samples per output sample */
    static int primed;
    if (!primed) { g_prevL = g_prevR = 0; primed = 1; }
    /* g_rpos is relative to the start of the current native block; the previous sample is carried over */
    int n = audio_count;
    while (g_rpos < n) {
        int i = (int)g_rpos; double fr = g_rpos - i;
        s16 l0, r0, l1, r1;
        if (i == 0) { l0 = g_prevL; r0 = g_prevR; l1 = audio_out[0]; r1 = audio_out[1]; }
        else { l0 = audio_out[(i - 1) * 2]; r0 = audio_out[(i - 1) * 2 + 1]; l1 = audio_out[i * 2]; r1 = audio_out[i * 2 + 1]; }
        ring_put((s16)(((int)(l0 + (l1 - l0) * fr) * g_gain) >> 12), (s16)(((int)(r0 + (r1 - r0) * fr) * g_gain) >> 12));
        g_rpos += step;
    }
    g_rpos -= n;
    if (n > 0) { g_prevL = audio_out[(n - 1) * 2]; g_prevR = audio_out[(n - 1) * 2 + 1]; }
}
static void audio_pump(void) {
    if (!g_wo_ok) return;
    for (int i = 0; i < NBUF; i++) {
        if (!(g_hdr[i].dwFlags & WHDR_DONE)) continue;
        if (g_rcount < BUFSAMP) return;
        for (int k = 0; k < BUFSAMP; k++) {
            int j = (g_rhead + k) % RINGN;
            g_abuf[i][k * 2] = g_ring[j * 2]; g_abuf[i][k * 2 + 1] = g_ring[j * 2 + 1];
        }
        g_rhead = (g_rhead + BUFSAMP) % RINGN; g_rcount -= BUFSAMP;
        g_hdr[i].dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
    }
}

/* ------------------------------------------------------------------ frame hook */
static void pump_messages(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g_quit = 1;
        if (options_filter(&msg)) continue;
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
}

void host_frame(void) {
    static int shown;
    pump_messages();
    if (g_quit) s3d_quit();
    poll_input();
    HDC dc = GetDC(g_hwnd);
    present(dc);
    ReleaseDC(g_hwnd, dc);
    if (g_do_fs) { g_do_fs = 0; toggle_fullscreen(); }
    if (g_fast) { audio_count = 0; return; }
    resample_in();
    audio_count = 0;
    audio_pump();
    if (g_wo_ok) {
        /* pace: wait while the sound card still has plenty queued */
        while (g_rcount >= BUFSAMP * 2 && !g_quit) {
            Sleep(1);
            audio_pump();
            pump_messages();
        }
    } else {
        /* no audio device: pace with the performance counter */
        static LARGE_INTEGER last;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (last.QuadPart) {
            double target = (vdp_pal ? 1.0 / 50.0 : 1.0 / 59.92) * g_freq.QuadPart;
            while ((double)(now.QuadPart - last.QuadPart) < target) { Sleep(0); QueryPerformanceCounter(&now); }
        }
        last = now;
    }
    if (++g_frames_title >= 60) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double secs = (double)(now.QuadPart - g_t0.QuadPart) / g_freq.QuadPart;
        char t[160];
        snprintf(t, sizeof t, "Sonic 3D Blast: Directors Cut Recompiled  [%s%s]  %.1f fps", vdp_wide_enable ? "16:9" : "4:3",
                 (wide_stage_on && (vdp_wide_enable != 0) != (wide_fill != 0)) ? ", applies when the next stage starts" : "",
                 g_frames_title / secs);
        SetWindowTextA(g_hwnd, t);
        g_t0 = now; g_frames_title = 0; shown = 1;
    }
    (void)shown;
}

/* ------------------------------------------------------------------ start-up */
static int load_rom(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static u8 buf[0x400000 + 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n != ROM_SIZE) {
        char m[300]; snprintf(m, sizeof m, "This ROM is %u bytes; the translated game needs the exact %u-byte Director's Cut ROM.", (unsigned)n, ROM_SIZE);
        MessageBoxA(NULL, m, "Wrong ROM", MB_ICONERROR); return -1;
    }
    char hex[40]; md5(buf, n, hex);
    if (strcmp(hex, ROM_MD5)) {
        char m[300]; snprintf(m, sizeof m, "ROM MD5 is %s\nExpected %s\n\nThe translated code only matches the exact Director's Cut ROM it was generated from.", hex, ROM_MD5);
        MessageBoxA(NULL, m, "Wrong ROM", MB_ICONERROR); return -1;
    }
    memcpy(rom_data, buf, n);
    return 1;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    char rom[MAX_PATH] = "";
    int cmd_aspect = -1;
    ini_path(); ini_load(); bind_load();
    /* simple ANSI command-line parse */
    {
        char *c = GetCommandLineA(); char *p = c;
        int first = 1;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char tok[MAX_PATH]; int n = 0;
            if (*p == '"') { p++; while (*p && *p != '"' && n < MAX_PATH - 1) tok[n++] = *p++; if (*p) p++; }
            else while (*p && *p != ' ' && n < MAX_PATH - 1) tok[n++] = *p++;
            tok[n] = 0;
            if (first) { first = 0; continue; }
            if (!strcmp(tok, "--pal")) vdp_pal = 1;
            else if (!strcmp(tok, "--4:3") || !strcmp(tok, "--43")) cmd_aspect = 0;
            else if (!strcmp(tok, "--16:9") || !strcmp(tok, "--169")) cmd_aspect = 1;
            else strncpy(rom, tok, MAX_PATH - 1);
        }
    }
    if (cmd_aspect >= 0) vdp_wide_enable = cmd_aspect;
    int ok = 0;
    if (rom[0]) ok = load_rom(rom);
    else {
        static const char *cand[] = { "sonic3dblast_dc.md", "Sonic 3D Blast - Director's Cut (World) (Unl).md", "rom.md", "rom.bin", NULL };
        for (int i = 0; cand[i] && ok <= 0; i++) ok = load_rom(cand[i]);
    }
    if (ok == 0) {
        OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
        char path[MAX_PATH] = "";
        ofn.lStructSize = sizeof ofn; ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = "Mega Drive ROM\0*.md;*.bin;*.gen;*.smd\0All files\0*.*\0";
        ofn.lpstrTitle = "Select your Sonic 3D Blast Director's Cut ROM";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameA(&ofn)) return 0;
        ok = load_rom(path);
    }
    if (ok <= 0) { if (ok == 0) MessageBoxA(NULL, "Could not read the ROM file.", "Error", MB_ICONERROR); return 1; }

    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "S3DNative"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_menu = CreateMenu();
    AppendMenuA(g_menu, MF_STRING, IDM_OPTIONS, "&Options");
    AppendMenuA(g_menu, MF_STRING, IDM_ABOUT, "&Help");
    RECT r = {0, 0, vdp_wide_enable ? 796 : 640, 448};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    g_hwnd = CreateWindowA("S3DNative", "Sonic 3D Blast: Directors Cut Recompiled", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, g_menu, hi, NULL);
    if (!g_hwnd) return 1;
    menu_sync();
    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = 448; g_bmi.bmiHeader.biHeight = -240;
    g_bmi.bmiHeader.biPlanes = 1; g_bmi.bmiHeader.biBitCount = 32; g_bmi.bmiHeader.biCompression = BI_RGB;
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
    timeBeginPeriod(1);
    init_input();
    audio_open();
    hw_init();
    s3d_start();
    if (g_wo_ok) { waveOutReset(g_wo); for (int i = 0; i < NBUF; i++) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]); waveOutClose(g_wo); }
    timeEndPeriod(1);
    return 0;
}
