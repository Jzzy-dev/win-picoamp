// pics.c - I: the pictures page, as PICOAMP's on the MSX (engine 0.17): a
// browser that shows only folders and pictures; Enter shows one full screen
// in SCREEN 8 (256 x 192, 256 colours: picture.c), Space marks pictures, G
// plays the marked ones (or all of the folder) as a slideshow; the settings:
// O the screensaver (off, or after 1-30 minutes without a key), R how long
// each picture shows, U in order or shuffled, F this folder for the
// screensaver. The screensaver (ui.c) opens this page with pics_saver().
//
// The pictures are decoded in a thread of their own (a big photo takes a
// moment), the next one of a slideshow while the one before shows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "sys.h"
#include "util.h"
#include "config.h"
#include "picture.h"

#define ROWS 19                         // the list: rows 2-20
enum { M_BROWSE, M_VIEW, M_SHOW, M_SAVER };

typedef struct { char *name; bool dir; bool mark; } entry;

static char   path[2048];               // the folder ("": the drives)
static entry *ent;
static int    n_ent, n_dir, top, cur;
static int    mode;
static int    shown = -1;               // the picture that shows (or is being made)
static char   msg[80];
static bool   saver_start;              // (the next open: the screensaver)

// ---------------------------------------------------------------- the decoding thread
static sys_mutex *lock;
static struct { unsigned gen; bool done; int err; uint8_t px[192][256]; } res;   // its result (under lock)
static unsigned want_gen;               // the picture asked for last
static int      want_idx = -1;
static uint8_t  scr[192][256];          // what shows
static bool     have_scr;
static uint32_t t_shown;
static int      fails;

typedef struct { unsigned gen; char path[2300]; } job;

static int work(void *arg)
{
    job *j = arg;
    uint8_t (*b)[256] = malloc(192 * 256);
    int e = b ? pic_file_s8(j->path, b) : PIC_E_OPEN;
    sys_lock(lock);
    if (j->gen == want_gen)
    {
        res.gen = j->gen;
        res.done = true;
        res.err = e;
        if (e == PIC_OK) memcpy(res.px, b, sizeof res.px);
    }
    sys_unlock(lock);
    free(b);
    free(j);
    return 0;
}

static void load(int idx)
{
    if (!lock) lock = sys_mutex_new();
    job *j = malloc(sizeof *j);
    if (!j || idx < 0 || idx >= n_ent) { free(j); return; }
    sys_lock(lock);
    j->gen = ++want_gen;
    res.done = false;
    sys_unlock(lock);
    want_idx = idx;
    u_join(j->path, sizeof j->path, path, ent[idx].name);
    sys_thread_detach(sys_thread_start(work, j));
}

// the picture asked for, when it is made: 1 made (in scr), -1 it cannot be read, 0 not yet
static int take(void)
{
    if (!lock) return 0;
    int r = 0;
    sys_lock(lock);
    if (res.done && res.gen == want_gen)
    {
        res.done = false;
        if (res.err == PIC_OK) { memcpy(scr, res.px, sizeof scr); r = 1; }
        else r = -1;
    }
    sys_unlock(lock);
    return r;
}

// ---------------------------------------------------------------- the folder
static void free_list(void)
{
    for (int i = 0; i < n_ent; i++) free(ent[i].name);
    free(ent);
    ent = NULL;
    n_ent = n_dir = 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return u_stricmp(x->name, y->name);
}

static bool read_folder(const char *p)
{
    free_list();
    if (!p[0])
    {
        // the drives (Windows)
        char d[26];
        int n = sys_drives(d);
        if (!n) return false;
        ent = calloc((size_t)n, sizeof *ent);
        for (int i = 0; ent && i < n; i++)
        {
            char s[4] = { d[i], ':', '\\', 0 };
            ent[i].name = strdup(s);
            ent[i].dir = true;
        }
        n_ent = n_dir = ent ? n : 0;
        u_copy(path, sizeof path, "");
        top = cur = 0;
        return true;
    }
    sys_dirent *d = NULL;
    int n = 0;
    if (!sys_list_dir(p, &d, &n)) return false;
    ent = calloc((size_t)(n ? n : 1), sizeof *ent);
    for (int i = 0; ent && i < n; i++)
    {
        if (d[i].name[0] == '.' || (!d[i].is_dir && !pic_is_picture_name(d[i].name))) continue;
        ent[n_ent].name = strdup(d[i].name);
        ent[n_ent].dir = d[i].is_dir;
        if (ent[n_ent].dir) n_dir++;
        n_ent++;
    }
    sys_free_dir(d, n);
    qsort(ent, (size_t)n_ent, sizeof *ent, ent_cmp);
    if (p != path) u_copy(path, sizeof path, p);
    top = cur = 0;
    return true;
}

static int n_pics(void) { return n_ent - n_dir; }
static int n_marked(void) { int m = 0; for (int i = n_dir; i < n_ent; i++) m += ent[i].mark; return m; }

// the folder above (its name chosen in it)
static void parent(void)
{
    if (!path[0]) return;
    char p[2048], was[1024];
    u_copy(p, sizeof p, path);
    size_t l = strlen(p);
    while (l > 1 && (p[l - 1] == '\\' || p[l - 1] == '/') && !(l == 3 && p[1] == ':')) p[--l] = 0;
    char *sl = strrchr(p, '/'), *bs = strrchr(p, '\\');
    if (bs > sl) sl = bs;
    if (l <= 3 && p[1] == ':')
    {
        // a drive's root: the drives
        u_copy(was, sizeof was, p);
        if (!read_folder("")) return;
    }
    else
    {
        if (!sl) return;
        u_copy(was, sizeof was, sl + 1);
        if (sl == p) sl[1] = 0;                           // "/"
        else if (sl == p + 2 && p[1] == ':') sl[1] = 0;    // "C:\"
        else *sl = 0;
        if (!read_folder(p)) return;
    }
    for (int i = 0; i < n_ent; i++)
        if (!u_stricmp(ent[i].name, was) || (!path[0] && !u_strnicmp(ent[i].name, was, 2))) { cur = i; break; }
    if (cur >= ROWS) top = cur - ROWS / 2;
}

// ---------------------------------------------------------------- the slideshow's order
static unsigned rng = 0x9E3779B9u;

static int next_pic(int from)
{
    int np = n_pics(), nm = n_marked();
    if (!np) return -1;
    if (cfg.pic_shuffle)
    {
        rng = rng * 1103515245u + 12345u + sys_millis();
        from = n_dir + (int)((rng >> 8) % (unsigned)np) - 1;
    }
    for (int k = 0; k < n_ent; k++)
    {
        from++;
        if (from >= n_ent || from < n_dir) from = n_dir;
        if (!nm || ent[from].mark) return from;
    }
    return -1;
}

static void start(int m, int idx)
{
    mode = m;
    shown = idx;
    have_scr = false;
    fails = 0;
    t_shown = 0;
    load(idx);
}

static void back(void)
{
    if (mode == M_SAVER) { ui_page(PG_PLAYER); return; }
    mode = M_BROWSE;
    if (lock) { sys_lock(lock); want_gen++; sys_unlock(lock); }   // (a picture still being made: not wanted)
    if (shown >= 0 && shown < n_ent)
    {
        cur = shown;
        if (cur < top || cur >= top + ROWS) top = cur > ROWS / 2 ? cur - ROWS / 2 : 0;
    }
}

// ---------------------------------------------------------------- the page
static const int savers[] = { 0, 1, 2, 5, 10, 15, 30 };
static const int intervals[] = { 5, 10, 20, 30, 60 };

void pics_saver(void) { saver_start = true; ui_page(PG_PICS); }

static void pc_open(void)
{
    mode = M_BROWSE;
    msg[0] = 0;
    const char *f = cfg.pic_folder[0] ? cfg.pic_folder : sys_pictures_dir();
    if (saver_start)
    {
        saver_start = false;
        int first = read_folder(f) ? next_pic(n_ent) : -1;
        if (first < 0) { ui_page(PG_PLAYER); return; }
        start(M_SAVER, first);
        return;
    }
    if (!read_folder(f) && !read_folder(sys_music_dir())) read_folder("");
}

static void pc_close(void)
{
    if (lock) { sys_lock(lock); want_gen++; sys_unlock(lock); }
    free_list();
}

static void draw_browser(void)
{
    gfx_clear(NAVY);
    gfx_fill(0, 0, 32, COL(WHITE, NAVY));
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    char t[2200];
    snprintf(t, sizeof t, "Pictures  %s", path[0] ? path : "(drives)");
    gfx_fill(1, 0, 32, COL(WHITE, BLUE));
    gfx_text6(1, 0, 32, t, COL(WHITE, BLUE), 0, 0);
    for (int r = 0; r < ROWS; r++)
    {
        int i = top + r;
        gfx_fill(2 + r, 0, 32, COL(WHITE, NAVY));
        if (i >= n_ent) continue;
        snprintf(t, sizeof t, "%c%s%s", ent[i].mark ? '*' : ' ', ent[i].name, ent[i].dir && path[0] ? "/" : "");
        uint8_t c = i == cur ? COL(NAVY, CYAN) : ent[i].mark ? COL(YELLOW, NAVY) : ent[i].dir ? COL(CYAN, NAVY) : COL(WHITE, NAVY);
        if (i == cur) gfx_fill(2 + r, 0, 32, c);
        gfx_text6(2 + r, 0, 32, t, c, 0, 0);
    }
    const char *note = ui_note_text();
    if (msg[0]) snprintf(t, sizeof t, "%s", msg);
    else if (note[0]) snprintf(t, sizeof t, "%s", note);
    else if (n_marked()) snprintf(t, sizeof t, "%d pictures, %d marked", n_pics(), n_marked());
    else snprintf(t, sizeof t, "%d pictures", n_pics());
    gfx_text6(21, 0, 32, t, COL(msg[0] || (note[0] && ui_note_error()) ? RED : GREEN, NAVY), 0, 0);
    gfx_text6(22, 0, 32, "Enter view  Space mark  A all  G slideshow", COL(GREY, NAVY), 0, 0);
    char sv[16];
    if (cfg.pic_saver) snprintf(sv, sizeof sv, "%dm", cfg.pic_saver);
    else snprintf(sv, sizeof sv, "off");
    snprintf(t, sizeof t, "O saver %s  R %ds  U shuffle %s  F here", sv, cfg.pic_interval, cfg.pic_shuffle ? "on" : "off");
    gfx_text6(23, 0, 32, t, COL(GREY, NAVY), 0, 0);
}

static void pc_frame(void)
{
    if (mode == M_BROWSE) { draw_browser(); return; }
    bool show = mode == M_SHOW || mode == M_SAVER;
    // the picture asked for: at once the first one and one picture; in a slideshow after its time
    if (!have_scr || mode == M_VIEW || sys_millis() - t_shown >= (uint32_t)cfg.pic_interval * 1000u)
    {
        int r = take();
        if (r > 0)
        {
            have_scr = true;
            shown = want_idx;
            t_shown = sys_millis();
            fails = 0;
            if (show) { int nx = next_pic(shown); if (nx >= 0 && nx != shown) load(nx); }
        }
        else if (r < 0)
        {
            if (mode == M_VIEW)
            {
                snprintf(msg, sizeof msg, "Not a picture it can read");
                if (!have_scr) { back(); draw_browser(); return; }
            }
            else if (++fails >= n_pics())
            {
                if (mode == M_SAVER) { ui_page(PG_PLAYER); return; }
                snprintf(msg, sizeof msg, "None of them can be read");
                back();
                draw_browser();
                return;
            }
            else { int nx = next_pic(want_idx); if (nx >= 0) load(nx); }
        }
    }
    if (have_scr)
    {
        memcpy(gfx_s8, scr, sizeof scr);
        gfx_s8_on = true;
    }
    else gfx_clear(BLK);
}

static void move(int d)
{
    if (!n_ent) return;
    cur += d;
    if (cur < 0) cur = 0;
    if (cur >= n_ent) cur = n_ent - 1;
    if (cur < top) top = cur;
    if (cur >= top + ROWS) top = cur - ROWS + 1;
}

static void enter(void)
{
    if (!n_ent) return;
    if (ent[cur].dir)
    {
        char p[2048];
        if (path[0]) u_join(p, sizeof p, path, ent[cur].name);
        else u_copy(p, sizeof p, ent[cur].name);
        if (!read_folder(p)) snprintf(msg, sizeof msg, "That folder cannot be read");
        return;
    }
    start(M_VIEW, cur);
}

static int step_of(const int *v, int n, int x) { for (int i = 0; i < n; i++) if (v[i] == x) return (i + 1) % n; return 0; }

static void pc_key(int k)
{
    msg[0] = 0;
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    if (mode == M_SAVER) { ui_page(PG_PLAYER); return; }
    if (mode != M_BROWSE)
    {
        if (u == K_RIGHT || u == K_LEFT)
        {
            if (mode != M_VIEW) { if (u == K_RIGHT) t_shown = 0; return; }   // the slideshow: the next one now
            int d = u == K_RIGHT ? 1 : -1;
            for (int i = want_idx + d; i >= n_dir && i < n_ent; i += d)
                if (!ent[i].dir) { load(i); return; }
            return;
        }
        if (u == ' ' && mode == M_VIEW) { if (shown >= n_dir && shown < n_ent) ent[shown].mark = !ent[shown].mark; return; }
        back();
        return;
    }
    switch (u)
    {
    case K_UP: move(-1); return;
    case K_DOWN: move(1); return;
    case K_LEFT: case K_PGUP: move(-ROWS); return;
    case K_RIGHT: case K_PGDN: move(ROWS); return;
    case K_HOME: move(-n_ent); return;
    case K_END: move(n_ent); return;
    case K_ENTER: enter(); return;
    case K_BS: parent(); return;
    case ' ':
        if (n_ent && !ent[cur].dir) { ent[cur].mark = !ent[cur].mark; move(1); }
        return;
    case 'A':
    {
        bool all = n_marked() == n_pics();
        for (int i = n_dir; i < n_ent; i++) ent[i].mark = !all;
        return;
    }
    case 'G':
    {
        int first = next_pic(n_ent);
        if (first < 0) { snprintf(msg, sizeof msg, "No pictures here"); return; }
        start(M_SHOW, first);
        return;
    }
    case 'O': cfg.pic_saver = savers[step_of(savers, 7, cfg.pic_saver)]; return;
    case 'R': cfg.pic_interval = intervals[step_of(intervals, 5, cfg.pic_interval)]; return;
    case 'U': cfg.pic_shuffle = !cfg.pic_shuffle; return;
    case 'F':
        if (path[0]) { u_copy(cfg.pic_folder, sizeof cfg.pic_folder, path); snprintf(msg, sizeof msg, "The screensaver shows this folder"); }
        return;
    case K_ESC: case 'I': ui_page(PG_PLAYER); return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'W': ui_page(PG_WX); return;
    case 'T': ui_page(PG_NEWS); return;
    case 'Z': ui_page(PG_FULL); return;
    }
    if (u != 'S' && u != 'H' && u != 'R') ui_playback_key(k);
}

static void pc_mouse(int x, int y, int b)
{
    (void)x;
    if (mode != M_BROWSE) { if (b == MOUSE_LEFT || b == MOUSE_RIGHT) pc_key(K_ESC); return; }
    if (b == MOUSE_RIGHT) { ui_page(PG_PLAYER); return; }
    if (b == MOUSE_WHEEL_UP) { move(-3); return; }
    if (b == MOUSE_WHEEL_DOWN) { move(3); return; }
    if (b != MOUSE_LEFT) return;
    int row = y / 8;
    if (row >= 2 && row < 2 + ROWS && top + row - 2 < n_ent)
    {
        int i = top + row - 2;
        if (i == cur) enter();
        else cur = i;
    }
}

static void pc_describe(char *out, size_t n)
{
    static const char *const mn[] = { "browse", "view", "slideshow", "screensaver" };
    snprintf(out, n, "pictures %s folder='%s' entries=%d folders=%d pictures=%d marked=%d cur=%d shown=%d have=%d saver=%d interval=%d shuffle=%d",
             mn[mode], path, n_ent, n_dir, n_pics(), n_marked(), cur, shown, have_scr, cfg.pic_saver, cfg.pic_interval, cfg.pic_shuffle);
}

const page_t page_pics = { pc_open, pc_frame, pc_key, pc_mouse, pc_close, pc_describe };
