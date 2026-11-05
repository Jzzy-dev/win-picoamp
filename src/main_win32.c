// main_win32.c - PicoAmp's window on Windows.
//
// The picture is PICOAMP's 256 x 192; the window can be any size. "Fit"
// (the default) keeps 4:3 and the pixels sharp: the picture is first blown up
// by a whole number with plain copies of pixels, and only the last step to the
// window's size is smoothed, so every MSX pixel stays a crisp, even block.
// "Integer" uses whole multiples only; "Stretch" fills the window.
//
//   PicoAmp.exe [files or folders or .m3u ...]
//   PicoAmp.exe --smoke out.png     (the CI's check: draw, take the window, quit)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "png.h"
#include "sys.h"
#include "util.h"

#ifndef PICOAMP_VERSION
#define PICOAMP_VERSION "dev"
#endif

#define HK_PLAY 1
#define HK_STOP 2
#define HK_NEXT 3
#define HK_PREV 4

static HWND      hwnd;
static bool      running = true;
static RECT      dest;                  // where the picture is in the client area
static uint32_t  rgb[GFX_MAXW * GFX_MAXH];
static uint32_t *big;                   // the picture blown up by whole numbers
static int       big_kx, big_ky, big_w, big_h;
static bool      big_scan;
static BITMAPINFO big_bmi;
static bool      smooth_ok = true;      // HALFTONE fast enough here
static WINDOWPLACEMENT saved_place = { sizeof saved_place };
static char      shown_title[600];

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

void plat_open_folder(const char *path)
{
    wchar_t *w = wide(path);
    ShellExecuteW(hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
}

void plat_quit(void) { PostMessageW(hwnd, WM_CLOSE, 0, 0); }

// ---------------- drawing ----------------
static void compute_dest(int cw, int ch)
{
    int dw, dh;
    if (cfg.scale == SCALE_STRETCH) { dw = cw; dh = ch; }
    else if (cfg.scale == SCALE_INTEGER)
    {
        int k = cw / GFX_W < ch / GFX_H ? cw / GFX_W : ch / GFX_H;
        if (k < 1) k = 1;
        dw = GFX_W * k;
        dh = GFX_H * k;
    }
    else
    {
        // 4:3, as large as fits
        if ((long long)cw * GFX_H <= (long long)ch * GFX_W) { dw = cw; dh = cw * GFX_H / GFX_W; }
        else { dh = ch; dw = ch * GFX_W / GFX_H; }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    dest.left = (cw - dw) / 2;
    dest.top = (ch - dh) / 2;
    dest.right = dest.left + dw;
    dest.bottom = dest.top + dh;
}

static void blow_up(int kx, int ky, bool scan)
{
    int gw = gfx_w, gh = gfx_h;
    if (kx != big_kx || ky != big_ky || gw != big_w || gh != big_h || !big)
    {
        free(big);
        big = malloc(sizeof(uint32_t) * (size_t)(gw * kx) * (size_t)(gh * ky));
        big_kx = kx;
        big_ky = ky;
        big_w = gw;
        big_h = gh;
        memset(&big_bmi, 0, sizeof big_bmi);
        big_bmi.bmiHeader.biSize = sizeof big_bmi.bmiHeader;
        big_bmi.bmiHeader.biWidth = gw * kx;
        big_bmi.bmiHeader.biHeight = -(gh * ky);       // top-down
        big_bmi.bmiHeader.biPlanes = 1;
        big_bmi.bmiHeader.biBitCount = 32;
        big_bmi.bmiHeader.biCompression = BI_RGB;
    }
    big_scan = scan;
    int W = gw * kx;
    for (int y = 0; y < gh; y++)
    {
        const uint32_t *src = rgb + y * gw;
        uint32_t *row0 = big + (size_t)(y * ky) * (size_t)W;
        for (int x = 0; x < gw; x++)
            for (int i = 0; i < kx; i++) row0[x * kx + i] = src[x];
        for (int r = 1; r < ky; r++)
        {
            uint32_t *row = row0 + (size_t)r * (size_t)W;
            // scan lines: the last third of every MSX line darker
            if (scan && r >= ky - (ky + 2) / 3)
                for (int x = 0; x < W; x++) row[x] = (row0[x] >> 1 & 0x7F7F7F) + (row0[x] >> 2 & 0x3F3F3F);
            else memcpy(row, row0, sizeof(uint32_t) * (size_t)W);
        }
    }
}

static void present(HDC dc)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    compute_dest(cr.right, cr.bottom);
    gfx_to_rgb(rgb);
    int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
    // whole multiples below the size (each way: the text mode's pixels are not square)
    int kx = dw / gfx_w, ky = dh / gfx_h;
    if (kx < 1) kx = 1;
    if (ky < 1) ky = 1;
    if (kx > 12) kx = 12;
    if (ky > 12) ky = 12;
    bool exact = dw == gfx_w * kx && dh == gfx_h * ky;
    bool scan = cfg.scanlines && ky >= 2;
    blow_up(kx, ky, scan);
    // the borders in the backdrop colour
    HBRUSH b = CreateSolidBrush(RGB(gfx_pal[NAVY] >> 16 & 255, gfx_pal[NAVY] >> 8 & 255, gfx_pal[NAVY] & 255));
    RECT r;
    if (dest.top > 0) { SetRect(&r, 0, 0, cr.right, dest.top); FillRect(dc, &r, b); }
    if (dest.bottom < cr.bottom) { SetRect(&r, 0, dest.bottom, cr.right, cr.bottom); FillRect(dc, &r, b); }
    if (dest.left > 0) { SetRect(&r, 0, dest.top, dest.left, dest.bottom); FillRect(dc, &r, b); }
    if (dest.right < cr.right) { SetRect(&r, dest.right, dest.top, cr.right, dest.bottom); FillRect(dc, &r, b); }
    DeleteObject(b);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    bool smooth = !exact && smooth_ok;
    SetStretchBltMode(dc, smooth ? HALFTONE : COLORONCOLOR);
    if (smooth) SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, dest.left, dest.top, dw, dh, 0, 0, gfx_w * kx, gfx_h * ky, big, &big_bmi, DIB_RGB_COLORS, SRCCOPY);
    QueryPerformanceCounter(&t1);
    // smoothing too slow on this machine (or remote desktop): plain copies
    if (smooth && (t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart > 14)
    {
        static int slow;
        if (++slow > 20) smooth_ok = false;
    }
}

static void redraw(void)
{
    HDC dc = GetDC(hwnd);
    present(dc);
    ReleaseDC(hwnd, dc);
}

static void update_title(void)
{
    audio_status s;
    audio_status_get(&s);
    char t[600];
    if (s.state == ST_STOPPED || (!s.title[0] && !s.album[0])) snprintf(t, sizeof t, "PicoAmp");
    else
    {
        const char *who = s.artist[0] ? s.artist : s.album;
        if (s.title[0] && who[0]) snprintf(t, sizeof t, "%s - %s - PicoAmp", s.title, who);
        else snprintf(t, sizeof t, "%s - PicoAmp", s.title[0] ? s.title : who);
    }
    if (strcmp(t, shown_title))
    {
        u_copy(shown_title, sizeof shown_title, t);
        wchar_t *w = wide(t);
        SetWindowTextW(hwnd, w);
        free(w);
    }
}

// ---------------- the window ----------------
static void set_fullscreen(bool on)
{
    DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    if (on)
    {
        MONITORINFO mi = { sizeof mi };
        if (!cfg.fullscreen) GetWindowPlacement(hwnd, &saved_place);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        if (cfg.fullscreen) SetWindowPlacement(hwnd, &saved_place);
        SetWindowPos(hwnd, cfg.on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    cfg.fullscreen = on;
    ShowCursor(TRUE);
}

static bool shown;

static void remember_size(void)
{
    if (!shown || cfg.fullscreen || IsZoomed(hwnd) || IsIconic(hwnd)) return;
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    cfg.win_x = wr.left;
    cfg.win_y = wr.top;
    cfg.win_w = cr.right;
    cfg.win_h = cr.bottom;
}

static void mouse(LPARAM lp, int button)
{
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
    if (dw <= 0 || dh <= 0) return;
    int fx = (x - dest.left) * gfx_w / dw, fy = (y - dest.top) * gfx_h / dh;
    if (x < dest.left) fx = -1;
    if (y < dest.top) fy = -1;
    ui_mouse(fx, fy, button);
}

static const char *scale_name(int s) { return s == SCALE_INTEGER ? "Scaling: whole multiples" : s == SCALE_STRETCH ? "Scaling: stretch to the window" : "Scaling: fit, 4:3, sharp"; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (!hwnd) hwnd = h;            // (messages come before CreateWindowEx returns)
    switch (m)
    {
    case WM_CLOSE:
        remember_size();
        cfg.maximized = IsZoomed(h) != 0;
        running = false;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        present(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
    case WM_MOVE:
        remember_size();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        RECT r = { 0, 0, GFX_W, GFX_H };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_DPICHANGED:
    {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CHAR:
        if (wp == 27) { ShowWindow(h, SW_MINIMIZE); return 0; }     // as on the MSX: the music plays on
        if (wp == 13 || wp == 8 || (wp >= 32 && wp < 127)) ui_key((int)wp);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { set_fullscreen(!cfg.fullscreen); return 0; }
        if (wp == VK_F4) break;               // Alt+F4
        break;
    case WM_SYSCHAR:
        if (wp == 13) return 0;               // no beep for Alt+Enter
        break;
    case WM_KEYDOWN:
        switch (wp)
        {
        case VK_UP: ui_key(K_UP); return 0;
        case VK_DOWN: ui_key(K_DOWN); return 0;
        case VK_LEFT: ui_key(K_LEFT); return 0;
        case VK_RIGHT: ui_key(K_RIGHT); return 0;
        case VK_PRIOR: ui_key(K_PGUP); return 0;
        case VK_NEXT: ui_key(K_PGDN); return 0;
        case VK_HOME: ui_key(K_HOME); return 0;
        case VK_END: ui_key(K_END); return 0;
        case VK_DELETE: ui_key(K_DEL); return 0;
        case VK_F11: set_fullscreen(!cfg.fullscreen); return 0;
        case VK_F2:
            cfg.scale = (cfg.scale + 1) % 3;
            ui_message(scale_name(cfg.scale), false);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case VK_F3:
            cfg.scanlines = !cfg.scanlines;
            ui_message(cfg.scanlines ? "Scan lines on" : "Scan lines off", false);
            return 0;
        case VK_F4:
            cfg.on_top = !cfg.on_top;
            SetWindowPos(h, cfg.on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            ui_message(cfg.on_top ? "Always on top" : "Not on top", false);
            return 0;
        case VK_MEDIA_PLAY_PAUSE: ui_key(K_MEDIA_PLAY); return 0;
        case VK_MEDIA_STOP: ui_key(K_MEDIA_STOP); return 0;
        case VK_MEDIA_NEXT_TRACK: ui_key(K_MEDIA_NEXT); return 0;
        case VK_MEDIA_PREV_TRACK: ui_key(K_MEDIA_PREV); return 0;
        }
        break;
    case WM_HOTKEY:
        switch (wp)
        {
        case HK_PLAY: ui_key(K_MEDIA_PLAY); break;
        case HK_STOP: ui_key(K_MEDIA_STOP); break;
        case HK_NEXT: ui_key(K_MEDIA_NEXT); break;
        case HK_PREV: ui_key(K_MEDIA_PREV); break;
        }
        return 0;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp))
        {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: ui_key(K_MEDIA_PLAY); return TRUE;
        case APPCOMMAND_MEDIA_STOP: ui_key(K_MEDIA_STOP); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: ui_key(K_MEDIA_NEXT); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: ui_key(K_MEDIA_PREV); return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        mouse(lp, MOUSE_LEFT);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        mouse(lp, MOUSE_UP);
        return 0;
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) mouse(lp, MOUSE_DRAG);
        return 0;
    case WM_LBUTTONDBLCLK:
        mouse(lp, MOUSE_LEFT);
        return 0;
    case WM_RBUTTONDOWN:
        mouse(lp, MOUSE_RIGHT);
        return 0;
    case WM_MOUSEWHEEL:
    {
        static int acc;
        acc += GET_WHEEL_DELTA_WPARAM(wp);
        while (acc >= WHEEL_DELTA) { ui_mouse(0, 0, MOUSE_WHEEL_UP); acc -= WHEEL_DELTA; }
        while (acc <= -WHEEL_DELTA) { ui_mouse(0, 0, MOUSE_WHEEL_DOWN); acc += WHEEL_DELTA; }
        return 0;
    }
    case WM_DROPFILES:
    {
        HDROP d = (HDROP)wp;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, NULL, 0);
        char **paths = calloc(n ? n : 1, sizeof(char *));
        for (UINT i = 0; i < n; i++)
        {
            UINT l = DragQueryFileW(d, i, NULL, 0);
            wchar_t *w = malloc(sizeof(wchar_t) * (l + 1));
            DragQueryFileW(d, i, w, l + 1);
            paths[i] = narrow(w);
            free(w);
        }
        DragFinish(d);
        ui_drop((const char *const *)paths, (int)n);
        for (UINT i = 0; i < n; i++) free(paths[i]);
        free(paths);
        SetForegroundWindow(h);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void dpi_aware(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    ctx_fn f = u ? (ctx_fn)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (f && f((HANDLE)-4)) return;                 // per monitor v2
    SetProcessDPIAware();
}

static void caption_colours(void)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);      // DWMWA_USE_IMMERSIVE_DARK_MODE
    COLORREF cap = RGB(gfx_pal[BLUE] >> 16 & 255, gfx_pal[BLUE] >> 8 & 255, gfx_pal[BLUE] & 255);
    COLORREF txt = RGB(255, 255, 255);
    DwmSetWindowAttribute(hwnd, 35, &cap, sizeof cap);         // DWMWA_CAPTION_COLOR (Windows 11)
    DwmSetWindowAttribute(hwnd, 36, &txt, sizeof txt);         // DWMWA_TEXT_COLOR
}

// the window's client area into a PNG (the CI's check of the real drawing)
static bool grab(const char *path)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    int w = cr.right, h = cr.bottom;
    HDC wdc = GetDC(hwnd), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bm = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mdc, bm);
    present(mdc);                                   // draw straight into the bitmap
    SelectObject(mdc, old);
    uint32_t *px = malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
    memcpy(px, bits, sizeof(uint32_t) * (size_t)w * (size_t)h);
    for (int i = 0; i < w * h; i++) px[i] &= 0xFFFFFF;
    bool ok = png_write(path, px, w, h, 1, 1);
    free(px);
    DeleteObject(bm);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return ok;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;
    dpi_aware();
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char *smoke = NULL, *smoke_url = NULL;
    int smoke_ms = 2500;
    for (int i = 1; i + 1 < argc; i++)
    {
        if (!wcscmp(argv[i], L"--smoke")) smoke = narrow(argv[i + 1]);
        if (!wcscmp(argv[i], L"--play")) smoke_url = narrow(argv[i + 1]);
        if (!wcscmp(argv[i], L"--wait")) smoke_ms = _wtoi(argv[i + 1]);
    }
    bool smoke_run = smoke != NULL;

    net_init();
    gfx_init();
    config_load();
    if (smoke) { cfg.win_w = 1000; cfg.win_h = 700; cfg.win_x = cfg.win_y = 40; cfg.fullscreen = cfg.maximized = false; cfg.scale = SCALE_FIT; }
    audio_init(false);
    ui_init();

    WNDCLASSEXW wc = { sizeof wc };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"PicoAmpWindow";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, cfg.win_w, cfg.win_h };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (cfg.win_x != -1 || cfg.win_y != -1)
    {
        POINT p = { cfg.win_x + 40, cfg.win_y + 20 };
        if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) { x = cfg.win_x; y = cfg.win_y; }
    }
    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PicoAmp", WS_OVERLAPPEDWINDOW, x, y,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    caption_colours();
    bool full = cfg.fullscreen;
    cfg.fullscreen = false;
    ShowWindow(hwnd, cfg.maximized && !smoke ? SW_SHOWMAXIMIZED : show);
    shown = true;
    if (full && !smoke) set_fullscreen(true);
    if (cfg.on_top) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    RegisterHotKey(hwnd, HK_PLAY, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(hwnd, HK_STOP, MOD_NOREPEAT, VK_MEDIA_STOP);
    RegisterHotKey(hwnd, HK_NEXT, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(hwnd, HK_PREV, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);

    if (smoke && smoke_url) audio_play_url(smoke_url, "Smoke test");
    // files given on the command line (or "Open with")
    if (!smoke && argc > 1)
    {
        char **paths = calloc((size_t)argc, sizeof(char *));
        int n = 0;
        for (int i = 1; i < argc; i++) if (argv[i][0] != L'-') paths[n++] = narrow(argv[i]);
        if (n) ui_drop((const char *const *)paths, n);
        for (int i = 0; i < n; i++) free(paths[i]);
        free(paths);
    }

    timeBeginPeriod(1);
    uint32_t start = sys_millis(), next = start;
    int rc = 0;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        uint32_t now = sys_millis();
        if ((int32_t)(now - next) >= 0)
        {
            next += 16;
            if ((int32_t)(now - next) > 100) next = now + 16;    // behind (a drag, a sleep): no catching up
            ui_frame();
            update_title();
            if (!IsIconic(hwnd)) redraw();
            if (smoke && now - start > (uint32_t)smoke_ms)
            {
                rc = grab(smoke) ? 0 : 3;
                // and what the player says, next to the picture
                char tp[1100];
                snprintf(tp, sizeof tp, "%s.txt", smoke);
                FILE *f = sys_fopen(tp, "w");
                if (f)
                {
                    audio_status s;
                    audio_status_get(&s);
                    fprintf(f, "state=%d elapsed=%d rate=%d channels=%d bitrate=%d stream=%d tls=%d buffering=%d "
                               "title=%s\nalbum=%s\nerror=%s\nsmooth=%d config=%s\n",
                            s.state, s.elapsed, s.rate, s.channels, s.bitrate, s.stream, s.tls, s.buffering,
                            s.title, s.album, s.error, smooth_ok, sys_config_dir());
                    fclose(f);
                }
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                smoke = NULL;
            }
        }
        DWORD wait = (DWORD)((int32_t)(next - sys_millis()) > 0 ? (int32_t)(next - sys_millis()) : 0);
        if (IsIconic(hwnd)) wait = 100;
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
    }
    timeEndPeriod(1);
    if (!smoke_run)
    {
        ui_save_state();
        config_save();
    }
    audio_shutdown();
    LocalFree(argv);
    return rc;
}
