// logo.c - a station's logo as its cover, found as the engine finds it (MSX
// engine 0.15): Radio Browser is asked for the station (by its id when it
// comes from the directory, else by its address, else by its exact name),
// its "favicon" is fetched, and the picture is kept in <config>/Logos so the
// next start shows it at once ("no logo" is kept too). The fetch runs in a
// thread; the main loop takes the result (logo_take) and makes the cover.
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
static unsigned   gen;                  // the station asked for last
static bool       have;                 // a result for it
static uint8_t   *res;                  // its picture (NULL: none)
static size_t     res_len;

typedef struct { char url[2048], uuid[64], name[256]; unsigned gen; } job_t;

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static void cache_path(char *out, size_t n, const char *url, const char *ext)
{
    char d[1024], f[32];
    u_join(d, sizeof d, sys_config_dir(), "Logos");
    snprintf(f, sizeof f, "%08X.%s", (unsigned)fnv(url), ext);
    u_join(out, n, d, f);
}

static void deliver(unsigned g, uint8_t *p, size_t n)
{
    sys_lock(lock);
    if (g == gen) { free(res); res = p; res_len = n; have = true; p = NULL; }
    sys_unlock(lock);
    free(p);
}

// "favicon":"..." of the first station in a reply ("" none; \/ is /)
static void favicon(const char *j, char *out, size_t n)
{
    out[0] = 0;
    const char *p = j ? strstr(j, "\"favicon\"") : NULL;
    if (!p) return;
    p += 9;
    while (*p == ' ' || *p == ':') p++;
    if (*p++ != '"') return;
    size_t k = 0;
    for (; *p && *p != '"' && k + 1 < n; p++)
        if (*p != '\\') out[k++] = *p;
    out[k] = 0;
}

static bool is_picture(const uint8_t *b, size_t n)
{
    if (n < 8) return false;
    return (b[0] == 0xFF && b[1] == 0xD8) || !memcmp(b, "\x89PNG", 4) || !memcmp(b, "GIF8", 4) ||
           !memcmp(b, "RIFF", 4) || !memcmp(b, "\0\0\1\0", 4) || !memcmp(b, "BM", 2);
}

static int run(void *arg)
{
    job_t *j = arg;
    char path[1100], e[100], q[1400], enc[1200], fav[1024];
    bool answered = false;
    fav[0] = 0;
    for (int way = j->uuid[0] ? 0 : 1; way < 3 && !fav[0] && j->gen == gen; way++)
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
            answered = true;
            favicon(r, fav, sizeof fav);
            free(r);
            break;
        }
        if (fav[0] && strncmp(fav, "http://", 7) && strncmp(fav, "https://", 8)) fav[0] = 0;
    }
    uint8_t *img = NULL;
    size_t n = 0;
    bool known = answered;
    if (fav[0] && j->gen == gen)
    {
        img = (uint8_t *)net_fetch(fav, IMG_MAX, &n, e, sizeof e);
        if (img && !is_picture(img, n)) { free(img); img = NULL; }
        else if (!img) known = false;           // (the server away: asked again next time)
    }
    if (known)
    {
        char d[1024];
        u_join(d, sizeof d, sys_config_dir(), "Logos");
        sys_mkdir(d);
        cache_path(path, sizeof path, j->url, img ? "img" : "none");
        sys_write_file(path, img ? (const void *)img : "", img ? n : 0);
    }
    deliver(j->gen, img, n);
    free(j);
    return 0;
}

void logo_station(const char *url, const char *uuid, const char *name)
{
    if (!lock) lock = sys_mutex_new();
    sys_lock(lock);
    gen++;
    have = false;
    free(res);
    res = NULL;
    unsigned g = gen;
    sys_unlock(lock);
    if (!url || !url[0]) { deliver(g, NULL, 0); return; }
    // known already
    char p[1100];
    cache_path(p, sizeof p, url, "img");
    size_t n = 0;
    char *d = sys_file_exists(p) ? sys_read_file(p, &n) : NULL;
    if (d) { deliver(g, (uint8_t *)d, n); return; }
    cache_path(p, sizeof p, url, "none");
    if (sys_file_exists(p)) { deliver(g, NULL, 0); return; }
    job_t *j = calloc(1, sizeof *j);
    if (!j) { deliver(g, NULL, 0); return; }
    u_copy(j->url, sizeof j->url, url);
    u_copy(j->uuid, sizeof j->uuid, uuid ? uuid : "");
    u_copy(j->name, sizeof j->name, name ? name : "");
    j->gen = g;
    sys_thread_detach(sys_thread_start(run, j));
}

void logo_cancel(void)
{
    if (!lock) return;
    sys_lock(lock);
    gen++;
    have = false;
    free(res);
    res = NULL;
    sys_unlock(lock);
}

bool logo_take(uint8_t **data, size_t *len)
{
    if (!lock) return false;
    sys_lock(lock);
    bool h = have;
    if (h) { *data = res; *len = res_len; res = NULL; have = false; }
    sys_unlock(lock);
    return h;
}
