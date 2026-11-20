// msxlink.c - the MSX link (see msxlink.h).
//
//   the audio threads -> the PCM ring -> the encoder thread (to 44.1 kHz, MP3)
//   -> the MP3 ring -> a thread for each MSX that listens (ICY titles put in)
//
// The stream goes on in real time: when nothing plays here, it is silence, so
// the MSX keeps its station and its buffer. The encoder runs only while an MSX
// listens. Each listener starts a second behind the newest MP3 (it fills its
// buffer at once) and is moved on when it falls behind.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define SEND_FLAGS 0
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define SEND_FLAGS MSG_NOSIGNAL           // (a listener that went: an error, not a signal)
#define sock_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"
#include "msxlink.h"
#include "mp3enc.h"
#include "jpegenc.h"
#include "audio.h"
#include "cover.h"
#include "gfx.h"
#include "ui.h"
#include "pages.h"
#include "sys.h"
#include "util.h"
#include "tjpgd.h"

#define RATE        44100
#define PCM_FRAMES  (2 * 96000)          // the PCM ring: 2 s at 96 kHz
#define MP3_RING    (512u * 1024u)
#define JOIN_MS     1000                 // a new listener starts this far behind
#define METAINT     8192
#define MAX_CLIENTS 4
#define SILENCE_MS  250                  // no sound for this long: silence, in time
#define COVER_PX    80

uint8_t *plat_decode_rgb(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h);

static volatile bool running;
static sock_t       lsock = BAD_SOCK;
static sys_thread  *accept_th, *enc_th;
static int          kbps = 160, port;
static char         url[96], err[160];
static sys_mutex   *lock;                // the counts, the cover
static volatile int clients, listeners;
static int          listeners_seen = -1;
static char         err_shown[160];

// the PCM ring (audio threads -> encoder)
static sys_mutex   *pcm_lock;
static int16_t     *pcm;
static uint32_t     pcm_head, pcm_count, pcm_rate;

// the MP3 ring (encoder -> listeners)
static sys_mutex   *mp3_lock;
static uint8_t     *mp3;
static uint64_t     mp3_total;

// the cover, made for the MSX
static uint8_t     *cover_jpg;
static size_t       cover_len;
static unsigned     cover_n, cover_seen = ~0u;

const char *msxlink_error(void) { return err; }
bool msxlink_on(void) { return running; }
int  msxlink_listeners(void) { return listeners; }
const char *msxlink_url(void) { return url; }

// ---------------- the sound ----------------
void msxlink_feed(const int16_t *s, uint32_t frames, uint32_t rate)
{
    if (!running || !listeners || !frames || !rate) return;
    sys_lock(pcm_lock);
    if (rate != pcm_rate) { pcm_rate = rate; pcm_head = pcm_count = 0; }
    for (uint32_t i = 0; i < frames; i++)
    {
        if (pcm_count == PCM_FRAMES) break;           // (the encoder is behind: the rest is lost)
        uint32_t w = (pcm_head + pcm_count) % PCM_FRAMES;
        pcm[2 * w] = s[2 * i];
        pcm[2 * w + 1] = s[2 * i + 1];
        pcm_count++;
    }
    sys_unlock(pcm_lock);
}

static void mp3_put(const uint8_t *b, int n)
{
    sys_lock(mp3_lock);
    for (int i = 0; i < n; i++) mp3[(mp3_total + (uint64_t)i) % MP3_RING] = b[i];
    mp3_total += (uint64_t)n;
    sys_unlock(mp3_lock);
}

static int enc_main(void *arg)
{
    (void)arg;
    enum { CHUNK = 4096 };
    int16_t *in = malloc(sizeof(int16_t) * 2 * CHUNK), *res = malloc(sizeof(int16_t) * 2 * (CHUNK * 2 + 64));
    uint8_t *out = malloc(256 * 1024);
    mp3enc *e = NULL;
    ma_data_converter conv;
    bool conv_ok = false;
    uint32_t conv_rate = 0, last_sound = 0, sil_t = 0;
    bool failed = false;
    while (running && in && res && out)
    {
        if (!listeners)
        {
            sys_lock(pcm_lock);
            pcm_head = pcm_count = 0;
            sys_unlock(pcm_lock);
            if (e) { mp3enc_close(e); e = NULL; }
            failed = false;
            sys_sleep_ms(50);
            continue;
        }
        if (!e && !failed)
        {
            e = mp3enc_open(RATE, 2, kbps);
            if (!e) { failed = true; snprintf(err, sizeof err, "MSX link: %s", mp3enc_error()); continue; }
            err[0] = 0;
            last_sound = 0;
            sil_t = sys_millis();
        }
        if (!e) { sys_sleep_ms(100); continue; }
        // the sound that came, else silence in time
        uint32_t n = 0, rate;
        sys_lock(pcm_lock);
        rate = pcm_rate;
        while (n < CHUNK && pcm_count)
        {
            in[2 * n] = pcm[2 * pcm_head];
            in[2 * n + 1] = pcm[2 * pcm_head + 1];
            pcm_head = (pcm_head + 1) % PCM_FRAMES;
            pcm_count--;
            n++;
        }
        sys_unlock(pcm_lock);
        uint32_t now = sys_millis();
        const int16_t *src = in;
        uint32_t frames = 0;
        if (n)
        {
            last_sound = now;
            if (rate == RATE) frames = n;
            else
            {
                if (!conv_ok || conv_rate != rate)
                {
                    if (conv_ok) ma_data_converter_uninit(&conv, NULL);
                    ma_data_converter_config cc = ma_data_converter_config_init(ma_format_s16, ma_format_s16, 2, 2, rate, RATE);
                    cc.resampling.algorithm = ma_resample_algorithm_linear;
                    conv_ok = ma_data_converter_init(&cc, NULL, &conv) == MA_SUCCESS;
                    conv_rate = rate;
                }
                if (conv_ok)
                {
                    ma_uint64 fi = n, fo = CHUNK * 2 + 64;
                    ma_data_converter_process_pcm_frames(&conv, in, &fi, res, &fo);
                    frames = (uint32_t)fo;
                    src = res;
                }
            }
            sil_t = now;
        }
        else if (now - last_sound > SILENCE_MS)
        {
            uint32_t ms = now - sil_t;
            frames = (uint32_t)((uint64_t)ms * RATE / 1000);
            if (frames > CHUNK) frames = CHUNK;
            sil_t += (uint32_t)((uint64_t)frames * 1000 / RATE);
            memset(in, 0, sizeof(int16_t) * 2 * frames);
            src = in;
        }
        if (frames)
        {
            int got = mp3enc_encode(e, src, (int)frames, out, 256 * 1024);
            if (got > 0) mp3_put(out, got);
        }
        if (n < CHUNK) sys_sleep_ms(10);
    }
    if (e) mp3enc_close(e);
    if (conv_ok) ma_data_converter_uninit(&conv, NULL);
    free(in);
    free(res);
    free(out);
    return 0;
}

// ---------------- the cover (the screen's thread) ----------------
typedef struct { const uint8_t *p; size_t n, pos; uint8_t *rgb; int w; } jsrc;

static size_t j_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jsrc *s = jd->device;
    if (n > s->n - s->pos) n = s->n - s->pos;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

static int j_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jsrc *s = jd->device;
    const uint8_t *b = bitmap;
    int w = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++)
        memcpy(s->rgb + ((size_t)y * (size_t)s->w + r->left) * 3, b + (size_t)(y - r->top) * (size_t)w * 3, (size_t)w * 3);
    return 1;
}

// a picture's pixels (JPEG: TJpgDec, made small while decoding; else Windows's decoder)
static uint8_t *decode(const uint8_t *data, size_t len, int *w, int *h)
{
    static uint8_t work[TJPGD_WORKSPACE_SIZE + 64];
    jsrc s = { data, len, 0, NULL, 0 };
    JDEC jd;
    if (jd_prepare(&jd, j_in, work, sizeof work, &s) == JDR_OK)
    {
        uint8_t sc = 0;
        unsigned m = jd.width < jd.height ? jd.width : jd.height;
        while (sc < 3 && (m >> (sc + 1)) >= COVER_PX) sc++;
        s.w = (int)((jd.width + (1u << sc) - 1) >> sc);
        int hh = (int)((jd.height + (1u << sc) - 1) >> sc);
        s.rgb = calloc((size_t)s.w * (size_t)hh, 3);
        if (s.rgb && jd_decomp(&jd, j_out, sc) == JDR_OK) { *w = s.w; *h = hh; return s.rgb; }
        free(s.rgb);
    }
    return plat_decode_rgb(data, len, 4 * COVER_PX, 4 * COVER_PX, w, h);
}

// 80 x 80: the picture made to fit (box filter), on the player's navy
static uint8_t *cover_for_msx(const uint8_t *data, size_t len, size_t *out_len)
{
    int w = 0, h = 0;
    uint8_t *px = decode(data, len, &w, &h);
    if (!px) return NULL;
    uint8_t *c = malloc(COVER_PX * COVER_PX * 3);
    if (!c) { free(px); return NULL; }
    uint32_t navy = gfx_pal_base[NAVY];
    for (int i = 0; i < COVER_PX * COVER_PX; i++) { c[3 * i] = (uint8_t)(navy >> 16); c[3 * i + 1] = (uint8_t)(navy >> 8); c[3 * i + 2] = (uint8_t)navy; }
    double f = (double)COVER_PX / (w > h ? w : h);
    int dw = (int)(w * f + 0.5), dh = (int)(h * f + 0.5), ox = (COVER_PX - dw) / 2, oy = (COVER_PX - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
        {
            int x0 = (int)(x / f), x1 = (int)((x + 1) / f), y0 = (int)(y / f), y1 = (int)((y + 1) / f);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            unsigned r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++)
                {
                    const uint8_t *p = px + ((size_t)yy * (size_t)w + (size_t)xx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; k++;
                }
            uint8_t *d = c + ((size_t)(oy + y) * COVER_PX + (size_t)(ox + x)) * 3;
            if (k) { d[0] = (uint8_t)(r / k); d[1] = (uint8_t)(g / k); d[2] = (uint8_t)(b / k); }
        }
    free(px);
    uint8_t *j = jpeg_encode(c, COVER_PX, COVER_PX, COVER_PX * 3, 75, out_len);
    free(c);
    return j;
}

void msxlink_tick(void)
{
    if (!running) return;
    if (strcmp(err, err_shown)) { u_copy(err_shown, sizeof err_shown, err); if (err[0]) ui_note(err, true); }
    if (listeners != listeners_seen)
    {
        if (listeners_seen >= 0 && listeners > listeners_seen) ui_note("The MSX listens", false);
        else if (listeners_seen > 0 && !listeners) ui_note("The MSX stopped listening", false);
        listeners_seen = listeners;
    }
    if (!clients && !listeners) return;
    ui_cover_follow();                      // (the cover of what plays, also when the page does not show it)
    unsigned g = cover_gen();
    if (g == cover_seen || cover_state() == COVER_MAKING) return;
    cover_seen = g;
    const uint8_t *d;
    size_t n, jl = 0;
    uint8_t *j = cover_source(&d, &n) ? cover_for_msx(d, n, &jl) : NULL;
    sys_lock(lock);
    free(cover_jpg);
    cover_jpg = j;
    cover_len = j ? jl : 0;
    cover_n++;
    sys_unlock(lock);
}

// ---------------- the connections ----------------
static bool send_all(sock_t s, const void *b, size_t n)
{
    const char *p = b;
    while (n)
    {
        int k = (int)send(s, p, (int)(n > 65536 ? 65536 : n), SEND_FLAGS);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool send_text(sock_t s, const char *status, const char *type, const char *body, size_t len, const char *extra)
{
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-cache\r\n"
                                  "Access-Control-Allow-Origin: *\r\nConnection: close\r\n%s\r\n",
                     status, type, (unsigned)len, extra ? extra : "");
    return send_all(s, h, (size_t)n) && send_all(s, body, len);
}

// "Artist - Title" (what the MSX shows), else the station or the program
static void now_title(char *t, size_t n)
{
    audio_status st;
    audio_status_get(&st);
    if (st.state == ST_STOPPED && !st.title[0]) { snprintf(t, n, "PicoAmp"); return; }
    if (st.artist[0] && st.title[0]) snprintf(t, n, "%s - %s", st.artist, st.title);
    else snprintf(t, n, "%s", st.title[0] ? st.title : st.stream ? st.album : "PicoAmp");
}

static void stream(sock_t s, bool meta)
{
    char h[512];
    char host[64];
    if (gethostname(host, sizeof host)) u_copy(host, sizeof host, "the PC");
    host[sizeof host - 1] = 0;
    int n = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: PicoAmp on %s\r\nicy-br: %d\r\n"
                                  "%s%s%sX-PicoAmp: 1\r\nCache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n",
                     host, kbps, meta ? "icy-metaint: " : "", meta ? "8192" : "", meta ? "\r\n" : "");
    if (!send_all(s, h, (size_t)n)) return;
    sys_lock(lock);
    listeners++;
    sys_unlock(lock);
    uint64_t join = (uint64_t)kbps * 1000 / 8 * JOIN_MS / 1000;
    sys_lock(mp3_lock);
    uint64_t pos = mp3_total > join ? mp3_total - join : 0;
    sys_unlock(mp3_lock);
    uint8_t buf[4096];
    int to_meta = METAINT;
    char last[600] = "";
    while (running)
    {
        size_t k = 0;
        sys_lock(mp3_lock);
        if (mp3_total - pos > MP3_RING - 65536) pos = mp3_total - join;     // (behind: on to the newest)
        while (k < sizeof buf && pos < mp3_total && (!meta || (int)k < to_meta)) buf[k++] = mp3[pos++ % MP3_RING];
        sys_unlock(mp3_lock);
        if (!k) { sys_sleep_ms(10); continue; }
        if (!send_all(s, buf, k)) break;
        if (!meta) continue;
        to_meta -= (int)k;
        if (to_meta) continue;
        to_meta = METAINT;
        // the ICY block: the title when it changed (else an empty block)
        char t[600], m[16 * 255 + 1];
        now_title(t, sizeof t);
        int len = 0;
        if (strcmp(t, last))
        {
            u_copy(last, sizeof last, t);
            for (char *p = t; *p; p++) if (*p == '\'') *p = '`';    // (a quote would end it early)
            len = snprintf(m + 1, sizeof m - 1, "StreamTitle='%s';", t);
            if (len > 16 * 255 - 1) len = 16 * 255 - 1;
        }
        int blocks = (len + 15) / 16;
        memset(m + 1 + len, 0, (size_t)(blocks * 16 - len));
        m[0] = (char)blocks;
        if (!send_all(s, m, (size_t)(1 + blocks * 16))) break;
    }
    sys_lock(lock);
    listeners--;
    sys_unlock(lock);
}

static void json_str(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (k + 1 < n) o[k++] = '"';
    for (; *s && k + 7 < n; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o[k++] = '\\'; o[k++] = (char)c; }
        else if (c < 32) k += (size_t)snprintf(o + k, n - k, "\\u%04x", c);
        else o[k++] = (char)c;
    }
    if (k + 1 < n) o[k++] = '"';
    o[k] = 0;
}

// the value of a query's key ("" none)
static void query(const char *path, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *q = strchr(path, '?');
    size_t kl = strlen(key);
    for (q = q ? q + 1 : NULL; q && *q; )
    {
        if (!strncmp(q, key, kl) && q[kl] == '=')
        {
            size_t i = 0;
            for (q += kl + 1; *q && *q != '&' && i + 1 < n; q++) out[i++] = *q;
            out[i] = 0;
            return;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
}

static int client_main(void *arg)
{
    sock_t s = (sock_t)(intptr_t)arg;
    char req[4096];
    size_t got = 0;
    while (got + 1 < sizeof req)
    {
        int k = (int)recv(s, req + got, (int)(sizeof req - 1 - got), 0);
        if (k <= 0) break;
        got += (size_t)k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = 0;
    char path[1024] = "";
    if (!strncmp(req, "GET ", 4))
    {
        const char *e = strchr(req + 4, ' ');
        size_t l = e ? (size_t)(e - req - 4) : 0;
        if (l >= sizeof path) l = sizeof path - 1;
        memcpy(path, req + 4, l);
        path[l] = 0;
    }
    char lower[4096];
    for (size_t i = 0; i <= got; i++) lower[i] = (char)(req[i] >= 'A' && req[i] <= 'Z' ? req[i] + 32 : req[i]);
    bool meta = strstr(lower, "\nicy-metadata: 1") || strstr(lower, "\nicy-metadata:1");
    char p0[1024];
    u_copy(p0, sizeof p0, path);
    char *qm = strchr(p0, '?');
    if (qm) *qm = 0;

    if (!path[0]) send_text(s, "400 Bad Request", "text/plain", "GET only\n", 9, NULL);
    else if (!strcmp(p0, "/") || !strcmp(p0, "/stream.mp3") || !strcmp(p0, "/stream") || !strcmp(p0, "/;")) stream(s, meta);
    else if (!strcmp(p0, "/cmd"))
    {
        char k[16], v[16];
        query(path, "k", k, sizeof k);
        audio_status st;
        audio_status_get(&st);
        bool ok = true;
        if (!strcmp(k, "toggle")) audio_toggle_pause();
        else if (!strcmp(k, "play")) { if (st.state != ST_PLAYING) audio_toggle_pause(); }
        else if (!strcmp(k, "pause")) { if (st.state == ST_PLAYING) audio_toggle_pause(); }
        else if (!strcmp(k, "next")) audio_next();
        else if (!strcmp(k, "prev")) audio_prev();
        else if (!strcmp(k, "seek"))
        {
            query(path, "s", v, sizeof v);
            if (v[0]) audio_seek_rel(atoi(v));
            else { query(path, "to", v, sizeof v); if (v[0]) audio_seek_to(atoi(v)); else ok = false; }
        }
        else ok = false;
        if (ok) send_text(s, "200 OK", "text/plain", "ok\n", 3, NULL);
        else send_text(s, "400 Bad Request", "text/plain", "k: toggle play pause next prev seek\n", 36, NULL);
    }
    else if (!strcmp(p0, "/now"))
    {
        audio_status st;
        audio_status_get(&st);
        char t[600], a[600], al[600], src[160], b[2400];
        json_str(t, sizeof t, st.title);
        json_str(a, sizeof a, st.artist);
        json_str(al, sizeof al, st.album);
        json_str(src, sizeof src, st.system ? st.source : st.stream ? "radio" : "picoamp");
        sys_lock(lock);
        unsigned cg = cover_n;
        bool hc = cover_jpg != NULL;
        sys_unlock(lock);
        char e[400];
        json_str(e, sizeof e, err);
        int n = snprintf(b, sizeof b, "{\"state\":\"%s\",\"title\":%s,\"artist\":%s,\"album\":%s,\"source\":%s,"
                                      "\"elapsed\":%d,\"total\":%d,\"cover\":%u,\"has_cover\":%s,\"listeners\":%d,\"error\":%s}\n",
                         st.state == ST_PLAYING ? "playing" : st.state == ST_PAUSED ? "paused" : "stopped",
                         t, a, al, src, st.elapsed, st.total, cg, hc ? "true" : "false", listeners, e);
        send_text(s, "200 OK", "application/json", b, (size_t)n, NULL);
    }
    else if (!strcmp(p0, "/cover.jpg"))
    {
        sys_lock(lock);
        uint8_t *c = cover_len ? malloc(cover_len) : NULL;
        size_t n = c ? cover_len : 0;
        unsigned cg = cover_n;
        if (c) memcpy(c, cover_jpg, n);
        sys_unlock(lock);
        char x[48];
        snprintf(x, sizeof x, "X-Cover-Gen: %u\r\n", cg);
        if (c) send_text(s, "200 OK", "image/jpeg", (const char *)c, n, x);
        else send_text(s, "404 Not Found", "text/plain", "no cover\n", 9, x);
        free(c);
    }
    else if (!strcmp(p0, "/picoamp.m3u"))
    {
        char b[256];
        int n = snprintf(b, sizeof b, "#EXTM3U\r\n#EXTINF:-1,PicoAmp on the PC\r\n%s\r\n", url);
        send_text(s, "200 OK", "audio/x-mpegurl", b, (size_t)n, NULL);
    }
    else send_text(s, "404 Not Found", "text/plain", "not here\n", 9, NULL);
    sock_close(s);
    sys_lock(lock);
    clients--;
    sys_unlock(lock);
    return 0;
}

static int accept_main(void *arg)
{
    (void)arg;
    while (running)
    {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(lsock, &fs);
        struct timeval tv = { 0, 200000 };
        if (select((int)lsock + 1, &fs, NULL, NULL, &tv) <= 0) continue;
        sock_t c = accept(lsock, NULL, NULL);
        if (c == BAD_SOCK) continue;
        if (clients >= MAX_CLIENTS + 2) { sock_close(c); continue; }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef _WIN32
        DWORD to = 10000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#else
        struct timeval to = { 10, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
#endif
        sys_lock(lock);
        clients++;
        sys_unlock(lock);
        sys_thread *t = sys_thread_start(client_main, (void *)(intptr_t)c);
        if (t) sys_thread_detach(t);
        else { sock_close(c); sys_lock(lock); clients--; sys_unlock(lock); }
    }
    return 0;
}

// this PC's address on the network (the one a packet out would come from)
static void local_ip(char *out, size_t n)
{
    u_copy(out, n, "127.0.0.1");
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCK) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = inet_addr("192.0.2.1");      // (no packet is sent: it only picks the way)
    if (!connect(s, (struct sockaddr *)&a, sizeof a))
    {
        struct sockaddr_in me;
        socklen_t l = sizeof me;
        if (!getsockname(s, (struct sockaddr *)&me, &l) && me.sin_addr.s_addr) u_copy(out, n, inet_ntoa(me.sin_addr));
    }
    sock_close(s);
}

bool msxlink_start(int p, int k)
{
    if (running) return true;
    err[0] = 0;
    if (!lock) { lock = sys_mutex_new(); pcm_lock = sys_mutex_new(); mp3_lock = sys_mutex_new(); }
    if (!pcm) pcm = malloc(sizeof(int16_t) * 2 * PCM_FRAMES);
    if (!mp3) mp3 = malloc(MP3_RING);
    if (!pcm || !mp3) { snprintf(err, sizeof err, "MSX link: no memory"); return false; }
    port = p;
    kbps = k;
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == BAD_SOCK) { snprintf(err, sizeof err, "MSX link: no socket"); return false; }
#ifndef _WIN32
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lsock, (struct sockaddr *)&a, sizeof a) || listen(lsock, 8))
    {
        snprintf(err, sizeof err, "MSX link: port %d is in use", port);
        sock_close(lsock);
        lsock = BAD_SOCK;
        return false;
    }
    char ip[64];
    local_ip(ip, sizeof ip);
    snprintf(url, sizeof url, "http://%s:%d/stream.mp3", ip, port);
    sys_lock(mp3_lock);
    mp3_total = 0;
    sys_unlock(mp3_lock);
    listeners_seen = -1;
    cover_seen = ~0u;
    running = true;
    accept_th = sys_thread_start(accept_main, NULL);
    enc_th = sys_thread_start(enc_main, NULL);
    audio_set_tap(msxlink_feed);
    return true;
}

void msxlink_stop(void)
{
    if (!running) return;
    audio_set_tap(NULL);
    running = false;
    sys_thread_join(accept_th);
    sys_thread_join(enc_th);
    sock_close(lsock);
    lsock = BAD_SOCK;
    // (the listeners' threads end by themselves: they see running false)
    for (int i = 0; i < 100 && clients; i++) sys_sleep_ms(20);
}
