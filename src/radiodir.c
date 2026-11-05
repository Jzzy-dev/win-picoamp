// radiodir.c - the Radio Browser station directory (see radiodir.h)
//
// Radio Browser is a free, community run directory of internet radio stations
// with a JSON API (the engine's pva_dir.c uses the same):
//   countries   /json/countries?hidebroken=true&order=name
//   stations    /json/stations/search?...&hidebroken=true&order=clickcount&reverse=true
//   a click     /json/url/<stationuuid>   (asked for by Radio Browser when a station starts)
// The servers are tried in turn. HLS streams and codecs PicoAmp does not
// play are left out.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radiodir.h"
#include "json.h"
#include "net.h"
#include "sys.h"
#include "util.h"

static const char *const servers[] = {
    "all.api.radio-browser.info", "de1.api.radio-browser.info", "de2.api.radio-browser.info",
    "fi1.api.radio-browser.info", "nl1.api.radio-browser.info", "at1.api.radio-browser.info",
};
#define N_SERVERS (sizeof servers / sizeof servers[0])

static sys_mutex *lock;
static rd_item   *items;            // the list shown
static int        n_items, n_raw, state = RD_IDLE;
static char       err[160];
static volatile unsigned gen;       // a newer request makes an older one's reply worthless

typedef struct { char path[1024]; bool countries, append; unsigned gen; } job_t;

bool rd_codec_ok(const char *codec)
{
    return !u_stricmp(codec, "MP3");
}

void rd_urlenc(char *out, size_t size, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < size; s++)
    {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

// where the directory is: PICOAMP_RB (the tests) or its servers
static int base_count(void) { return getenv("PICOAMP_RB") ? 1 : (int)N_SERVERS; }
static void base_url(int i, char *out, size_t n)
{
    const char *e = getenv("PICOAMP_RB");
    if (e) { u_copy(out, n, e); return; }
#ifdef _WIN32
    snprintf(out, n, "https://%s", servers[i]);
#else
    snprintf(out, n, "http://%s", servers[i]);
#endif
}

// ---------------- the reply ----------------
typedef struct
{
    bool     countries;
    rd_item *v;
    int      n, cap, raw;
    rd_item  cur;
    bool     hls, broken;
    char     url_plain[1024];
} parse_t;

static void on_json(json_t *j, int ev, const char *path, const char *val, uint16_t idx)
{
    (void)idx;
    parse_t *p = j->user;
    if (ev == JSON_END)
    {
        if (strcmp(path, "")) return;           // the top-level array's objects only
        p->raw++;
        rd_item *c = &p->cur;
        if (!c->url[0]) u_copy(c->url, sizeof c->url, p->url_plain);
        bool ok = p->countries ? c->name[0] && c->count > 0
                               : c->name[0] && u_is_url(c->url) && !p->hls && !p->broken && rd_codec_ok(c->codec);
        if (ok)
        {
            if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 128; p->v = realloc(p->v, sizeof *p->v * (size_t)p->cap); }
            p->v[p->n++] = *c;
        }
        memset(c, 0, sizeof *c);
        p->hls = p->broken = false;
        p->url_plain[0] = 0;
        return;
    }
    rd_item *c = &p->cur;
    if (!strcmp(path, "name")) { u_copy(c->name, sizeof c->name, val); u_trim(c->name); }
    else if (!strcmp(path, "url_resolved")) u_copy(c->url, sizeof c->url, val);
    else if (!strcmp(path, "url")) u_copy(p->url_plain, sizeof p->url_plain, val);
    else if (!strcmp(path, "stationuuid")) u_copy(c->uuid, sizeof c->uuid, val);
    else if (!strcmp(path, "codec")) u_copy(c->codec, sizeof c->codec, val);
    else if (!strcmp(path, "bitrate")) c->bitrate = atoi(val);
    else if (!strcmp(path, "stationcount")) c->count = atoi(val);
    else if (!strcmp(path, "iso_3166_1")) u_copy(c->code, sizeof c->code, val);
    else if (!strcmp(path, "hls")) p->hls = !strcmp(val, "1") || !strcmp(val, "true");
    else if (!strcmp(path, "lastcheckok")) p->broken = !strcmp(val, "0") || !strcmp(val, "false");
}

static int run(void *arg)
{
    job_t *jb = arg;
    parse_t p;
    memset(&p, 0, sizeof p);
    p.countries = jb->countries;
    char e[160] = "No answer from the station directory";
    bool ok = false;
    for (int i = 0; i < base_count() && !ok && jb->gen == gen; i++)
    {
        char url[1400], base[256];
        base_url(i, base, sizeof base);
        snprintf(url, sizeof url, "%s%s", base, jb->path);
        int st;
        char ne[160];
        volatile bool stop = false;
        net_conn *c = net_get(url, NULL, &st, ne, sizeof ne, &stop);
        if (!c) { if (ne[0]) u_copy(e, sizeof e, ne); continue; }
        json_t *j = calloc(1, sizeof *j);
        j->cb = on_json;
        j->user = &p;
        json_reset(j);
        char buf[16384];
        long r;
        while ((r = net_read(c, buf, sizeof buf)) > 0 && jb->gen == gen) json_feed(j, buf, (size_t)r);
        ok = j->done;
        free(j);
        net_close(c);
        if (!ok) { free(p.v); memset(&p, 0, sizeof p); p.countries = jb->countries; snprintf(e, sizeof e, "The station directory's reply broke off"); }
    }
    sys_lock(lock);
    if (jb->gen == gen)
    {
        if (ok)
        {
            if (jb->append)
            {
                items = realloc(items, sizeof *items * (size_t)(n_items + p.n + 1));
                memcpy(items + n_items, p.v, sizeof *items * (size_t)p.n);
                n_items += p.n;
                free(p.v);
            }
            else
            {
                free(items);
                items = p.v;
                n_items = p.n;
            }
            p.v = NULL;
            n_raw = p.raw;
            state = RD_DONE;
        }
        else
        {
            u_copy(err, sizeof err, e);
            state = RD_FAILED;
        }
    }
    sys_unlock(lock);
    free(p.v);
    free(jb);
    return 0;
}

void rd_fetch(const char *path, bool countries, bool append)
{
    if (!lock) lock = sys_mutex_new();
    job_t *jb = calloc(1, sizeof *jb);
    u_copy(jb->path, sizeof jb->path, path);
    jb->countries = countries;
    jb->append = append;
    sys_lock(lock);
    jb->gen = ++gen;
    state = RD_LOADING;
    err[0] = 0;
    sys_unlock(lock);
    sys_thread *t = sys_thread_start(run, jb);
    if (!t) { free(jb); state = RD_FAILED; u_copy(err, sizeof err, "No thread"); }
    else sys_thread_detach(t);
}

int rd_state(void)
{
    if (!lock) return RD_IDLE;
    sys_lock(lock);
    int s = state;
    sys_unlock(lock);
    return s;
}

const char *rd_error(void) { return err; }

const rd_item *rd_items(int *count, int *raw)
{
    if (count) *count = n_items;
    if (raw) *raw = n_raw;
    return items;
}

void rd_cancel(void)
{
    if (!lock) return;
    sys_lock(lock);
    gen++;
    if (state == RD_LOADING) state = RD_IDLE;
    sys_unlock(lock);
}

static int click_run(void *arg)
{
    char *uuid = arg;
    for (int i = 0; i < base_count(); i++)
    {
        char base[256], url[512];
        base_url(i, base, sizeof base);
        snprintf(url, sizeof url, "%s/json/url/%s", base, uuid);
        int st;
        char e[100];
        net_conn *c = net_get(url, NULL, &st, e, sizeof e, NULL);
        if (c) { char b[2048]; while (net_read(c, b, sizeof b) > 0) {} net_close(c); break; }
    }
    free(uuid);
    return 0;
}

void rd_click(const char *uuid)
{
    if (!uuid || !uuid[0]) return;
    sys_thread_detach(sys_thread_start(click_run, strdup(uuid)));
}
