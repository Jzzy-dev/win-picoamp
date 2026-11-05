// sys.c - threads, time, folders and files for Windows and (for the tests) POSIX
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sys.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

struct sys_thread { HANDLE h; int (*fn)(void *); void *arg; };
struct sys_mutex  { CRITICAL_SECTION cs; };

static DWORD WINAPI thread_main(LPVOID p)
{
    sys_thread *t = p;
    return (DWORD)t->fn(t->arg);
}

sys_thread *sys_thread_start(int (*fn)(void *), void *arg)
{
    sys_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}

void sys_thread_join(sys_thread *t)
{
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

void sys_thread_detach(sys_thread *t)
{
    // (the thread has t's fn and arg already; t itself stays until the end)
    if (t) CloseHandle(t->h);
}

sys_mutex *sys_mutex_new(void)
{
    sys_mutex *m = calloc(1, sizeof *m);
    InitializeCriticalSection(&m->cs);
    return m;
}
void sys_lock(sys_mutex *m)   { EnterCriticalSection(&m->cs); }
void sys_unlock(sys_mutex *m) { LeaveCriticalSection(&m->cs); }
void sys_sleep_ms(unsigned ms) { Sleep(ms); }
uint32_t sys_millis(void) { return (uint32_t)GetTickCount64(); }

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n <= 0) { w[0] = 0; return w; }
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    for (wchar_t *p = w; *p; p++) if (*p == '/') *p = '\\';
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (n <= 0) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

bool sys_list_dir(const char *path, sys_dirent **out, int *count)
{
    size_t l = strlen(path);
    char *pat = malloc(l + 3);
    memcpy(pat, path, l);
    if (l && path[l - 1] != '/' && path[l - 1] != '\\') pat[l++] = '\\';
    pat[l++] = '*';
    pat[l] = 0;
    wchar_t *w = wide(pat);
    free(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    *out = NULL;
    *count = 0;
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    int cap = 0;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (*count == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, sizeof **out * cap); }
        sys_dirent *e = &(*out)[(*count)++];
        e->name = narrow(fd.cFileName);
        e->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool sys_is_dir(const char *path)
{
    wchar_t *w = wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool sys_file_exists(const char *path)
{
    wchar_t *w = wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

static bool mkdir_one(const char *path)
{
    wchar_t *w = wide(path);
    BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok;
}

void *sys_fopen(const char *path, const char *mode)
{
    wchar_t *w = wide(path), *m = wide(mode);
    FILE *f = _wfopen(w, m);
    free(w);
    free(m);
    return f;
}

// installed from the Store (an MSIX package): its family name, else false
static bool package_family(wchar_t *out, UINT32 n)
{
    typedef LONG (WINAPI *fn_t)(UINT32 *, PWSTR);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    fn_t f = k ? (fn_t)(void *)GetProcAddress(k, "GetCurrentPackageFamilyName") : NULL;
    return f && f(&n, out) == ERROR_SUCCESS && out[0];
}

const char *sys_config_dir(void)
{
    static char dir[1024];
    wchar_t fam[256];
    if (!dir[0] && package_family(fam, 256))
    {
        // a packaged app's %APPDATA% is redirected to a place of its own: the
        // package's LocalState instead, a real folder that Explorer opens (O)
        wchar_t *p = NULL;
        if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &p) == S_OK)
        {
            char *s = narrow(p), *f = narrow(fam);
            snprintf(dir, sizeof dir, "%s\\Packages\\%s\\LocalState\\PicoAmp", s, f);
            free(s);
            free(f);
        }
        if (p) CoTaskMemFree(p);
    }
    if (!dir[0])
    {
        wchar_t *p = NULL;
        if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &p) == S_OK)
        {
            char *s = narrow(p);
            snprintf(dir, sizeof dir, "%s\\PicoAmp", s);
            free(s);
        }
        else snprintf(dir, sizeof dir, "PicoAmp");
        if (p) CoTaskMemFree(p);
    }
    return dir;
}

const char *sys_music_dir(void)
{
    static char dir[1024];
    if (!dir[0])
    {
        wchar_t *p = NULL;
        if (SHGetKnownFolderPath(&FOLDERID_Music, 0, NULL, &p) == S_OK)
        {
            char *s = narrow(p);
            snprintf(dir, sizeof dir, "%s", s);
            free(s);
        }
        if (p) CoTaskMemFree(p);
    }
    return dir;
}

int sys_drives(char letters[26])
{
    DWORD m = GetLogicalDrives();
    int n = 0;
    for (int i = 0; i < 26; i++) if (m & (1u << i)) letters[n++] = (char)('A' + i);
    return n;
}

#else  // POSIX (the tests)
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

struct sys_thread { pthread_t t; int (*fn)(void *); void *arg; };
struct sys_mutex  { pthread_mutex_t m; };

static void *thread_main(void *p)
{
    sys_thread *t = p;
    t->fn(t->arg);
    return NULL;
}

sys_thread *sys_thread_start(int (*fn)(void *), void *arg)
{
    sys_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_main, t)) { free(t); return NULL; }
    return t;
}

void sys_thread_join(sys_thread *t)
{
    if (!t) return;
    pthread_join(t->t, NULL);
    free(t);
}

void sys_thread_detach(sys_thread *t)
{
    if (t) pthread_detach(t->t);
}

sys_mutex *sys_mutex_new(void)
{
    sys_mutex *m = calloc(1, sizeof *m);
    pthread_mutex_init(&m->m, NULL);
    return m;
}
void sys_lock(sys_mutex *m)   { pthread_mutex_lock(&m->m); }
void sys_unlock(sys_mutex *m) { pthread_mutex_unlock(&m->m); }
void sys_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
uint32_t sys_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool sys_list_dir(const char *path, sys_dirent **out, int *count)
{
    *out = NULL;
    *count = 0;
    DIR *d = opendir(path);
    if (!d) return false;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d)))
    {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        if (*count == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, sizeof **out * cap); }
        sys_dirent *e = &(*out)[(*count)++];
        e->name = strdup(de->d_name);
        e->is_dir = S_ISDIR(st.st_mode);
        e->size = (uint64_t)st.st_size;
    }
    closedir(d);
    return true;
}

bool sys_is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

bool sys_file_exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

static bool mkdir_one(const char *path)
{
    return !mkdir(path, 0755) || sys_is_dir(path);
}

void *sys_fopen(const char *path, const char *mode) { return fopen(path, mode); }

const char *sys_config_dir(void)
{
    static char dir[1024];
    if (!dir[0])
    {
        const char *e = getenv("PICOAMP_CONFIG");
        if (e) snprintf(dir, sizeof dir, "%s", e);
        else snprintf(dir, sizeof dir, "%s/.config/picoamp", getenv("HOME") ? getenv("HOME") : ".");
    }
    return dir;
}

const char *sys_music_dir(void)
{
    static char dir[1024];
    if (!dir[0])
    {
        const char *e = getenv("PICOAMP_MUSIC");
        if (e) snprintf(dir, sizeof dir, "%s", e);
        else snprintf(dir, sizeof dir, "%s/Music", getenv("HOME") ? getenv("HOME") : ".");
    }
    return dir;
}

int sys_drives(char letters[26]) { (void)letters; return 0; }
#endif

int64_t sys_unix_time(void) { return (int64_t)time(NULL); }

void sys_free_dir(sys_dirent *d, int count)
{
    for (int i = 0; i < count; i++) free(d[i].name);
    free(d);
}

bool sys_mkdir(const char *path)
{
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if ((*p == '/' || *p == '\\') && p[-1] != ':')
        {
            char c = *p;
            *p = 0;
            mkdir_one(buf);
            *p = c;
        }
    return mkdir_one(buf);
}

char *sys_read_file(const char *path, size_t *len)
{
    FILE *f = sys_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

bool sys_write_file(const char *path, const void *data, size_t len)
{
    // write a new file, then put it in place: a crash never leaves half a file
    char tmp[2100];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = sys_fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }
#ifdef _WIN32
    wchar_t *a = wide(tmp), *b = wide(path);
    ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) != 0;
    free(a);
    free(b);
#else
    ok = rename(tmp, path) == 0;
#endif
    return ok;
}
