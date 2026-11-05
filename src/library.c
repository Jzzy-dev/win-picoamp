// library.c - folders and station lists (see library.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"
#include "config.h"
#include "sys.h"
#include "util.h"

#define FAV_FILE "Favourites.m3u"

static const char default_stations[] =
    "#EXTM3U\r\n"
    "#EXTINF:-1,Radio Paradise (MP3 128k)\r\nhttp://stream.radioparadise.com/mp3-128\r\n"
    "#EXTINF:-1,SomaFM Groove Salad (MP3 128k)\r\nhttp://ice1.somafm.com/groovesalad-128-mp3\r\n"
    "#EXTINF:-1,SomaFM Groove Salad (MP3 128k, HTTPS)\r\nhttps://ice1.somafm.com/groovesalad-128-mp3\r\n"
    "#EXTINF:-1,SomaFM Secret Agent (MP3 128k)\r\nhttp://ice1.somafm.com/secretagent-128-mp3\r\n"
    "#EXTINF:-1,SomaFM Drone Zone (MP3 128k)\r\nhttp://ice1.somafm.com/dronezone-128-mp3\r\n"
    "#EXTINF:-1,SomaFM DEF CON Radio (MP3 128k)\r\nhttp://ice1.somafm.com/defcon-128-mp3\r\n";

static const char readme[] =
    "PicoAmp for Windows - station lists\r\n"
    "\r\n"
    "Every .m3u or .pls file in this folder is a list under Internet radio.\r\n"
    "Add, rename or remove files and edit them in any text editor; PicoAmp reads\r\n"
    "them again each time a list is opened. An .m3u list looks like this:\r\n"
    "\r\n"
    "  #EXTM3U\r\n"
    "  #EXTINF:-1,Name of the station\r\n"
    "  http://address.of/the/stream\r\n"
    "\r\n"
    "Favourites.m3u is written by PicoAmp itself (F adds, D removes).\r\n"
    "Dropping an .m3u or .pls file on the PicoAmp window copies it here.\r\n";

bool lib_is_music(const char *name)
{
    return u_ends_with_ci(name, ".mp3") || u_ends_with_ci(name, ".wav") || u_ends_with_ci(name, ".flac");
}

static bool is_list_file(const char *name)
{
    return u_ends_with_ci(name, ".m3u") || u_ends_with_ci(name, ".m3u8") || u_ends_with_ci(name, ".pls");
}

void lib_init(void)
{
    const char *rd = config_radio_dir();
    sys_mkdir(rd);
    char p[2200];
    u_join(p, sizeof p, rd, "Stations.m3u");
    if (cfg.first_run && !sys_file_exists(p))
    {
        u_join(p, sizeof p, rd, "Stations.m3u");
        sys_write_file(p, default_stations, sizeof default_stations - 1);
        u_join(p, sizeof p, rd, "README.txt");
        sys_write_file(p, readme, sizeof readme - 1);
    }
    u_join(p, sizeof p, rd, FAV_FILE);
    if (!sys_file_exists(p)) sys_write_file(p, "#EXTM3U\r\n", 9);
}

static void add(lib_list *l, int type, const char *name, const char *target)
{
    if (l->count % 64 == 0) l->e = realloc(l->e, sizeof *l->e * (size_t)(l->count + 64));
    lib_entry *e = &l->e[l->count++];
    memset(e, 0, sizeof *e);
    e->type = type;
    u_copy(e->name, sizeof e->name, name);
    u_copy(e->target, sizeof e->target, target);
    e->tls = type == E_STATION && !u_strnicmp(target, "https:", 6);
}

static int cmp_entry(const void *a, const void *b)
{
    const lib_entry *x = a, *y = b;
    if ((x->type == E_PARENT) != (y->type == E_PARENT)) return x->type == E_PARENT ? -1 : 1;
    if ((x->type == E_FOLDER) != (y->type == E_FOLDER)) return x->type == E_FOLDER ? -1 : 1;
    return u_stricmp(x->name, y->name);
}

// the stations of an .m3u or .pls text
typedef void (*station_fn)(void *ctx, const char *name, const char *url);

static void parse_list(char *text, station_fn fn, void *ctx)
{
    char name[256] = "";
    bool pls = strstr(text, "[playlist]") != NULL || strstr(text, "[Playlist]") != NULL;
    // .pls: FileN= and TitleN= may come in any order
    char *pls_file[200] = { 0 }, *pls_title[200] = { 0 };
    if (!strncmp(text, "\xEF\xBB\xBF", 3)) text += 3;
    char *cur = text;
    for (char *line; (line = u_token(&cur, "\r\n")); )
    {
        line = u_trim(line);
        if (!*line) continue;
        if (pls)
        {
            int idx = 0;
            if (!u_strnicmp(line, "File", 4) && (idx = atoi(line + 4)) > 0 && idx < 200)
                { char *eq = strchr(line, '='); if (eq) pls_file[idx] = u_trim(eq + 1); }
            else if (!u_strnicmp(line, "Title", 5) && (idx = atoi(line + 5)) > 0 && idx < 200)
                { char *eq = strchr(line, '='); if (eq) pls_title[idx] = u_trim(eq + 1); }
            continue;
        }
        if (!u_strnicmp(line, "#EXTINF:", 8))
        {
            char *comma = strchr(line, ',');
            u_copy(name, sizeof name, comma ? u_trim(comma + 1) : "");
            continue;
        }
        if (line[0] == '#') continue;
        if (u_is_url(line))
        {
            fn(ctx, name[0] ? name : line, line);
            name[0] = 0;
        }
    }
    if (pls)
        for (int i = 1; i < 200; i++)
            if (pls_file[i] && u_is_url(pls_file[i]))
                fn(ctx, pls_title[i] && *pls_title[i] ? pls_title[i] : pls_file[i], pls_file[i]);
}

static void add_station(void *ctx, const char *name, const char *url) { add(ctx, E_STATION, name, url); }

static void list_name(const char *file, char *out, size_t size)
{
    u_copy(out, size, file);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

bool lib_open(lib_list *l, const char *place)
{
    lib_free(l);
    u_copy(l->place, sizeof l->place, place);
    l->ok = true;
    if (place[0]) add(l, E_PARENT, "..", "");          // as on the MSX: [..] first
    if (!place[0])
    {
        u_copy(l->title, sizeof l->title, "/");
        add(l, E_FOLDER, "Internet radio", "radio:");
        if (cfg.music_dir[0] && sys_is_dir(cfg.music_dir)) add(l, E_FOLDER, "Music", cfg.music_dir);
        char d[26];
        int n = sys_drives(d);
        for (int i = 0; i < n; i++)
        {
            char nm[8], tg[8];
            snprintf(nm, sizeof nm, "%c:", d[i]);
            snprintf(tg, sizeof tg, "%c:\\", d[i]);
            add(l, E_FOLDER, nm, tg);
        }
#ifndef _WIN32
        add(l, E_FOLDER, "/", "/");
#endif
        return true;
    }
    if (!strncmp(place, "radio:", 6))
    {
        const char *file = place + 6;
        const char *rd = config_radio_dir();
        if (!*file)
        {
            u_copy(l->title, sizeof l->title, "/Internet radio");
            sys_dirent *d;
            int n;
            if (!sys_list_dir(rd, &d, &n)) { l->ok = false; return false; }
            // Favourites first, the others by name
            add(l, E_FOLDER, "Favourites", "radio:" FAV_FILE);
            int first = l->count;
            for (int i = 0; i < n; i++)
            {
                if (d[i].is_dir || !is_list_file(d[i].name) || !u_stricmp(d[i].name, FAV_FILE)) continue;
                char nm[256], tg[300];
                list_name(d[i].name, nm, sizeof nm);
                snprintf(tg, sizeof tg, "radio:%s", d[i].name);
                add(l, E_FOLDER, nm, tg);
            }
            sys_free_dir(d, n);
            qsort(l->e + first, (size_t)(l->count - first), sizeof *l->e, cmp_entry);
            return true;
        }
        char nm[256], path[2200];
        list_name(file, nm, sizeof nm);
        snprintf(l->title, sizeof l->title, "/Internet radio/%s", nm);
        u_join(path, sizeof path, rd, file);
        char *text = sys_read_file(path, NULL);
        if (!text) { l->ok = false; return false; }
        parse_list(text, add_station, l);
        free(text);
        return true;
    }
    // a folder of the PC
    u_copy(l->title, sizeof l->title, place);
    for (char *p = l->title; *p; p++) if (*p == '\\') *p = '/';
    sys_dirent *d;
    int n;
    if (!sys_list_dir(place, &d, &n)) { l->ok = false; return false; }
    for (int i = 0; i < n; i++)
    {
        char full[2200];
        u_join(full, sizeof full, place, d[i].name);
        if (d[i].is_dir) add(l, E_FOLDER, d[i].name, full);
        else if (lib_is_music(d[i].name)) add(l, E_FILE, d[i].name, full);
    }
    sys_free_dir(d, n);
    if (l->count) qsort(l->e, (size_t)l->count, sizeof *l->e, cmp_entry);
    return true;
}

void lib_free(lib_list *l)
{
    free(l->e);
    l->e = NULL;
    l->count = 0;
}

void lib_parent(const char *place, char *out, size_t size)
{
    if (!place[0]) { out[0] = 0; return; }
    if (!strncmp(place, "radio:", 6)) { u_copy(out, size, place[6] ? "radio:" : ""); return; }
    char buf[2048];
    u_copy(buf, sizeof buf, place);
    size_t l = strlen(buf);
    while (l > 1 && (buf[l - 1] == '\\' || buf[l - 1] == '/') && buf[l - 2] != ':') buf[--l] = 0;
    char *sep = NULL;
    for (char *p = buf; *p; p++) if (*p == '\\' || *p == '/') sep = p;
    // the root of a drive (C:\) or of the file system: up to the top
    if (!sep || (sep[1] == 0)) { out[0] = 0; return; }
    if (sep == buf) { u_copy(out, size, "/"); return; }
    if (sep > buf && sep[-1] == ':') sep[1] = 0;      // C:\Music -> C:\ (keep the separator)
    else *sep = 0;
    u_copy(out, size, buf);
}

bool lib_is_favourites(const char *place)
{
    return !u_stricmp(place, "radio:" FAV_FILE);
}

// ---------------- favourites ----------------
typedef struct { char (*name)[256]; char (*url)[2048]; int n; } fav_t;

static void fav_take(void *ctx, const char *name, const char *url)
{
    fav_t *f = ctx;
    if (f->n % 32 == 0)
    {
        f->name = realloc(f->name, sizeof *f->name * (size_t)(f->n + 32));
        f->url = realloc(f->url, sizeof *f->url * (size_t)(f->n + 32));
    }
    u_copy(f->name[f->n], sizeof f->name[f->n], name);
    u_copy(f->url[f->n], sizeof f->url[f->n], url);
    f->n++;
}

static const char *fav_path(void)
{
    static char p[2200];
    u_join(p, sizeof p, config_radio_dir(), FAV_FILE);
    return p;
}

static void fav_load(fav_t *f)
{
    memset(f, 0, sizeof *f);
    char *text = sys_read_file(fav_path(), NULL);
    if (text) { parse_list(text, fav_take, f); free(text); }
}

static bool fav_save(const fav_t *f)
{
    size_t cap = 64 + (size_t)f->n * 2400, n = 0;
    char *b = malloc(cap);
    n += (size_t)snprintf(b + n, cap - n, "#EXTM3U\r\n");
    for (int i = 0; i < f->n; i++)
        n += (size_t)snprintf(b + n, cap - n, "#EXTINF:-1,%s\r\n%s\r\n", f->name[i], f->url[i]);
    bool ok = sys_write_file(fav_path(), b, n);
    free(b);
    return ok;
}

static void fav_free(fav_t *f) { free(f->name); free(f->url); }

int lib_fav_add(const char *name, const char *url)
{
    fav_t f;
    fav_load(&f);
    for (int i = 0; i < f.n; i++) if (!strcmp(f.url[i], url)) { fav_free(&f); return 0; }
    fav_take(&f, name && *name ? name : url, url);
    int r = fav_save(&f) ? 1 : -1;
    fav_free(&f);
    return r;
}

int lib_fav_del(const char *url)
{
    fav_t f;
    fav_load(&f);
    int r = 0;
    for (int i = 0; i < f.n; i++)
        if (!strcmp(f.url[i], url))
        {
            memmove(f.name + i, f.name + i + 1, sizeof *f.name * (size_t)(f.n - i - 1));
            memmove(f.url + i, f.url + i + 1, sizeof *f.url * (size_t)(f.n - i - 1));
            f.n--;
            r = fav_save(&f) ? 1 : -1;
            break;
        }
    fav_free(&f);
    return r;
}

static void count_one(void *ctx, const char *name, const char *url) { (void)name; (void)url; (*(int *)ctx)++; }

bool lib_import_list(const char *path, char *msg, size_t msglen)
{
    size_t len;
    char *text = sys_read_file(path, &len);
    if (!text) { snprintf(msg, msglen, "Cannot read %s", u_basename(path)); return false; }
    char *copy = malloc(len + 1);
    memcpy(copy, text, len + 1);
    int n = 0;
    parse_list(copy, count_one, &n);
    free(copy);
    if (!n) { free(text); snprintf(msg, msglen, "%s has no stations", u_basename(path)); return false; }
    char dst[2200];
    u_join(dst, sizeof dst, config_radio_dir(), u_basename(path));
    if (!u_stricmp(u_basename(path), FAV_FILE)) u_join(dst, sizeof dst, config_radio_dir(), "Favourites (imported).m3u");
    bool ok = sys_write_file(dst, text, len);
    free(text);
    char nm[256];
    list_name(u_basename(dst), nm, sizeof nm);
    if (ok) snprintf(msg, msglen, "%d stations: Internet radio/%s", n, nm);
    else snprintf(msg, msglen, "Cannot write to the Radio folder");
    return ok;
}
