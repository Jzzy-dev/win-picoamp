// stream.c - internet radio (see stream.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "stream.h"
#include "net.h"
#include "sys.h"
#include "util.h"
#include "aacdec.h"

#define RING_SIZE (512 * 1024)     // about 30 s of 128 kbit/s

struct stream
{
    sys_mutex *lock;
    int        refs;               // the thread and the player
    volatile bool stop;
    char       url[2048];
    unsigned char *ring;
    size_t     rpos, count;
    stream_info info;
};

static void unref(stream *s)
{
    sys_lock(s->lock);
    int r = --s->refs;
    sys_unlock(s->lock);
    if (r == 0)
    {
        free(s->ring);
        free(s);                    // (the mutex is small: left)
    }
}

static void fail(stream *s, const char *msg)
{
    sys_lock(s->lock);
    if (!s->info.failed) snprintf(s->info.error, sizeof s->info.error, "%s", msg);
    s->info.failed = true;
    sys_unlock(s->lock);
}

// the first stream URL of a .pls or .m3u text
static bool url_in_list(const char *text, char *out, size_t outlen)
{
    for (const char *p = text; *p; )
    {
        const char *e = p + strcspn(p, "\r\n");
        const char *v = p;
        while (v < e && (*v == ' ' || *v == '\t')) v++;
        if (!u_strnicmp(v, "File", 4))        // File1=http://...
        {
            const char *eq = memchr(v, '=', (size_t)(e - v));
            if (eq) v = eq + 1;
        }
        if ((size_t)(e - v) > 8 && (!strncmp(v, "http://", 7) || !strncmp(v, "https://", 8)))
        {
            size_t l = (size_t)(e - v);
            while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
            if (l >= outlen) l = outlen - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return true;
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

static void put(stream *s, const unsigned char *p, size_t n)
{
    while (n && !s->stop)
    {
        sys_lock(s->lock);
        size_t room = RING_SIZE - s->count;
        size_t k = n < room ? n : room;
        for (size_t i = 0; i < k; i++) s->ring[(s->rpos + s->count + i) % RING_SIZE] = p[i];
        s->count += k;
        s->info.fill_pct = (int)(s->count * 100 / RING_SIZE);
        sys_unlock(s->lock);
        p += k;
        n -= k;
        if (n) sys_sleep_ms(20);       // full: the player is behind
    }
}

// StreamTitle='Artist - Song';StreamUrl='';
static void take_meta(stream *s, const char *m)
{
    const char *t = strstr(m, "StreamTitle='");
    if (!t) return;
    t += 13;
    const char *e = strstr(t, "';");
    if (!e) e = t + strlen(t);
    while (e > t && e[-1] == '\'') e--;
    char title[256];
    size_t l = (size_t)(e - t);
    if (l >= sizeof title) l = sizeof title - 1;
    memcpy(title, t, l);
    title[l] = 0;
    sys_lock(s->lock);
    if (strcmp(title, s->info.title))
    {
        snprintf(s->info.title, sizeof s->info.title, "%s", title);
        s->info.meta_gen++;
    }
    sys_unlock(s->lock);
}

// kbit/s of the first MPEG audio frame header in p
static int mp3_bitrate(const unsigned char *p, size_t n)
{
    static const short br[2][16] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },   // MPEG1 L3
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }        // MPEG2/2.5 L3
    };
    for (size_t i = 0; i + 4 <= n; i++)
        if (p[i] == 0xFF && (p[i + 1] & 0xE0) == 0xE0 && ((p[i + 1] >> 1) & 3) == 1)
        {
            int ver = (p[i + 1] >> 3) & 3;      // 3 = MPEG1
            int idx = p[i + 2] >> 4;
            int b = br[ver == 3 ? 0 : 1][idx];
            if (b) return b;
        }
    return 0;
}

static int run(void *arg)
{
    stream *s = arg;
    char url[2048];
    snprintf(url, sizeof url, "%s", s->url);
    int retries = 0;
    for (int hop = 0; hop < 4 && !s->stop; hop++)
    {
        char err[160];
        int st;
        net_conn *c = net_get(url, "Icy-MetaData: 1", &st, err, sizeof err, &s->stop);
        if (!c)
        {
            if (!s->stop) fail(s, err);
            break;
        }
        const char *ct = net_header(c, "Content-Type");
        char type[64] = "";
        if (ct) snprintf(type, sizeof type, "%s", ct);
        for (char *p = type; *p; p++) *p = (char)tolower((unsigned char)*p);
        char *semi = strchr(type, ';');
        if (semi) *semi = 0;
        // a list that points at the stream
        bool is_list = strstr(type, "scpls") || strstr(type, "mpegurl") || strstr(type, "x-mpeg-url") ||
                       (!strncmp(type, "text/", 5) && (u_ends_with_ci(url, ".pls") || u_ends_with_ci(url, ".m3u") || u_ends_with_ci(url, ".m3u8")));
        if (is_list || ((u_ends_with_ci(url, ".pls") || u_ends_with_ci(url, ".m3u")) && strncmp(type, "audio/", 6)))
        {
            char body[16384];
            size_t n = 0;
            long r;
            while (n < sizeof body - 1 && (r = net_read(c, body + n, sizeof body - 1 - n)) > 0) n += (size_t)r;
            body[n] = 0;
            net_close(c);
            if (strstr(body, "#EXT-X-")) { fail(s, "HLS streams are not supported yet"); break; }
            if (!url_in_list(body, url, sizeof url)) { fail(s, "The list has no stream in it"); break; }
            continue;
        }
        bool mp3 = !type[0] || strstr(type, "mpeg") || strstr(type, "mp3") || !strcmp(type, "application/octet-stream");
        static int aac_ok = -1;
        if (aac_ok < 0) aac_ok = aacdec_available();
        if (strstr(type, "aac") && aac_ok) mp3 = true;          // (AAC: Windows decodes it)
        if (!mp3)
        {
            char m[160];
            if (strstr(type, "aac")) snprintf(m, sizeof m, "AAC streams are not supported yet");
            else if (strstr(type, "ogg")) snprintf(m, sizeof m, "Ogg streams are not supported yet");
            else snprintf(m, sizeof m, "Not an MP3 stream (%s)", type);
            fail(s, m);
            net_close(c);
            break;
        }
        int metaint = 0;
        const char *mi = net_header(c, "icy-metaint");
        if (mi) metaint = atoi(mi);
        sys_lock(s->lock);
        const char *nm = net_header(c, "icy-name");
        if (nm && *nm && !s->info.station[0]) snprintf(s->info.station, sizeof s->info.station, "%s", nm);
        const char *br = net_header(c, "icy-br");
        if (br) s->info.bitrate = atoi(br);
        snprintf(s->info.type, sizeof s->info.type, "%s", type);
        s->info.tls = net_is_tls(c);
        s->info.connected = true;
        sys_unlock(s->lock);

        unsigned char buf[16384];
        int until_meta = metaint;
        bool got_rate = s->info.bitrate > 0;
        long r;
        while (!s->stop && (r = net_read(c, buf, sizeof buf)) > 0)
        {
            retries = 0;
            size_t n = (size_t)r, p = 0;
            while (p < n)
            {
                if (metaint && until_meta == 0)
                {
                    // one length byte (x16), then the metadata: may span reads
                    int ml = buf[p++] * 16;
                    char meta[4096 + 1];
                    int got = 0;
                    while (got < ml)
                    {
                        if (p < n) { int k = (int)(n - p) < ml - got ? (int)(n - p) : ml - got; memcpy(meta + got, buf + p, (size_t)k); got += k; p += (size_t)k; }
                        else
                        {
                            r = net_read(c, buf, sizeof buf);
                            if (r <= 0) break;
                            n = (size_t)r;
                            p = 0;
                        }
                    }
                    meta[got] = 0;
                    if (got) take_meta(s, meta);
                    until_meta = metaint;
                    continue;
                }
                size_t k = n - p;
                if (metaint && (size_t)until_meta < k) k = (size_t)until_meta;
                if (!got_rate && !strstr(type, "aac"))
                {
                    int b = mp3_bitrate(buf + p, k);
                    if (b) { sys_lock(s->lock); s->info.bitrate = b; sys_unlock(s->lock); got_rate = true; }
                }
                put(s, buf + p, k);
                p += k;
                if (metaint) until_meta -= (int)k;
            }
        }
        net_close(c);
        if (s->stop) break;
        // the server closed the stream: try again a few times
        if (++retries <= 3) { hop--; sys_sleep_ms(1000); continue; }
        fail(s, "The stream stopped");
        break;
    }
    sys_lock(s->lock);
    s->info.ended = true;
    sys_unlock(s->lock);
    unref(s);
    return 0;
}

stream *stream_start(const char *url)
{
    stream *s = calloc(1, sizeof *s);
    s->lock = sys_mutex_new();
    s->ring = malloc(RING_SIZE);
    s->refs = 2;
    snprintf(s->url, sizeof s->url, "%s", url);
    sys_thread *t = sys_thread_start(run, s);
    if (!t) { s->refs = 1; fail(s, "No thread"); s->info.ended = true; }
    else sys_thread_detach(t);
    // the thread is never joined: it ends by itself once stopped
    return s;
}

size_t stream_read(stream *s, void *buf, size_t n, volatile bool *stop)
{
    for (;;)
    {
        sys_lock(s->lock);
        size_t k = s->count < n ? s->count : n;
        for (size_t i = 0; i < k; i++) ((unsigned char *)buf)[i] = s->ring[(s->rpos + i) % RING_SIZE];
        s->rpos = (s->rpos + k) % RING_SIZE;
        s->count -= k;
        s->info.fill_pct = (int)(s->count * 100 / RING_SIZE);
        bool done = s->info.ended || s->info.failed;
        sys_unlock(s->lock);
        if (k) return k;
        if (done || s->stop || (stop && *stop)) return 0;
        sys_sleep_ms(10);
    }
}

size_t stream_buffered(stream *s)
{
    sys_lock(s->lock);
    size_t n = s->count;
    sys_unlock(s->lock);
    return n;
}

size_t stream_capacity(stream *s) { (void)s; return RING_SIZE; }

void stream_info_get(stream *s, stream_info *out)
{
    sys_lock(s->lock);
    *out = s->info;
    sys_unlock(s->lock);
}

void stream_release(stream *s)
{
    if (!s) return;
    s->stop = true;
    unref(s);
}
