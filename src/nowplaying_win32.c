// nowplaying_win32.c - another program's music through Windows's media controls
// (see nowplaying.h).
//
// Windows.Media.Control's GlobalSystemMediaTransportControlsSessionManager is a
// WinRT class; from C it is plain COM: its interfaces' methods are called by
// their place in the vtable (the places of the Windows SDK's metadata, after
// IUnknown's 3 and IInspectable's 3), and the asynchronous ones are waited for
// by asking their IAsyncInfo. combase.dll (WinRT) and shcore.dll (the picture's
// stream as an IStream) are loaded when needed, so PicoAmp still starts where
// they are not.
//
// Its own thread (the WinRT calls in the multithreaded apartment) looks a few
// times a second; the screen reads the copy (np_get). The program followed:
// Spotify when it has a session, else the one Windows calls the current one.
// Its volume is its session's in the mixer (the default output device), and
// the loopback of its sound uses its process tree (audio.c).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nowplaying.h"
#include "sys.h"
#include "util.h"

// ---------------- COM by vtable places ----------------
typedef void *hstr_t;
#define VT(p) (*(void ***)(p))
typedef HRESULT (WINAPI *f_ptr)(void *, void **);
typedef HRESULT (WINAPI *f_i32)(void *, INT32 *);
typedef HRESULT (WINAPI *f_i64)(void *, INT64 *);
typedef HRESULT (WINAPI *f_bool)(void *, BYTE *);
typedef HRESULT (WINAPI *f_qi)(void *, const GUID *, void **);
typedef ULONG   (WINAPI *f_rel)(void *);

static void rel(void *p) { if (p) ((f_rel)VT(p)[2])(p); }
static void *get_ptr(void *o, int slot) { void *r = NULL; if (o && FAILED(((f_ptr)VT(o)[slot])(o, &r))) r = NULL; return r; }
static INT32 get_i32(void *o, int slot, INT32 dflt) { INT32 v = dflt; if (o && FAILED(((f_i32)VT(o)[slot])(o, &v))) v = dflt; return v; }
static INT64 get_i64(void *o, int slot) { INT64 v = 0; if (o && FAILED(((f_i64)VT(o)[slot])(o, &v))) v = 0; return v; }
static bool get_bool(void *o, int slot) { BYTE v = 0; if (o && FAILED(((f_bool)VT(o)[slot])(o, &v))) v = 0; return v != 0; }
static void *qi(void *o, const GUID *iid) { void *r = NULL; if (o && FAILED(((f_qi)VT(o)[0])(o, iid, &r))) r = NULL; return r; }

static const GUID IID_AsyncInfo   = { 0x00000036, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_Closable    = { 0x30D5A829, 0x7FA4, 0x4026, { 0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E } };
static const GUID IID_Stream      = { 0x0000000C, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID IID_MgrStatics  = { 0x2050C4EE, 0x11A0, 0x57DE, { 0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45 } };
static const GUID CLSID_DevEnum   = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID IID_DevEnum     = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID IID_SessMgr2    = { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F } };
static const GUID IID_SessCtl2    = { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D } };
static const GUID IID_SimpleVol   = { 0x87CE5498, 0x68D6, 0x44E5, { 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8 } };

// combase.dll, shcore.dll
static HRESULT (WINAPI *pRoInitialize)(int);
static void    (WINAPI *pRoUninitialize)(void);
static HRESULT (WINAPI *pRoGetActivationFactory)(hstr_t, const GUID *, void **);
static HRESULT (WINAPI *pWindowsCreateString)(const WCHAR *, UINT32, hstr_t *);
static HRESULT (WINAPI *pWindowsDeleteString)(hstr_t);
static const WCHAR *(WINAPI *pWindowsGetStringRawBuffer)(hstr_t, UINT32 *);
static HRESULT (WINAPI *pCreateStreamOverRandomAccessStream)(void *, const GUID *, void **);

static bool load_api(void)
{
    static int done;            // 0 not yet, 1 there, -1 not
    if (done) return done > 0;
    HMODULE c = LoadLibraryW(L"combase.dll"), s = LoadLibraryW(L"shcore.dll");
    done = -1;
    if (!c) return false;
    pRoInitialize = (void *)GetProcAddress(c, "RoInitialize");
    pRoUninitialize = (void *)GetProcAddress(c, "RoUninitialize");
    pRoGetActivationFactory = (void *)GetProcAddress(c, "RoGetActivationFactory");
    pWindowsCreateString = (void *)GetProcAddress(c, "WindowsCreateString");
    pWindowsDeleteString = (void *)GetProcAddress(c, "WindowsDeleteString");
    pWindowsGetStringRawBuffer = (void *)GetProcAddress(c, "WindowsGetStringRawBuffer");
    if (s) pCreateStreamOverRandomAccessStream = (void *)GetProcAddress(s, "CreateStreamOverRandomAccessStream");
    if (pRoInitialize && pRoUninitialize && pRoGetActivationFactory && pWindowsCreateString && pWindowsDeleteString && pWindowsGetStringRawBuffer)
        done = 1;
    return done > 0;
}

// a property that is an HSTRING, as UTF-8
static void get_str(void *o, int slot, char *out, size_t n)
{
    out[0] = 0;
    hstr_t h = NULL;
    if (!o || FAILED(((f_ptr)VT(o)[slot])(o, &h)) || !h) return;
    UINT32 len = 0;
    const WCHAR *w = pWindowsGetStringRawBuffer(h, &len);
    if (w && len)
    {
        int k = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, out, (int)n - 1, NULL, NULL);
        out[k > 0 ? k : 0] = 0;
    }
    pWindowsDeleteString(h);
}

// an IAsyncOperation<some interface>: wait (up to ms), its result (or NULL); the operation is released
static void *await(void *op, unsigned ms)
{
    void *res = NULL;
    if (!op) return NULL;
    void *info = qi(op, &IID_AsyncInfo);
    if (info)
    {
        INT32 s = 0;
        DWORD t0 = GetTickCount();
        while ((s = get_i32(info, 7, 3)) == 0 && GetTickCount() - t0 < ms) Sleep(2);   // Status: 0 started, 1 done
        if (s == 1) res = get_ptr(op, 8);    // GetResults
        rel(info);
    }
    rel(op);
    return res;
}

// an IReference<int / bool> (may be NULL): its value
static INT32 ref_i32(void *r, INT32 dflt) { INT32 v = get_i32(r, 6, dflt); rel(r); return v; }
static bool  ref_bool(void *r) { bool v = get_bool(r, 6); rel(r); return v; }

// ---------------- the state ----------------
static sys_mutex  *lock;
static sys_thread *thread;
static volatile bool running;
static np_info     info;            // (lock)
static uint8_t    *art;             // (lock) the picture's bytes
static size_t      art_len;

typedef struct { int cmd, arg; } np_q;
static np_q        q[16];           // (lock) the keys, for the thread
static int         qn;
static volatile int want_vol = -1;  // (the thread sets it in the mixer)
static volatile int want_mute;

// the thread's own
static void   *mgr;                 // the session manager
static void   *cur;                 // the session followed
static INT64   cur_start;           // its timeline's start (100 ns)
static char    track_key[800];
static DWORD   art_at[2];           // when to read the picture (0: not)
static bool    art_first;           // the first read for this track: shown even if the same bytes
static WCHAR   exe[MAX_PATH];       // the program's .exe ("Spotify.exe"), for the mixer and the loopback
static DWORD   pids[64];
static int     npids;
static DWORD   root_pid;
static DWORD   pids_at, vol_at;
static void   *dev_enum;

bool np_supported(void) { return load_api(); }

// "Spotify.exe", "SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic",
// "Chrome" ...: a name to show, and the .exe when the id says it
static void app_name(const char *id, char *name, size_t n)
{
    char t[256];
    const char *p = strrchr(id, '!');
    u_copy(t, sizeof t, p ? p + 1 : u_basename(id));
    if (u_ends_with_ci(t, ".exe")) t[strlen(t) - 4] = 0;
    const char *d = strrchr(t, '.');
    u_copy(name, n, d && d[1] ? d + 1 : t);
    if (name[0] >= 'a' && name[0] <= 'z') name[0] -= 32;
    exe[0] = 0;
    char e[MAX_PATH];
    if (strstr(id, "potify") || strstr(id, "POTIFY")) u_copy(e, sizeof e, "Spotify.exe");
    else if (u_ends_with_ci(id, ".exe")) u_copy(e, sizeof e, u_basename(id));
    else e[0] = 0;
    if (e[0]) MultiByteToWideChar(CP_UTF8, 0, e, -1, exe, MAX_PATH);
}

// the program's processes (all of the .exe's), and the first of them (its parent is not one of them)
static void find_pids(void)
{
    npids = 0;
    root_pid = 0;
    if (!exe[0]) return;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof pe };
    DWORD parent[64];
    for (BOOL ok = Process32FirstW(s, &pe); ok && npids < 64; ok = Process32NextW(s, &pe))
        if (!_wcsicmp(pe.szExeFile, exe)) { parent[npids] = pe.th32ParentProcessID; pids[npids++] = pe.th32ProcessID; }
    CloseHandle(s);
    for (int i = 0; i < npids && !root_pid; i++)
    {
        bool child = false;
        for (int j = 0; j < npids; j++) if (parent[i] == pids[j]) child = true;
        if (!child) root_pid = pids[i];
    }
}

static bool our_pid(DWORD p) { for (int i = 0; i < npids; i++) if (pids[i] == p) return true; return false; }

// the mixer: the program's sessions on the default output; set (volume >= 0) or read
static void mixer(int set_volume, int set_mute, int *volume, bool *muted)
{
    *volume = -1;
    if (!npids) return;
    if (!dev_enum && FAILED(CoCreateInstance(&CLSID_DevEnum, NULL, CLSCTX_ALL, &IID_DevEnum, &dev_enum))) { dev_enum = NULL; return; }
    void *dev = NULL, *mgr2 = NULL, *en = NULL;
    // IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eMultimedia)
    if (FAILED(((HRESULT (WINAPI *)(void *, int, int, void **))VT(dev_enum)[4])(dev_enum, 0, 1, &dev)) || !dev) return;
    // IMMDevice::Activate(IAudioSessionManager2)
    if (SUCCEEDED(((HRESULT (WINAPI *)(void *, const GUID *, DWORD, void *, void **))VT(dev)[3])(dev, &IID_SessMgr2, CLSCTX_ALL, NULL, &mgr2)) && mgr2)
    {
        if (SUCCEEDED(((f_ptr)VT(mgr2)[5])(mgr2, &en)) && en)       // GetSessionEnumerator
        {
            int n = 0;
            ((HRESULT (WINAPI *)(void *, int *))VT(en)[3])(en, &n);   // GetCount
            for (int i = 0; i < n; i++)
            {
                void *ctl = NULL;
                if (FAILED(((HRESULT (WINAPI *)(void *, int, void **))VT(en)[4])(en, i, &ctl)) || !ctl) continue;
                void *ctl2 = qi(ctl, &IID_SessCtl2);
                DWORD pid = 0;
                if (ctl2) ((HRESULT (WINAPI *)(void *, DWORD *))VT(ctl2)[14])(ctl2, &pid);    // GetProcessId
                if (pid && our_pid(pid))
                {
                    void *sv = qi(ctl, &IID_SimpleVol);
                    if (sv)
                    {
                        if (set_volume >= 0)
                        {
                            ((HRESULT (WINAPI *)(void *, float, const GUID *))VT(sv)[3])(sv, set_volume / 100.0f, NULL);
                            ((HRESULT (WINAPI *)(void *, BOOL, const GUID *))VT(sv)[5])(sv, set_mute ? TRUE : FALSE, NULL);
                        }
                        float f = 0;
                        BOOL m = FALSE;
                        if (*volume < 0 && SUCCEEDED(((HRESULT (WINAPI *)(void *, float *))VT(sv)[4])(sv, &f)))
                        {
                            ((HRESULT (WINAPI *)(void *, BOOL *))VT(sv)[6])(sv, &m);
                            *volume = (int)(f * 100.0f + 0.5f);
                            *muted = m != FALSE;
                        }
                        rel(sv);
                    }
                }
                rel(ctl2);
                rel(ctl);
            }
            rel(en);
        }
        rel(mgr2);
    }
    rel(dev);
}

// the picture of the session's media properties: its bytes (malloc'd) or NULL
static uint8_t *read_art(void *props, size_t *len)
{
    *len = 0;
    if (!pCreateStreamOverRandomAccessStream) return NULL;
    void *ref = get_ptr(props, 15);                     // Thumbnail: IRandomAccessStreamReference
    if (!ref) return NULL;
    void *ras = await(get_ptr(ref, 6), 2000);           // OpenReadAsync
    rel(ref);
    if (!ras) return NULL;
    void *is = NULL;
    uint8_t *b = NULL;
    size_t n = 0, cap = 0;
    if (SUCCEEDED(pCreateStreamOverRandomAccessStream(ras, &IID_Stream, &is)) && is)
    {
        for (;;)
        {
            if (n + 65536 > cap)
            {
                if (cap >= 8u << 20) break;
                cap = cap ? cap * 2 : 131072;
                uint8_t *nb = realloc(b, cap);
                if (!nb) break;
                b = nb;
            }
            ULONG got = 0;
            HRESULT hr = ((HRESULT (WINAPI *)(void *, void *, ULONG, ULONG *))VT(is)[3])(is, b + n, 65536, &got);   // Read
            n += got;
            if (FAILED(hr) || got == 0) break;
        }
        rel(is);
    }
    void *cl = qi(ras, &IID_Closable);
    if (cl) { ((HRESULT (WINAPI *)(void *))VT(cl)[6])(cl); rel(cl); }
    rel(ras);
    if (n < 16) { free(b); return NULL; }
    *len = n;
    return b;
}

// the session to follow: Spotify's, else the current one (AddRef'd)
static void *pick_session(char *id, size_t idn)
{
    void *pick = NULL;
    void *v = get_ptr(mgr, 7);                          // GetSessions: IVectorView<session>
    if (v)
    {
        UINT32 n = 0;
        ((HRESULT (WINAPI *)(void *, UINT32 *))VT(v)[7])(v, &n);      // Size
        for (UINT32 i = 0; i < n && !pick; i++)
        {
            void *s = NULL;
            if (FAILED(((HRESULT (WINAPI *)(void *, UINT32, void **))VT(v)[6])(v, i, &s)) || !s) continue;    // GetAt
            char sid[256];
            get_str(s, 6, sid, sizeof sid);                   // SourceAppUserModelId
            if (strstr(sid, "potify") || strstr(sid, "POTIFY")) { pick = s; u_copy(id, idn, sid); }
            else rel(s);
        }
        rel(v);
    }
    if (!pick && (pick = get_ptr(mgr, 6)))                    // GetCurrentSession
        get_str(pick, 6, id, idn);
    return pick;
}

static void poll(void)
{
    np_info n;
    memset(&n, 0, sizeof n);
    n.volume = -1;
    char id[256] = "";
    void *s = mgr ? pick_session(id, sizeof id) : NULL;
    rel(cur);
    cur = s;
    if (!s)
    {
        sys_lock(lock);
        n.art_gen = info.art_gen;
        info = n;
        sys_unlock(lock);
        track_key[0] = 0;
        exe[0] = 0;
        npids = 0;
        return;
    }
    WCHAR old_exe[MAX_PATH];
    wcscpy(old_exe, exe);
    app_name(id, n.app, sizeof n.app);
    DWORD now = GetTickCount();
    if (wcscmp(old_exe, exe) || now - pids_at > 3000) { find_pids(); pids_at = now; }
    n.audio_pid = root_pid;

    // the playback: its state, what its keys can do, shuffle and repeat
    void *pb = get_ptr(s, 9);                                 // GetPlaybackInfo
    if (pb)
    {
        INT32 ps = get_i32(pb, 7, 0);                         // PlaybackStatus: 3 stopped, 4 playing, 5 paused
        n.state = ps == 4 ? NP_PLAYING : ps == 5 ? NP_PAUSED : NP_STOPPED;
        void *c = get_ptr(pb, 6);                             // Controls
        if (c)
        {
            n.can_next = get_bool(c, 12);
            n.can_prev = get_bool(c, 13);
            n.can_shuffle = get_bool(c, 17);
            n.can_repeat = get_bool(c, 18);
            n.can_seek = get_bool(c, 20);
            rel(c);
        }
        n.repeat = ref_i32(get_ptr(pb, 9), 0);               // AutoRepeatMode
        n.shuffle = ref_bool(get_ptr(pb, 11));              // IsShuffleActive
        rel(pb);
    }
    else n.state = NP_STOPPED;

    // the timeline (100 ns): where, how long; while it plays, on from when it was told
    void *tl = get_ptr(s, 8);                                 // GetTimelineProperties
    if (tl)
    {
        INT64 start = get_i64(tl, 6), end = get_i64(tl, 7), pos = get_i64(tl, 10), upd = get_i64(tl, 11);
        cur_start = start;
        INT64 e = pos - start;
        if (n.state == NP_PLAYING && upd > 0)
        {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            INT64 nowft = (INT64)(((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
            if (nowft > upd && nowft - upd < 36000000000LL) e += nowft - upd;
        }
        if (end > start) n.total_ms = (int)((end - start) / 10000);
        if (e < 0) e = 0;
        n.elapsed_ms = (int)(e / 10000);
        n.at = sys_millis();
        if (n.total_ms && n.elapsed_ms > n.total_ms) n.elapsed_ms = n.total_ms;
        rel(tl);
    }

    // what plays
    void *props = await(get_ptr(s, 7), 1500);                 // TryGetMediaPropertiesAsync
    if (props)
    {
        get_str(props, 6, n.title, sizeof n.title);
        get_str(props, 9, n.artist, sizeof n.artist);
        if (!n.artist[0]) get_str(props, 8, n.artist, sizeof n.artist);   // AlbumArtist
        get_str(props, 10, n.album, sizeof n.album);
        char k[sizeof track_key];
        snprintf(k, sizeof k, "%s\x1f%s\x1f%s\x1f%s", id, n.title, n.artist, n.album);
        if (strcmp(k, track_key))
        {
            // a new track: its picture a moment later (the program sends the words first), and again
            u_copy(track_key, sizeof track_key, k);
            art_at[0] = now + 500;
            art_at[1] = now + 3000;
            art_first = true;
            if (!art_at[0]) art_at[0] = 1;
        }
        for (int i = 0; i < 2; i++)
            if (art_at[i] && (int)(now - art_at[i]) >= 0)
            {
                art_at[i] = 0;
                size_t len;
                uint8_t *b = read_art(props, &len);
                sys_lock(lock);
                bool same = b ? (art && len == art_len && !memcmp(b, art, len)) : !art;
                if (art_first || !same)
                {
                    free(art);
                    art = b;
                    art_len = b ? len : 0;
                    b = NULL;
                    info.art_gen++;
                }
                art_first = false;
                sys_unlock(lock);
                free(b);
            }
        rel(props);
    }

    // its volume in the mixer (a second's old at most)
    if (want_vol >= 0 || now - vol_at > 1000)
    {
        int v = want_vol, m = want_mute;
        want_vol = -1;
        int got = -1;
        bool muted = false;
        mixer(v, m, &got, &muted);
        vol_at = now;
        sys_lock(lock);
        info.volume = got;
        info.muted = muted;
        sys_unlock(lock);
    }
    sys_lock(lock);
    n.volume = info.volume;
    n.muted = info.muted;
    n.art_gen = info.art_gen;
    info = n;
    sys_unlock(lock);
}

static void run_cmd(np_q c)
{
    if (!cur) return;
    void *op = NULL;
    void *s = cur;
    switch (c.cmd)
    {
    case NP_TOGGLE:  op = get_ptr(s, 20); break;
    case NP_PLAY:    op = get_ptr(s, 10); break;
    case NP_PAUSE:   op = get_ptr(s, 11); break;
    case NP_STOP:    op = get_ptr(s, 12); break;
    case NP_NEXT:    op = get_ptr(s, 16); break;
    case NP_PREV:    op = get_ptr(s, 17); break;
    case NP_SEEK_TO:
        ((HRESULT (WINAPI *)(void *, INT64, void **))VT(s)[24])(s, cur_start + (INT64)(c.arg < 0 ? 0 : c.arg) * 10000, &op);
        break;
    case NP_SHUFFLE:
        ((HRESULT (WINAPI *)(void *, BYTE, void **))VT(s)[23])(s, (BYTE)(c.arg != 0), &op);
        break;
    case NP_REPEAT:
        ((HRESULT (WINAPI *)(void *, INT32, void **))VT(s)[21])(s, (INT32)c.arg, &op);
        break;
    }
    // (an IAsyncOperation<bool>: its answer does not matter; the next look shows what happened)
    if (op)
    {
        void *ai = qi(op, &IID_AsyncInfo);
        DWORD t0 = GetTickCount();
        while (ai && get_i32(ai, 7, 3) == 0 && GetTickCount() - t0 < 500) Sleep(2);
        rel(ai);
        rel(op);
    }
}

static int np_main(void *arg)
{
    (void)arg;
    bool ro = SUCCEEDED(pRoInitialize(1));                   // RO_INIT_MULTITHREADED
    static const WCHAR cls[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    hstr_t h = NULL;
    void *statics = NULL;
    if (SUCCEEDED(pWindowsCreateString(cls, (UINT32)(sizeof cls / sizeof cls[0] - 1), &h)))
    {
        if (FAILED(pRoGetActivationFactory(h, &IID_MgrStatics, &statics))) statics = NULL;
        pWindowsDeleteString(h);
    }
    if (statics) { mgr = await(get_ptr(statics, 6), 5000); rel(statics); }   // RequestAsync
    DWORD next = 0;
    while (running)
    {
        np_q c[16];
        int k;
        sys_lock(lock);
        k = qn;
        memcpy(c, q, sizeof c[0] * (size_t)k);
        qn = 0;
        sys_unlock(lock);
        for (int i = 0; i < k; i++) run_cmd(c[i]);
        DWORD now = GetTickCount();
        if (k) next = now + 120;                              // soon after a key: what it did
        if ((int)(now - next) >= 0 || want_vol >= 0)
        {
            poll();
            next = GetTickCount() + 250;
        }
        Sleep(15);
    }
    // the keys asked for last (np_end after a pause: it still goes to the program)
    sys_lock(lock);
    np_q last[16];
    int nl = qn;
    memcpy(last, q, sizeof last[0] * (size_t)nl);
    qn = 0;
    sys_unlock(lock);
    for (int i = 0; i < nl; i++) run_cmd(last[i]);
    rel(cur);
    cur = NULL;
    rel(mgr);
    mgr = NULL;
    rel(dev_enum);
    dev_enum = NULL;
    if (ro) pRoUninitialize();
    return 0;
}

void np_start(void)
{
    if (thread || !load_api()) return;
    if (!lock) lock = sys_mutex_new();
    sys_lock(lock);
    memset(&info, 0, sizeof info);
    info.volume = -1;
    free(art);
    art = NULL;
    art_len = 0;
    qn = 0;
    sys_unlock(lock);
    track_key[0] = 0;
    art_at[0] = art_at[1] = 0;
    want_vol = -1;
    running = true;
    thread = sys_thread_start(np_main, NULL);
}

void np_end(void)
{
    if (!thread) return;
    running = false;
    sys_thread_join(thread);
    thread = NULL;
}

bool np_get(np_info *out)
{
    if (!thread) return false;
    sys_lock(lock);
    *out = info;
    sys_unlock(lock);
    return true;
}

bool np_art(uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    if (!thread) return false;
    sys_lock(lock);
    if (art && (*data = malloc(art_len))) { memcpy(*data, art, art_len); *len = art_len; }
    sys_unlock(lock);
    return *data != NULL;
}

void np_cmd(int cmd, int arg)
{
    if (!thread) return;
    sys_lock(lock);
    if (qn < 16) { q[qn].cmd = cmd; q[qn].arg = arg; qn++; }
    sys_unlock(lock);
}

void np_volume(int volume, bool muted)
{
    if (!thread) return;
    want_mute = muted;
    want_vol = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    sys_lock(lock);
    info.volume = want_vol;          // (shown at once; the mixer's own a moment later)
    info.muted = muted;
    sys_unlock(lock);
}
