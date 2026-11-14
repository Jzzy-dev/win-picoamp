// logo.c - pictures from the internet for the cover view, as the MSX engine
// 0.16 finds them:
//   * a station's logo: Radio Browser is asked for the station (by its
//     directory id, else by its address, else by its exact name), its
//     "favicon" is fetched;
//   * a song (on a station: "Artist - Title") or a file without a picture
//     (artist and album, else artist and title): the iTunes search is asked,
//     its "artworkUrl100" is fetched (bigger: 300 x 300).
// What was found is kept in <config>/Logos and <config>/Covers ("none" too),
// so the next time it is there at once. Each lookup runs in a thread; the
// main loop takes the result (logo_take) and makes the cover.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "logo.h"
#include "net.h"
#include "radiodir.h"
#include "sys.h"
#include "util.h"

#define IMG_MAX (2u << 20)

static sys_mutex *lock;
static struct
{
    unsigned gen;                       // the lookup asked for last
    bool     have;                      // a result for it
    uint8_t *res;                       // its picture (NULL: none)
    size_t   len;
} ch[2];

typedef struct { int ch; char url[2048], uuid[64], name[256], term[600], cache[1100]; bool album; unsigned gen; } job_t;

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

// <config>/<dir>/<hash of key>. (the extension follows)
static void cache_path(char *out, size_t n, const char *dir, const char *key)
{
    char d[1024], f[32];
    u_join(d, sizeof d, sys_config_dir(), dir);
    snprintf(f, sizeof f, "%08X.", (unsigned)fnv(key));
    u_join(out, n, d, f);
}

static void deliver(int c, unsigned g, uint8_t *p, size_t n)
{
    sys_lock(lock);
    if (g == ch[c].gen) { free(ch[c].res); ch[c].res = p; ch[c].len = n; ch[c].have = true; p = NULL; }
    sys_unlock(lock);
    free(p);
}

// the value of "key": in a reply ("" none; \/ is /)
static void json_value(const char *j, const char *key, char *out, size_t n)
{
    out[0] = 0;
    char k[40];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = j ? strstr(j, k) : NULL;
    if (!p) return;
    p += strlen(k);
    while (*p == ' ' || *p == ':') p++;
    if (*p++ != '"') return;
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < n; p++)
        if (*p != '\\') out[o++] = *p;
    out[o] = 0;
}

static bool is_picture(const uint8_t *b, size_t n)
{
    if (n < 8) return false;
    return (b[0] == 0xFF && b[1] == 0xD8) || !memcmp(b, "\x89PNG", 4) || !memcmp(b, "GIF8", 4) ||
           !memcmp(b, "RIFF", 4) || !memcmp(b, "\0\0\1\0", 4) || !memcmp(b, "BM", 2);
}

// the station's logo's address (answered: a server answered at all)
static void ask_radio_browser(job_t *j, char *fav, size_t n, bool *answered)
{
    char q[1400], enc[1200], e[100];
    for (int way = j->uuid[0] ? 0 : 1; way < 3 && !fav[0] && j->gen == ch[j->ch].gen; way++)
    {
        if (way == 0) snprintf(q, sizeof q, "/json/stations/byuuid/%s", j->uuid);
        else if (way == 1) { rd_urlenc(enc, sizeof enc, j->url); snprintf(q, sizeof q, "/json/stations/byurl?url=%s", enc); }
        else if (j->name[0]) { rd_urlenc(enc, sizeof enc, j->name); snprintf(q, sizeof q, "/json/stations/search?name=%s&nameExact=true&limit=1&order=votes&reverse=true&hidebroken=true", enc); }
        else break;
        for (int i = 0; i < rd_base_count(); i++)
        {
            char base[256], u[1700];
            rd_base_url(i, base, sizeof base);
            snprintf(u, sizeof u, "%s%s", base, q);
            size_t len;
            char *r = net_fetch(u, 512u * 1024u, &len, e, sizeof e);
            if (!r) continue;
            *answered = true;
            json_value(r, "favicon", fav, n);
            free(r);
            break;
        }
    }
}

// the song's (or album's) cover's address
static void ask_itunes(job_t *j, char *art, size_t n, bool *answered)
{
    char enc[1300], u[1700], e[100];
    rd_urlenc(enc, sizeof enc, j->term);
    const char *base = getenv("PICOAMP_ITUNES");
    snprintf(u, sizeof u, "%s/search?term=%s&media=music&entity=%s&limit=1", base && *base ? base : "https://itunes.apple.com", enc, j->album ? "album" : "song");
    size_t len;
    char *r = net_fetch(u, 512u * 1024u, &len, e, sizeof e);
    if (!r) return;
    *answered = true;
    json_value(r, "artworkUrl100", art, n);
    free(r);
    char *s = strstr(art, "100x100bb");     // (a bigger one: Apple makes any size)
    if (s) memcpy(s, "300x300bb", 9);
}

static int run(void *arg)
{
    job_t *j = arg;
    char pic[1024] = "";
    bool answered = false;
    if (j->ch == LOGO_STATION) ask_radio_browser(j, pic, sizeof pic, &answered);
    else ask_itunes(j, pic, sizeof pic, &answered);
    if (pic[0] && strncmp(pic, "http://", 7) && strncmp(pic, "https://", 8)) pic[0] = 0;
    uint8_t *img = NULL;
    size_t n = 0;
    bool known = answered;
    if (pic[0] && j->gen == ch[j->ch].gen)
    {
        char e[100];
        img = (uint8_t *)net_fetch(pic, IMG_MAX, &n, e, sizeof e);
        if (img && !is_picture(img, n)) { free(img); img = NULL; }
        else if (!img) known = false;           // (the server away: asked again next time)
    }
    if (known)
    {
        char d[1024], p[1100];
        u_join(d, sizeof d, sys_config_dir(), j->ch == LOGO_STATION ? "Logos" : "Covers");
        sys_mkdir(d);
        snprintf(p, sizeof p, "%s%s", j->cache, img ? "img" : "none");
        sys_write_file(p, img ? (const void *)img : "", img ? n : 0);
    }
    deliver(j->ch, j->gen, img, n);
    free(j);
    return 0;
}

// known already: delivered at once; else asked in a thread
static void ask(job_t *j)
{
    char p[1100];
    size_t n = 0;
    snprintf(p, sizeof p, "%simg", j->cache);
    char *d = sys_file_exists(p) ? sys_read_file(p, &n) : NULL;
    if (d) { deliver(j->ch, j->gen, (uint8_t *)d, n); free(j); return; }
    snprintf(p, sizeof p, "%snone", j->cache);
    if (sys_file_exists(p)) { deliver(j->ch, j->gen, NULL, 0); free(j); return; }
    sys_thread_detach(sys_thread_start(run, j));
}

static job_t *new_job(int c)
{
    if (!lock) lock = sys_mutex_new();
    sys_lock(lock);
    ch[c].gen++;
    ch[c].have = false;
    free(ch[c].res);
    ch[c].res = NULL;
    unsigned g = ch[c].gen;
    sys_unlock(lock);
    job_t *j = calloc(1, sizeof *j);
    if (!j) { deliver(c, g, NULL, 0); return NULL; }
    j->ch = c;
    j->gen = g;
    return j;
}

void logo_station(const char *url, const char *uuid, const char *name)
{
    job_t *j = new_job(LOGO_STATION);
    if (!j) return;
    if (!url || !url[0]) { deliver(j->ch, j->gen, NULL, 0); free(j); return; }
    u_copy(j->url, sizeof j->url, url);
    u_copy(j->uuid, sizeof j->uuid, uuid ? uuid : "");
    u_copy(j->name, sizeof j->name, name ? name : "");
    cache_path(j->cache, sizeof j->cache, "Logos", url);
    ask(j);
}

void logo_song(const char *artist, const char *what, bool album)
{
    job_t *j = new_job(LOGO_SONG);
    if (!j) return;
    if (!artist || !artist[0] || !what || !what[0]) { deliver(j->ch, j->gen, NULL, 0); free(j); return; }
    snprintf(j->term, sizeof j->term, "%s %s", artist, what);
    j->album = album;
    char k[600];
    size_t i = 0;
    for (const char *s = j->term; *s && i + 1 < sizeof k; s++) k[i++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
    k[i] = 0;
    cache_path(j->cache, sizeof j->cache, "Covers", k);
    ask(j);
}

void logo_cancel(int c)
{
    if (!lock) return;
    sys_lock(lock);
    ch[c].gen++;
    ch[c].have = false;
    free(ch[c].res);
    ch[c].res = NULL;
    sys_unlock(lock);
}

bool logo_take(int c, uint8_t **data, size_t *len)
{
    if (!lock) return false;
    sys_lock(lock);
    bool h = ch[c].have;
    if (h) { *data = ch[c].res; *len = ch[c].len; ch[c].res = NULL; ch[c].have = false; }
    sys_unlock(lock);
    return h;
}
