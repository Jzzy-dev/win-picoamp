// wxsvc.c - the place, its zone and its weather (see wxsvc.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wxsvc.h"
#include "config.h"
#include "json.h"
#include "net.h"
#include "radiodir.h"
#include "sys.h"
#include "util.h"

#define REFRESH_MS   (30u * 60u * 1000u)
#define IP_EVERY_MS  (6u * 60u * 60u * 1000u)
#define RETRY_MS     60000u
#define MAX_RESULTS  8

static sys_mutex *lock;
static wx_info    info;
static volatile bool want_refresh, want_ip, quit;
static char       search_q[100];
static volatile bool want_search;
static int        s_state = WXS_IDLE;
static struct { char name[160]; double lat, lon; } res[MAX_RESULTS];
static int        nres;
static double     lat, lon;
static bool       have_ll;

static const char *base(const char *env, const char *def)
{
    const char *e = getenv(env);
    return e ? e : def;
}
#ifdef _WIN32
#define METEO_DEF "https://api.open-meteo.com"
#define GEO_DEF   "https://geocoding-api.open-meteo.com"
#else
#define METEO_DEF "http://api.open-meteo.com"
#define GEO_DEF   "http://geocoding-api.open-meteo.com"
#endif

static void changed(void) { info.gen++; }

// a whole JSON reply through the reader
static bool fetch_json(const char *url, json_cb cb, void *user, char *err, size_t errlen)
{
    int st;
    net_conn *c = net_get(url, NULL, &st, err, errlen, &quit);
    if (!c) return false;
    json_t *j = calloc(1, sizeof *j);
    j->cb = cb;
    j->user = user;
    json_reset(j);
    char buf[8192];
    long r;
    while ((r = net_read(c, buf, sizeof buf)) > 0) json_feed(j, buf, (size_t)r);
    bool ok = j->done;
    free(j);
    net_close(c);
    if (!ok) snprintf(err, errlen, "The reply broke off");
    return ok;
}

static int tenths(const char *v)
{
    double d = atof(v);
    return (int)(d * 10.0 + (d < 0 ? -0.5 : 0.5));
}

// ---------------- ip-api.com ----------------
typedef struct { char city[64], region[64], cc[8]; double lat, lon; bool ok, have_ll; } ip_t;

static void on_ip(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)idx;
    ip_t *p = j->user;
    if (ev != JSON_VALUE) return;
    if (!strcmp(path, "status")) p->ok = !strcmp(v, "success");
    else if (!strcmp(path, "city")) u_copy(p->city, sizeof p->city, v);
    else if (!strcmp(path, "regionName")) u_copy(p->region, sizeof p->region, v);
    else if (!strcmp(path, "countryCode")) u_copy(p->cc, sizeof p->cc, v);
    else if (!strcmp(path, "lat")) { p->lat = atof(v); p->have_ll = true; }
    else if (!strcmp(path, "lon")) p->lon = atof(v);
}

static void place_name(char *out, size_t n, const char *city, const char *region, const char *cc)
{
    snprintf(out, n, "%s", city);
    if (region[0] && strcmp(region, city)) { size_t l = strlen(out); snprintf(out + l, n - l, ", %s", region); }
    if (cc[0]) { size_t l = strlen(out); snprintf(out + l, n - l, ", %s", cc); }
}

static bool ask_ip(void)
{
    ip_t p;
    memset(&p, 0, sizeof p);
    char url[300], err[120];
    snprintf(url, sizeof url, "%s/json/?fields=status,message,countryCode,regionName,city,lat,lon,timezone,offset",
             base("PICOAMP_IPAPI", "http://ip-api.com"));
    if (!fetch_json(url, on_ip, &p, err, sizeof err) || !p.ok || !p.have_ll)
    {
        sys_lock(lock);
        snprintf(info.error, sizeof info.error, "No place from ip-api.com%s%s", err[0] ? ": " : "", err);
        changed();
        sys_unlock(lock);
        return false;
    }
    sys_lock(lock);
    place_name(info.place, sizeof info.place, p.city, p.region, p.cc);
    info.have_place = true;
    lat = p.lat;
    lon = p.lon;
    have_ll = true;
    info.error[0] = 0;
    changed();
    sys_unlock(lock);
    return true;
}

// ---------------- Open-Meteo's forecast ----------------
typedef struct { wx_info w; bool have_off, have_cur; } met_t;

static void on_meteo(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    met_t *m = j->user;
    if (ev != JSON_VALUE) return;
    wx_info *w = &m->w;
    if (!strcmp(path, "utc_offset_seconds")) { w->utc_offset = atoi(v); m->have_off = true; }
    else if (!strncmp(path, "current.", 8))
    {
        const char *f = path + 8;
        if (!strcmp(f, "temperature_2m")) { w->t10 = tenths(v); m->have_cur = true; }
        else if (!strcmp(f, "apparent_temperature")) w->feel10 = tenths(v);
        else if (!strcmp(f, "relative_humidity_2m")) w->hum = atoi(v);
        else if (!strcmp(f, "wind_speed_10m")) w->wind = (tenths(v) + 5) / 10;
        else if (!strcmp(f, "weather_code")) w->code = atoi(v);
        else if (!strcmp(f, "is_day")) w->is_day = atoi(v) != 0;
    }
    else if (!strncmp(path, "daily.", 6) && idx < WX_DAYS)
    {
        const char *f = path + 6;
        wx_day *d = &w->days[idx];
        if (!strcmp(f, "time"))
        {
            sscanf(v, "%d-%d-%d", &d->year, &d->month, &d->day);
            struct tm tm = { 0 };
            tm.tm_year = d->year - 1900;
            tm.tm_mon = d->month - 1;
            tm.tm_mday = d->day;
            tm.tm_hour = 12;
            mktime(&tm);
            d->wday = (tm.tm_wday + 6) % 7;
            if (idx + 1 > w->ndays) w->ndays = idx + 1;
        }
        else if (!strcmp(f, "weather_code")) { d->code = atoi(v); d->ok = true; }
        else if (!strcmp(f, "temperature_2m_max")) d->tmax10 = tenths(v);
        else if (!strcmp(f, "temperature_2m_min")) d->tmin10 = tenths(v);
        else if (!strcmp(f, "precipitation_probability_max")) d->pp = v[0] == 'n' ? -1 : atoi(v);
    }
}

static bool ask_meteo(void)
{
    met_t m;
    memset(&m, 0, sizeof m);
    for (int i = 0; i < WX_DAYS; i++) m.w.days[i].pp = -1;
    char url[600], err[120];
    snprintf(url, sizeof url, "%s/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&timezone=auto&forecast_days=%d&current=temperature_2m,weather_code,apparent_temperature,"
             "relative_humidity_2m,wind_speed_10m,is_day&daily=weather_code,temperature_2m_max,"
             "temperature_2m_min,precipitation_probability_max", base("PICOAMP_METEO", METEO_DEF), lat, lon, WX_DAYS);
    bool ok = fetch_json(url, on_meteo, &m, err, sizeof err) && m.have_off;
    sys_lock(lock);
    if (ok)
    {
        info.utc_offset = m.w.utc_offset;
        info.have_zone = true;
        if (m.have_cur)
        {
            info.t10 = m.w.t10; info.feel10 = m.w.feel10; info.hum = m.w.hum; info.wind = m.w.wind;
            info.code = m.w.code; info.is_day = m.w.is_day;
            memcpy(info.days, m.w.days, sizeof info.days);
            info.ndays = m.w.ndays;
            info.have_wx = true;
        }
        info.error[0] = 0;
    }
    else snprintf(info.error, sizeof info.error, "No weather from Open-Meteo%s%s", err[0] ? ": " : "", err);
    changed();
    sys_unlock(lock);
    return ok;
}

// ---------------- the place search ----------------
typedef struct { char name[64], admin[64], cc[8]; double lat, lon; bool ll; } geo_cur;

static void on_geo(json_t *j, int ev, const char *path, const char *v, uint16_t idx)
{
    (void)idx;
    geo_cur *g = j->user;
    if (ev == JSON_END && !strcmp(path, "results"))
    {
        if (g->name[0] && g->ll && nres < MAX_RESULTS)
        {
            place_name(res[nres].name, sizeof res[nres].name, g->name, g->admin, g->cc);
            res[nres].lat = g->lat;
            res[nres].lon = g->lon;
            nres++;
        }
        memset(g, 0, sizeof *g);
        return;
    }
    if (ev != JSON_VALUE || strncmp(path, "results.", 8)) return;
    const char *f = path + 8;
    if (!strcmp(f, "name")) u_copy(g->name, sizeof g->name, v);
    else if (!strcmp(f, "admin1")) u_copy(g->admin, sizeof g->admin, v);
    else if (!strcmp(f, "country_code")) u_copy(g->cc, sizeof g->cc, v);
    else if (!strcmp(f, "latitude")) { g->lat = atof(v); g->ll = true; }
    else if (!strcmp(f, "longitude")) g->lon = atof(v);
}

static void do_search(void)
{
    char q[100], enc[300], url[500], err[120];
    sys_lock(lock);
    u_copy(q, sizeof q, search_q);
    sys_unlock(lock);
    rd_urlenc(enc, sizeof enc, q);
    snprintf(url, sizeof url, "%s/v1/search?name=%s&count=%d&language=en&format=json", base("PICOAMP_GEO", GEO_DEF), enc, MAX_RESULTS);
    geo_cur g;
    memset(&g, 0, sizeof g);
    nres = 0;
    bool ok = fetch_json(url, on_geo, &g, err, sizeof err);
    sys_lock(lock);
    s_state = ok ? WXS_DONE : WXS_FAILED;
    sys_unlock(lock);
}

// ---------------- the thread ----------------
static int run(void *arg)
{
    (void)arg;
    uint32_t next = 0, ip_at = 0, retry = RETRY_MS;
    bool ip_done = false;
    while (!quit)
    {
        uint32_t now = sys_millis();
        if (want_search) { want_search = false; do_search(); continue; }
        bool mode_ip = cfg.loc_ip;
        if (mode_ip && (want_ip || !ip_done || now - ip_at >= IP_EVERY_MS) && (want_ip || (int32_t)(now - next) >= 0))
        {
            want_ip = false;
            ip_at = now;
            if (ask_ip()) { ip_done = true; want_refresh = true; retry = RETRY_MS; }
            else { next = now + retry; if (retry < 10 * 60000u) retry *= 2; }
            continue;
        }
        if (have_ll && (want_refresh || (int32_t)(now - next) >= 0))
        {
            want_refresh = false;
            if (ask_meteo()) { next = now + REFRESH_MS; retry = RETRY_MS; }
            else { next = now + retry; if (retry < 10 * 60000u) retry *= 2; }
            continue;
        }
        sys_sleep_ms(100);
    }
    return 0;
}

void wx_start(void)
{
    if (lock) return;
    lock = sys_mutex_new();
    if (!cfg.loc_ip && cfg.loc_name[0])
    {
        u_copy(info.place, sizeof info.place, cfg.loc_name);
        info.have_place = true;
        lat = cfg.loc_lat;
        lon = cfg.loc_lon;
        have_ll = true;
    }
    want_refresh = true;
    sys_thread_detach(sys_thread_start(run, NULL));
}

void wx_get(wx_info *out)
{
    if (!lock) { memset(out, 0, sizeof *out); return; }
    sys_lock(lock);
    *out = info;
    sys_unlock(lock);
}

void wx_refresh(void) { want_refresh = true; }

bool wx_now(wx_time *t, bool place_time)
{
    time_t now = time(NULL);
    struct tm tm;
    bool placed = false;
    if (place_time && lock)
    {
        sys_lock(lock);
        bool hz = info.have_zone;
        int off = info.utc_offset;
        sys_unlock(lock);
        if (hz)
        {
            time_t l = now + off;
#ifdef _WIN32
            gmtime_s(&tm, &l);
#else
            gmtime_r(&l, &tm);
#endif
            placed = true;
        }
    }
    if (!placed)
    {
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
    }
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->wday = (tm.tm_wday + 6) % 7;
    t->hour = tm.tm_hour;
    t->min = tm.tm_min;
    t->sec = tm.tm_sec;
    return placed;
}

void wx_search(const char *name)
{
    if (!lock) return;
    sys_lock(lock);
    u_copy(search_q, sizeof search_q, name);
    s_state = WXS_BUSY;
    sys_unlock(lock);
    want_search = true;
}

int wx_search_state(void)
{
    if (!lock) return WXS_IDLE;
    sys_lock(lock);
    int s = s_state;
    sys_unlock(lock);
    return s;
}

int wx_search_results(char names[][160], int max)
{
    int n = nres < max ? nres : max;
    for (int i = 0; i < n; i++) u_copy(names[i], 160, res[i].name);
    return n;
}

void wx_choose(int i)
{
    if (i < 0 || i >= nres) return;
    sys_lock(lock);
    cfg.loc_ip = false;
    u_copy(cfg.loc_name, sizeof cfg.loc_name, res[i].name);
    cfg.loc_lat = res[i].lat;
    cfg.loc_lon = res[i].lon;
    u_copy(info.place, sizeof info.place, res[i].name);
    info.have_place = true;
    info.have_zone = info.have_wx = false;
    lat = res[i].lat;
    lon = res[i].lon;
    have_ll = true;
    changed();
    sys_unlock(lock);
    want_refresh = true;
}

void wx_use_ip(void)
{
    cfg.loc_ip = true;
    cfg.loc_name[0] = 0;
    want_ip = true;
}

const char *wx_words(int code)
{
    static const struct { int max; const char *w; } t[] = {
        { 0, "Clear" }, { 1, "Mainly clear" }, { 2, "Partly cloudy" }, { 3, "Overcast" }, { 48, "Fog" },
        { 57, "Drizzle" }, { 67, "Rain" }, { 77, "Snow" }, { 82, "Showers" }, { 86, "Snow showers" }, { 255, "Thunderstorm" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (code <= t[i].max) return t[i].w;
    return "";
}
