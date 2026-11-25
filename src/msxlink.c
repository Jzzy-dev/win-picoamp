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
#include "lyrics.h"
#include "sonos.h"
#include "net.h"
#include "config.h"

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
static volatile int clients, listeners, screens;
static int          listeners_seen = -1, screens_seen = -1;
static viz_data     snap;                   // the bars as the screen shows them (the screen's thread copies them)
static char        *ly_block;               // the words for the MSX's screen: "L<state>\n" rows "L.\n" (lock)
static unsigned     ly_gen;
static uint32_t     ly_at;
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
static uint8_t     *cover_jpg, *cover_big;     // 80 x 80 on navy; the full screen's (fits 281 x 192, as it is)
static size_t       cover_len, cover_big_len;
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
static uint8_t *decode(const uint8_t *data, size_t len, int *w, int *h, int min_px)
{
    static uint8_t work[TJPGD_WORKSPACE_SIZE + 64];
    jsrc s = { data, len, 0, NULL, 0 };
    JDEC jd;
    if (jd_prepare(&jd, j_in, work, sizeof work, &s) == JDR_OK)
    {
        uint8_t sc = 0;
        unsigned m = jd.width < jd.height ? jd.width : jd.height;
        while (sc < 3 && (m >> (sc + 1)) >= (unsigned)min_px) sc++;
        s.w = (int)((jd.width + (1u << sc) - 1) >> sc);
        int hh = (int)((jd.height + (1u << sc) - 1) >> sc);
        s.rgb = calloc((size_t)s.w * (size_t)hh, 3);
        if (s.rgb && jd_decomp(&jd, j_out, sc) == JDR_OK) { *w = s.w; *h = hh; return s.rgb; }
        free(s.rgb);
    }
    return plat_decode_rgb(data, len, 2 * min_px > 320 ? 2 * min_px : 320, 2 * min_px > 320 ? 2 * min_px : 320, w, h);
}

// the picture made to fit mw x mh (box filter): pad: on the player's navy, mw x mh (the cover view's);
// else as big as fits (the full screen's, as the engine's image proxy makes it: fit=inside)
static uint8_t *cover_for_msx(const uint8_t *data, size_t len, int mw, int mh, bool pad, size_t *out_len)
{
    int w = 0, h = 0;
    uint8_t *px = decode(data, len, &w, &h, mh);
    if (!px) return NULL;
    double f = (double)mw / w;
    if ((double)mh / h < f) f = (double)mh / h;
    int dw = (int)(w * f + 0.5), dh = (int)(h * f + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int cw = pad ? mw : dw, ch = pad ? mh : dh, ox = (cw - dw) / 2, oy = (ch - dh) / 2;
    uint8_t *c = malloc((size_t)cw * (size_t)ch * 3);
    if (!c) { free(px); return NULL; }
    uint32_t navy = gfx_pal_base[NAVY];
    for (int i = 0; i < cw * ch; i++) { c[3 * i] = (uint8_t)(navy >> 16); c[3 * i + 1] = (uint8_t)(navy >> 8); c[3 * i + 2] = (uint8_t)navy; }
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
            uint8_t *d = c + ((size_t)(oy + y) * (size_t)cw + (size_t)(ox + x)) * 3;
            if (k) { d[0] = (uint8_t)(r / k); d[1] = (uint8_t)(g / k); d[2] = (uint8_t)(b / k); }
        }
    free(px);
    uint8_t *j = jpeg_encode(c, cw, ch, cw * 3, 75, out_len);
    free(c);
    return j;
}

static char *lyrics_block(void);

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
    if (screens != screens_seen)
    {
        if (screens_seen >= 0 && screens > screens_seen) ui_note("The MSX shows what plays", false);
        screens_seen = screens;
    }
    if (!clients && !listeners) return;
    sys_lock(lock);
    snap = ui_viz;
    sys_unlock(lock);
    if (screens && sys_millis() - ly_at >= 250)
    {
        // the words of what plays (looked for as the lyrics page does), for the MSX's screen
        ly_at = sys_millis();
        audio_status st;
        audio_status_get(&st);
        if (st.state == ST_STOPPED && !st.title[0]) lyrics_want("", "", "", 0);
        else lyrics_want(st.artist, st.title, st.stream ? "" : st.album, st.stream ? 0 : st.total);
        char *b = lyrics_block();
        sys_lock(lock);
        if (b && (!ly_block || strcmp(b, ly_block))) { free(ly_block); ly_block = b; b = NULL; ly_gen++; }
        sys_unlock(lock);
        free(b);
    }
    ui_cover_follow();                      // (the cover of what plays, also when the page does not show it)
    unsigned g = cover_gen();
    if (g == cover_seen || cover_state() == COVER_MAKING) return;
    cover_seen = g;
    const uint8_t *d;
    size_t n, jl = 0, bl = 0;
    bool src = cover_source(&d, &n);
    uint8_t *j = src ? cover_for_msx(d, n, COVER_PX, COVER_PX, true, &jl) : NULL;
    uint8_t *b = src ? cover_for_msx(d, n, 281, 192, false, &bl) : NULL;
    sys_lock(lock);
    free(cover_jpg);
    free(cover_big);
    cover_jpg = j;
    cover_len = j ? jl : 0;
    cover_big = b;
    cover_big_len = b ? bl : 0;
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

// a text in the MSX's character set, without tabs and line ends
static int msx_text(char *o, int n, const char *utf8)
{
    uint8_t b[256];
    int k = gfx_to_msx(utf8, b, sizeof b), m = 0;
    for (int i = 0; i < k && m + 1 < n; i++) o[m++] = (char)(b[i] == '\t' || b[i] == '\n' || b[i] == '\r' ? ' ' : b[i]);
    o[m] = 0;
    return m;
}

// the MSX's screen (X-PicoAmp: 2): no sound, what the PC's player shows, as lines
//   V<24 levels, '0' + 0..31><VU left, right><64 scope samples, ' ' + (s + 128) / 3>   every 40 ms
//   T <elapsed ms> <length s> <p playing | a paused | s stopped> <volume> <muted 0/1> every 500 ms and when it changes
//   C<n>   the cover of what plays is made (n: +1 each time): fetch /cover.jpg; F<text> the format line (21 chars)
//   S<artist> TAB <title> TAB <album>                                               when the song changes
//   L<s|p|i|n|l|->, R<ms, -1 not synced> TAB <a row of words, 40 characters>..., L.  the words, when they change
static void screen(sock_t s)
{
    char host[64], h[300];
    if (gethostname(host, sizeof host)) u_copy(host, sizeof host, "the PC");
    host[sizeof host - 1] = 0;
    int n = snprintf(h, sizeof h, "HTTP/1.0 200 OK\r\nContent-Type: text/x-picoamp-screen\r\nicy-name: PicoAmp on %s\r\n"
                                  "X-PicoAmp: 2\r\nCache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n", host);
    if (!send_all(s, h, (size_t)n)) return;
    sys_lock(lock);
    screens++;
    sys_unlock(lock);
    char song[700] = "\x01", line[800];
    int last_state = -1, last_vol = -1;
    unsigned ly_sent = ly_gen - 1;
    uint32_t last_t = 0;
    unsigned cover_sent = 0;
    bool cover_any = false;
    char fmt_sent[40] = "\x01";
    while (running)
    {
        audio_status st;
        audio_status_get(&st);
        uint32_t now = sys_millis();
        // its cover is made (a new one, or none): the MSX fetches /cover.jpg then (not at the song: it may be the last one still)
        sys_lock(lock);
        unsigned cg = cover_n;
        sys_unlock(lock);
        if (!cover_any || cg != cover_sent)
        {
            n = snprintf(line, sizeof line, "C%u\n", cg);
            if (!send_all(s, line, (size_t)n)) break;
            cover_sent = cg;
            cover_any = true;
        }
        // the format line, as this PC shows it
        {
            char f[64], m[64];
            ui_format_of(&st, f, sizeof f);
            msx_text(m, sizeof m, f);
            m[21] = 0;
            if (strcmp(m, fmt_sent))
            {
                u_copy(fmt_sent, sizeof fmt_sent, m);
                n = snprintf(line, sizeof line, "F%s\n", m);
                if (!send_all(s, line, (size_t)n)) break;
            }
        }
        // the song
        char a[200], t[200], al[200];
        msx_text(a, sizeof a, st.artist);
        msx_text(t, sizeof t, st.title[0] ? st.title : st.state == ST_STOPPED ? "" : st.album);
        msx_text(al, sizeof al, st.stream ? "" : st.album);
        n = snprintf(line, sizeof line, "S%.39s\t%.47s\t%.39s\n", a, t, al);
        if (strcmp(line, song))
        {
            u_copy(song, sizeof song, line);
            if (!send_all(s, line, (size_t)n)) break;
        }
        // its time and state
        int ps = st.state == ST_PLAYING ? 'p' : st.state == ST_PAUSED ? 'a' : 's';
        int vm = st.volume * 2 + (st.muted ? 1 : 0);
        if (ps != last_state || vm != last_vol || now - last_t >= 500)
        {
            n = snprintf(line, sizeof line, "T %d %d %c %d %d\n", st.elapsed_ms, st.total, ps, st.volume, st.muted ? 1 : 0);
            if (!send_all(s, line, (size_t)n)) break;
            last_state = ps;
            last_vol = vm;
            last_t = now;
        }
        // the words, when they change
        if (ly_sent != ly_gen)
        {
            sys_lock(lock);
            char *b = ly_block ? strdup(ly_block) : NULL;
            unsigned g = ly_gen;
            sys_unlock(lock);
            if (b)
            {
                bool ok = send_all(s, b, strlen(b));
                free(b);
                if (!ok) break;
                ly_sent = g;
            }
        }
        // the bars
        viz_data v;
        sys_lock(lock);
        v = snap;
        sys_unlock(lock);
        int k = 0;
        line[k++] = 'V';
        for (int b = 0; b < 24; b++) line[k++] = (char)('0' + (b < VIZ_BANDS ? (v.level[b] > 31 ? 31 : v.level[b]) : 0));
        line[k++] = (char)('0' + (v.vu[0] > 31 ? 31 : v.vu[0]));
        line[k++] = (char)('0' + (v.vu[1] > 31 ? 31 : v.vu[1]));
        for (int i = 0; i < 64; i++) line[k++] = (char)(' ' + ((i < VIZ_WAVE ? v.scope[i] : 0) + 128) / 3);
        line[k++] = '\n';
        if (!send_all(s, line, (size_t)k)) break;
        sys_sleep_ms(40);
    }
    sys_lock(lock);
    screens--;
    sys_unlock(lock);
}

// the words as the MSX shows them: rows of 40 characters at most (cut at a space), each with its line's time
static char *lyrics_block(void)
{
    int ls = lyrics_state();
    char c = ls == LY_SYNCED ? 's' : ls == LY_PLAIN ? 'p' : ls == LY_INSTRUMENTAL ? 'i' : ls == LY_NOT_FOUND || ls == LY_FAILED ? 'n'
           : ls == LY_LOADING ? 'l' : '-';
    size_t cap = 256, n = 0;
    char *b = malloc(cap);
    if (!b) return NULL;
    n += (size_t)snprintf(b, cap, "L%c\n", c);
    int lines = (c == 's' || c == 'p') ? lyrics_count() : 0;
    for (int i = 0; i < lines; i++)
    {
        int ms;
        const char *t = lyrics_line(i, &ms);
        char m[400];
        int len = msx_text(m, sizeof m, t), off = 0;
        do
        {
            int take = len - off;
            if (take > 40)
            {
                take = 40;
                int sp = take;
                while (sp > 0 && m[off + sp] != ' ') sp--;
                if (sp > 0) take = sp;
            }
            if (n + (size_t)take + 32 > cap)
            {
                cap = cap * 2 + (size_t)take + 64;
                char *nb = realloc(b, cap);
                if (!nb) { free(b); return NULL; }
                b = nb;
            }
            n += (size_t)snprintf(b + n, cap - n, "R%d\t%.*s\n", c == 's' ? ms : -1, take, m + off);
            off += take;
            while (off < len && m[off] == ' ') off++;
        } while (off < len);
    }
    if (n + 4 > cap) { char *nb = realloc(b, n + 4); if (!nb) { free(b); return NULL; } b = nb; }
    memcpy(b + n, "L.\n", 4);
    return b;
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

// ---------------------------------------------------------------- PicoSing (karaoke on the MSX)
// PicoSing on the MSX sings with what this PC plays: it asks for the song
// here, follows the player's time on /screen and sends pause/seek/volume
// on /cmd as PicoAmp on the MSX does.
//   /sing/open?yt=<YouTube id>     opens the video in the browser and follows it
//   /sing/open?q=<artist title>    a Spotify search (the app, else the web player)
// And when its WiFi cannot reach USDB (usdb.animux.de: the songs' notes and
// words), this PC fetches the pages for it, logged in with the MSX's name
// and password, or with [usdb] user/pass here; the cookies stay here:
//   /usdb/login?u=&p=   "ok", "bad" (wrong name or password), or why not
//   /usdb/list?interpret=&title=&start=&limit=   the search's page
//   /usdb/txt?id=       the page with the song's .txt (in its <textarea>)
//   /usdb/cover?id=     its cover (JPEG)
#define USDB_SITE "https://usdb.animux.de/"
static char usdb_jar[1024], usdb_u[128], usdb_p[128];
static sys_mutex *usdb_lock;

static bool ok_id(const char *id)
{
    if (strlen(id) != 11) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

static void sing_open(sock_t s, const char *path)
{
    char yt[32], q[300], url[600];
    query(path, "yt", yt, sizeof yt);
    query(path, "q", q, sizeof q);
    bool ok = false;
    if (yt[0])
    {
        if (!ok_id(yt)) { send_text(s, "400 Bad Request", "text/plain", "not a video id\n", 15, NULL); return; }
        snprintf(url, sizeof url, "https://www.youtube.com/watch?v=%s", yt);
        ok = plat_open_url(url);
    }
    else if (q[0])
    {
        // only what a search needs: letters, digits, %xx, + (no other schemes, no paths)
        for (char *p = q; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '%' || *p == '+' || *p == '-' || *p == '.' || *p == '_'))
                *p = '+';
        for (char *p = q; *p; p++) if (*p == '+') *p = ' ';
        char e[600] = "";
        for (char *p = q; *p; p++) { if (*p == ' ') strcat(e, "%20"); else strncat(e, p, 1); if (strlen(e) > 560) break; }
        snprintf(url, sizeof url, "spotify:search:%s", e);
        ok = plat_open_url(url);
        if (!ok) { snprintf(url, sizeof url, "https://open.spotify.com/search/%s", e); ok = plat_open_url(url); }
    }
    else { send_text(s, "400 Bad Request", "text/plain", "yt= or q=\n", 10, NULL); return; }
    if (!ok) { send_text(s, "502 Bad Gateway", "text/plain", "could not open it\n", 18, NULL); return; }
    audio_follow_system();
    send_text(s, "200 OK", "text/plain", "ok\n", 3, NULL);
}

// a USDB page, with the jar's cookies (NULL: no answer); *len its size
static char *usdb_page(const char *method, const char *rel, const char *form, size_t *len, int *status)
{
    const char *site = getenv("PICOAMP_USDB");
    char url[600], e[200];
    snprintf(url, sizeof url, "%s%s", site && *site ? site : USDB_SITE, rel);
    sys_lock(usdb_lock);
    char jar[sizeof usdb_jar];
    u_copy(jar, sizeof jar, usdb_jar);
    sys_unlock(usdb_lock);
    net_conn *c = net_request(method, url, NULL, form, form ? strlen(form) : 0, jar, sizeof jar, status, e, sizeof e, NULL);
    sys_lock(usdb_lock);
    u_copy(usdb_jar, sizeof usdb_jar, jar);
    sys_unlock(usdb_lock);
    if (!c) return NULL;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap + 1);
    for (;;)
    {
        if (n == cap) { if (cap >= (4u << 20)) break; cap *= 2; b = realloc(b, cap + 1); }
        long r = net_read(c, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    net_close(c);
    b[n] = 0;
    *len = n;
    return b;
}

// log in (the MSX's name, else this PC's): 1 in, 0 wrong name or password, -1 no answer
static int usdb_login(void)
{
    char u[128], p[128], form[400];
    sys_lock(usdb_lock);
    u_copy(u, sizeof u, usdb_u[0] ? usdb_u : cfg.usdb_user);
    u_copy(p, sizeof p, usdb_u[0] ? usdb_p : cfg.usdb_pass);
    sys_unlock(usdb_lock);
    if (!u[0]) return 0;
    // (the MSX sends them escaped; the PC's own are escaped here)
    char ue[400], pe[400];
    if (usdb_u[0]) { u_copy(ue, sizeof ue, u); u_copy(pe, sizeof pe, p); }
    else
    {
        static const char hex[] = "0123456789ABCDEF";
        char *src[2] = { u, p }, *dst[2] = { ue, pe };
        for (int k = 0; k < 2; k++)
        {
            size_t o = 0;
            for (unsigned char *q = (unsigned char *)src[k]; *q && o + 4 < sizeof ue; q++)
            {
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '-' || *q == '_' || *q == '.') dst[k][o++] = (char)*q;
                else { dst[k][o++] = '%'; dst[k][o++] = hex[*q >> 4]; dst[k][o++] = hex[*q & 15]; }
            }
            dst[k][o] = 0;
        }
    }
    snprintf(form, sizeof form, "user=%s&pass=%s&login=Login", ue, pe);
    size_t n;
    int st;
    char *b = usdb_page("POST", "index.php?link=login", form, &n, &st);
    if (!b) return -1;
    int r = strstr(b, "Password invalid") ? 0 : 1;
    free(b);
    return r;
}

static void usdb_bridge(sock_t s, const char *what, const char *path)
{
    if (!usdb_lock) usdb_lock = sys_mutex_new();
    if (!strcmp(what, "login"))
    {
        char u[128], p[128];
        query(path, "u", u, sizeof u);
        query(path, "p", p, sizeof p);
        sys_lock(usdb_lock);
        u_copy(usdb_u, sizeof usdb_u, u);
        u_copy(usdb_p, sizeof usdb_p, p);
        usdb_jar[0] = 0;
        sys_unlock(usdb_lock);
        int r = usdb_login();
        const char *t = r > 0 ? "ok\n" : r == 0 ? "bad\n" : "no answer from usdb.animux.de\n";
        send_text(s, "200 OK", "text/plain", t, strlen(t), NULL);
        return;
    }
    char rel[300], form[900] = "", v[300], id[16];
    const char *method = "POST";
    query(path, "id", id, sizeof id);
    for (char *p = id; *p; p++) if (*p < '0' || *p > '9') *p = 0;
    if (!strcmp(what, "list"))
    {
        // the MSX's query (escaped already) as USDB's search form
        static const char *keys[] = { "interpret", "title", "edition", "language", "genre", "start", "limit" };
        u_copy(rel, sizeof rel, "index.php?link=list");
        snprintf(form, sizeof form, "order=rating&ud=desc&details=1");
        for (int i = 0; i < 7; i++)
        {
            query(path, keys[i], v, sizeof v);
            size_t l = strlen(form);
            snprintf(form + l, sizeof form - l, "&%s=%s", keys[i], v);
        }
    }
    else if (!strcmp(what, "txt") && id[0]) { snprintf(rel, sizeof rel, "index.php?link=gettxt&id=%s", id); u_copy(form, sizeof form, "wd=1"); }
    else if (!strcmp(what, "cover") && id[0]) { snprintf(rel, sizeof rel, "data/cover/%s.jpg", id); method = "GET"; }
    else { send_text(s, "404 Not Found", "text/plain", "login list txt cover\n", 21, NULL); return; }
    size_t n = 0;
    int st = 0;
    char *b = NULL;
    for (int round = 0; round < 2; round++)
    {
        b = usdb_page(method, rel, method[0] == 'P' ? form : NULL, &n, &st);
        // a session that ran out: in again, once
        if (b && strstr(b, "You are not logged in") && round == 0 && usdb_login() > 0) { free(b); b = NULL; continue; }
        break;
    }
    if (!b)
    {
        char m[80];
        int k = snprintf(m, sizeof m, "usdb.animux.de: %d\n", st);
        send_text(s, st == 404 ? "404 Not Found" : "502 Bad Gateway", "text/plain", m, (size_t)k, NULL);
        return;
    }
    send_text(s, "200 OK", what[0] == 'c' ? "image/jpeg" : "text/html", b, n, NULL);
    free(b);
}

// ---------------------------------------------------------------- the Sonos bridge
// PicoAmp for the web (a page cannot reach the speakers itself) asks here:
//   /sonos/rooms[?fresh=1]          {"state":"done","error":"","rooms":[{"id","name","members"}]}
//   /sonos/now?id=RINCON_..         what the room plays (as nowplaying.h's np_info)
//   /sonos/cmd?id=..&c=toggle|play|pause|next|prev|seek&ms=..|shuffle&on=0/1|repeat&on=0/1/2
//   /sonos/vol?id=..&v=0..100&m=0/1 the room's volume and mute (either)
//   /sonos/art?id=..                its cover (the picture's bytes)
static void sonos_bridge(sock_t s, const char *what, const char *path)
{
    char id[64], v[32], m[16];
    query(path, "id", id, sizeof id);
    if (!strcmp(what, "rooms"))
    {
        query(path, "fresh", v, sizeof v);
        sonos_discover(atoi(v) != 0);
        for (int i = 0; i < 40 && sonos_state() == SONOS_LOADING; i++) sys_sleep_ms(50);
        sonos_room r[32];
        int n = sonos_rooms(r, 32), st = sonos_state();
        size_t cap = 256 + (size_t)n * 400, k = 0;
        char *b = malloc(cap), e[300];
        json_str(e, sizeof e, sonos_error());
        k += (size_t)snprintf(b + k, cap - k, "{\"state\":\"%s\",\"error\":%s,\"rooms\":[",
                              st == SONOS_LOADING ? "loading" : st == SONOS_FAILED ? "failed" : st == SONOS_DONE ? "done" : "idle", e);
        for (int i = 0; i < n; i++)
        {
            char ji[128], jn[300];
            json_str(ji, sizeof ji, r[i].id);
            json_str(jn, sizeof jn, r[i].name);
            k += (size_t)snprintf(b + k, cap - k, "%s{\"id\":%s,\"name\":%s,\"members\":%d}", i ? "," : "", ji, jn, r[i].members);
        }
        k += (size_t)snprintf(b + k, cap - k, "]}\n");
        send_text(s, "200 OK", "application/json", b, k, NULL);
        free(b);
        return;
    }
    if (!id[0]) { send_text(s, "400 Bad Request", "text/plain", "id?\n", 4, NULL); return; }
    if (!strcmp(what, "now"))
    {
        np_info i;
        if (!sonos_query(id, &i)) { send_text(s, "404 Not Found", "application/json", "{\"error\":\"no answer\"}\n", 22, NULL); return; }
        char t[600], a[600], al[600], ap[200], b[3000];
        json_str(t, sizeof t, i.title);
        json_str(a, sizeof a, i.artist);
        json_str(al, sizeof al, i.album);
        json_str(ap, sizeof ap, i.app);
        int n = snprintf(b, sizeof b,
                         "{\"state\":\"%s\",\"app\":%s,\"title\":%s,\"artist\":%s,\"album\":%s,\"elapsed_ms\":%d,\"total_ms\":%d,"
                         "\"can_seek\":%s,\"can_next\":%s,\"can_prev\":%s,\"can_shuffle\":%s,\"can_repeat\":%s,"
                         "\"shuffle\":%s,\"repeat\":%d,\"volume\":%d,\"muted\":%s,\"art\":%u}\n",
                         i.state == NP_PLAYING ? "playing" : i.state == NP_PAUSED ? "paused" : "stopped", ap, t, a, al,
                         i.elapsed_ms, i.total_ms, i.can_seek ? "true" : "false", i.can_next ? "true" : "false",
                         i.can_prev ? "true" : "false", i.can_shuffle ? "true" : "false", i.can_repeat ? "true" : "false",
                         i.shuffle ? "true" : "false", i.repeat, i.volume, i.muted ? "true" : "false", i.art_gen);
        send_text(s, "200 OK", "application/json", b, (size_t)n, NULL);
        return;
    }
    if (!strcmp(what, "cmd"))
    {
        char c[16];
        query(path, "c", c, sizeof c);
        int cmd = !strcmp(c, "toggle") ? NP_TOGGLE : !strcmp(c, "play") ? NP_PLAY : !strcmp(c, "pause") ? NP_PAUSE
                : !strcmp(c, "next") ? NP_NEXT : !strcmp(c, "prev") ? NP_PREV : !strcmp(c, "seek") ? NP_SEEK_TO
                : !strcmp(c, "shuffle") ? NP_SHUFFLE : !strcmp(c, "repeat") ? NP_REPEAT : 0;
        query(path, cmd == NP_SEEK_TO ? "ms" : "on", v, sizeof v);
        bool ok = cmd && sonos_command(id, cmd, atoi(v));
        if (ok) send_text(s, "200 OK", "text/plain", "ok\n", 3, NULL);
        else send_text(s, "502 Bad Gateway", "text/plain", "the speaker said no\n", 20, NULL);
        return;
    }
    if (!strcmp(what, "vol"))
    {
        query(path, "v", v, sizeof v);
        query(path, "m", m, sizeof m);
        bool ok = (v[0] || m[0]) && sonos_set_volume(id, v[0] ? atoi(v) : -1, m[0] ? atoi(m) != 0 : -1);
        if (ok) send_text(s, "200 OK", "text/plain", "ok\n", 3, NULL);
        else send_text(s, "502 Bad Gateway", "text/plain", "the speaker said no\n", 20, NULL);
        return;
    }
    if (!strcmp(what, "art"))
    {
        uint8_t *d;
        size_t n;
        if (sonos_room_art(id, &d, &n))
        {
            bool png = n > 4 && d[0] == 0x89 && d[1] == 'P';
            send_text(s, "200 OK", png ? "image/png" : "image/jpeg", (const char *)d, n, NULL);
            free(d);
        }
        else send_text(s, "404 Not Found", "text/plain", "no cover\n", 9, NULL);
        return;
    }
    send_text(s, "404 Not Found", "text/plain", "rooms now cmd vol art\n", 22, NULL);
}

// ---------------------------------------------------------------- the pictures and music folders
// PicoAmp on a TV (the Android app) or in a browser shows this PC's pictures (the Pictures page's
// folder: [pictures] folder in picoamp.ini, else the Pictures folder) and plays its music (the
// list's Music folder):
//   /pics/list?path=a/b         the folder's folders and pictures, a line each: "d name" or "f name"
//   /pics/file?path=a/b/c.jpg   a picture's bytes
//   /music/list?path=a/b        the folder's folders, music and pictures (its cover.jpg)
//   /music/file?path=a/b/c.mp3  a song, streamed: Range gives a part (206; seeking, the tags)
// (path: '/' between the folders, %-escaped; nothing above the folder)
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void pct_decode(char *t)
{
    char *o = t;
    for (const char *i = t; *i; i++)
    {
        if (*i == '%' && hexv(i[1]) >= 0 && hexv(i[2]) >= 0) { *o++ = (char)(hexv(i[1]) * 16 + hexv(i[2])); i += 2; }
        else *o++ = *i == '+' ? ' ' : *i;
    }
    *o = 0;
}

static const char *pic_type(const char *name)
{
    const char *e = strrchr(name, '.');
    if (!e) return NULL;
    char x[8] = "";
    for (int i = 0; i < 7 && e[i + 1]; i++) x[i] = (char)(e[i + 1] >= 'A' && e[i + 1] <= 'Z' ? e[i + 1] + 32 : e[i + 1]);
    if (!strcmp(x, "jpg") || !strcmp(x, "jpeg")) return "image/jpeg";
    if (!strcmp(x, "png")) return "image/png";
    if (!strcmp(x, "gif")) return "image/gif";
    if (!strcmp(x, "bmp")) return "image/bmp";
    if (!strcmp(x, "webp")) return "image/webp";
    return NULL;
}

static const char *music_type(const char *name)
{
    const char *e = strrchr(name, '.');
    if (!e) return NULL;
    char x[8] = "";
    for (int i = 0; i < 7 && e[i + 1]; i++) x[i] = (char)(e[i + 1] >= 'A' && e[i + 1] <= 'Z' ? e[i + 1] + 32 : e[i + 1]);
    if (!strcmp(x, "mp3")) return "audio/mpeg";
    if (!strcmp(x, "flac")) return "audio/flac";
    if (!strcmp(x, "m4a") || !strcmp(x, "mp4")) return "audio/mp4";
    if (!strcmp(x, "aac")) return "audio/aac";
    if (!strcmp(x, "ogg") || !strcmp(x, "oga") || !strcmp(x, "opus")) return "audio/ogg";
    if (!strcmp(x, "wav")) return "audio/wav";
    if (!strcmp(x, "webm")) return "audio/webm";
    return NULL;
}

// a song: all of it, or the part Range asks for ("bytes=a-b", "bytes=a-", "bytes=-n"), in pieces
static void send_song(sock_t s, const char *full, const char *type, const char *range)
{
    FILE *f = sys_fopen(full, "rb");
    if (!f) { send_text(s, "404 Not Found", "text/plain", "no such song\n", 13, NULL); return; }
    long long size = -1;
#ifdef _WIN32
    if (!_fseeki64(f, 0, SEEK_END)) size = _ftelli64(f);
#else
    if (!fseeko(f, 0, SEEK_END)) size = (long long)ftello(f);
#endif
    if (size < 0) { fclose(f); send_text(s, "500 Error", "text/plain", "cannot read\n", 12, NULL); return; }
    long long a = 0, b = size - 1;
    bool part = false;
    if (range)
    {
        const char *r = strstr(range, "bytes=");
        if (r)
        {
            r += 6;
            char *end;
            if (*r == '-') { long long n = strtoll(r + 1, &end, 10); a = n < size ? size - n : 0; }
            else
            {
                a = strtoll(r, &end, 10);
                if (*end == '-' && end[1] >= '0' && end[1] <= '9') b = strtoll(end + 1, NULL, 10);
            }
            if (b > size - 1) b = size - 1;
            part = true;
        }
    }
    if (part && (a > b || a >= size))
    {
        char x[96];
        snprintf(x, sizeof x, "Content-Range: bytes */%lld\r\n", size);
        fclose(f);
        send_text(s, "416 Range Not Satisfiable", "text/plain", "", 0, x);
        return;
    }
    char h[700];
    int n = snprintf(h, sizeof h, "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n"
                                  "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: Content-Range, Content-Length\r\n"
                                  "Cache-Control: no-cache\r\nConnection: close\r\n",
                     part ? "206 Partial Content" : "200 OK", type, b - a + 1);
    if (part) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", a, b, size);
    n += snprintf(h + n, sizeof h - (size_t)n, "\r\n");
#ifdef _WIN32
    _fseeki64(f, a, SEEK_SET);
#else
    fseeko(f, (off_t)a, SEEK_SET);
#endif
    if (send_all(s, h, (size_t)n))
    {
        char *bb = malloc(65536);
        long long left = b - a + 1;
        while (bb && left > 0)
        {
            size_t k = fread(bb, 1, left < 65536 ? (size_t)left : 65536, f);
            if (!k || !send_all(s, bb, k)) break;
            left -= (long long)k;
        }
        free(bb);
    }
    fclose(f);
}

static void pics_bridge(sock_t s, const char *what, const char *path, const char *range, bool music)
{
    char rel[1024];
    query(path, "path", rel, sizeof rel);
    pct_decode(rel);
    // nothing above the folder, no drives
    bool ok = !strchr(rel, ':') && !strchr(rel, '\\') && rel[0] != '/';
    for (const char *p = rel; ok && *p; )
    {
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if ((l == 2 && !strncmp(p, "..", 2)) || (l == 1 && p[0] == '.')) ok = false;
        p = e ? e + 1 : p + l;
    }
    if (!ok) { send_text(s, "403 Forbidden", "text/plain", "not here\n", 9, NULL); return; }
    const char *root = music ? (cfg.music_dir[0] ? cfg.music_dir : sys_music_dir()) : cfg.pic_folder[0] ? cfg.pic_folder : sys_pictures_dir();
    char full[2048];
    snprintf(full, sizeof full, "%s%s%s", root, rel[0] ? "/" : "", rel);
#ifdef _WIN32
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
#endif
    if (!strcmp(what, "list"))
    {
        sys_dirent *d = NULL;
        int n = 0;
        if (!sys_list_dir(full, &d, &n)) { send_text(s, "404 Not Found", "text/plain", "no such folder\n", 15, NULL); return; }
        size_t cap = 256, len = 0;
        for (int i = 0; i < n; i++) cap += strlen(d[i].name) + 4;
        char *b = malloc(cap);
        if (!b) { sys_free_dir(d, n); return; }
        b[0] = 0;
        for (int i = 0; i < n; i++)
        {
            if (d[i].name[0] == '.' || (!d[i].is_dir && !pic_type(d[i].name) && !(music && music_type(d[i].name)))) continue;
            len += (size_t)snprintf(b + len, cap - len, "%c %s\n", d[i].is_dir ? 'd' : 'f', d[i].name);
        }
        sys_free_dir(d, n);
        send_text(s, "200 OK", "text/plain; charset=utf-8", b, len, NULL);
        free(b);
        return;
    }
    if (!strcmp(what, "file") && music && music_type(full)) { send_song(s, full, music_type(full), range); return; }
    if (!strcmp(what, "file"))
    {
        const char *type = pic_type(full);
        size_t n = 0;
        char *b = type ? sys_read_file(full, &n) : NULL;
        if (!b) { send_text(s, "404 Not Found", "text/plain", "no such picture\n", 16, NULL); return; }
        send_text(s, "200 OK", type, b, n, NULL);
        free(b);
        return;
    }
    send_text(s, "404 Not Found", "text/plain", "list or file\n", 13, NULL);
}

// ---------------------------------------------------------------- PicoAmp for the web, from this PC
// /web/... is PicoAmp for the web, fetched from its site (and kept for an hour), so a phone or a
// tablet in this network opens it on http://<this PC>:<port>/web/: then it may ask /sonos/ here (a
// page on https may not ask an http address of the network). PICOAMP_WEBAPP: another site (tests).
#define WEB_SITE  "https://jzzy-dev.github.io/web-picoamp/"
#define WEB_FILES 16
static struct { char name[96]; char type[64]; uint8_t *data; size_t len; uint32_t at; } web[WEB_FILES];

static void web_app(sock_t s, const char *path)
{
    if (!strcmp(path, "/web")) { send_text(s, "301 Moved", "text/plain", "", 0, "Location: /web/\r\n"); return; }
    char name[96];
    u_copy(name, sizeof name, path + 5);
    char *q = strchr(name, '?');
    if (q) *q = 0;
    if (!name[0]) u_copy(name, sizeof name, "index.html");
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_') || (c[0] == '.' && c[1] == '.'))
            { send_text(s, "404 Not Found", "text/plain", "not here\n", 9, NULL); return; }
    sys_lock(lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < WEB_FILES; i++)
    {
        if (!strcmp(web[i].name, name) && web[i].data && sys_millis() - web[i].at < 3600000u) slot = i;
        if (web[i].at < web[oldest].at || !web[i].data) oldest = i;
    }
    if (slot >= 0)
    {
        uint8_t *d = malloc(web[slot].len);
        size_t n = web[slot].len;
        char type[64];
        memcpy(d, web[slot].data, n);
        u_copy(type, sizeof type, web[slot].type);
        sys_unlock(lock);
        send_text(s, "200 OK", type, (const char *)d, n, NULL);
        free(d);
        return;
    }
    sys_unlock(lock);
    const char *site = getenv("PICOAMP_WEBAPP");
    char u[400], e[160];
    snprintf(u, sizeof u, "%s%s", site && *site ? site : WEB_SITE, name);
    int status = 0;
    net_conn *c = net_get(u, NULL, &status, e, sizeof e, NULL);
    if (!c || status != 200)
    {
        if (c) net_close(c);
        char m[300];
        int n = snprintf(m, sizeof m, "PicoAmp for the web could not be fetched (%s)\n", status ? "not there" : e);
        send_text(s, "502 Bad Gateway", "text/plain", m, (size_t)n, NULL);
        return;
    }
    char type[64];
    const char *ct = net_header(c, "Content-Type");
    // (the browser compiles wasm while it comes only when it is called so)
    u_copy(type, sizeof type, u_ends_with_ci(name, ".wasm") ? "application/wasm" : ct ? ct : "application/octet-stream");
    size_t cap = 65536, n = 0;
    uint8_t *d = malloc(cap);
    for (;;)
    {
        if (n == cap) { if (cap >= 16u << 20) break; cap *= 2; d = realloc(d, cap); }
        long r = net_read(c, d + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    net_close(c);
    send_text(s, "200 OK", type, (const char *)d, n, NULL);
    sys_lock(lock);
    free(web[oldest].data);
    u_copy(web[oldest].name, sizeof web[oldest].name, name);
    u_copy(web[oldest].type, sizeof web[oldest].type, type);
    web[oldest].data = d;
    web[oldest].len = n;
    web[oldest].at = sys_millis();
    sys_unlock(lock);
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
    bool options = !strncmp(req, "OPTIONS ", 8);
    if (!strncmp(req, "GET ", 4) || options)
    {
        const char *b0 = req + (options ? 8 : 4);
        const char *e = strchr(b0, ' ');
        size_t l = e ? (size_t)(e - b0) : 0;
        if (l >= sizeof path) l = sizeof path - 1;
        memcpy(path, b0, l);
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
    else if (options)
        // a page of another site asks first (CORS, and Chrome's private network access)
        send_text(s, "204 No Content", "text/plain", "", 0,
                  "Access-Control-Allow-Methods: GET\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Private-Network: true\r\nAccess-Control-Max-Age: 600\r\n");
    else if (!strncmp(p0, "/sonos/", 7)) sonos_bridge(s, p0 + 7, path);
    else if (!strcmp(p0, "/sing/open")) sing_open(s, path);
    else if (!strncmp(p0, "/usdb/", 6)) usdb_bridge(s, p0 + 6, path);
    else if (!strncmp(p0, "/pics/", 6)) pics_bridge(s, p0 + 6, path, NULL, false);
    else if (!strncmp(p0, "/music/", 7))
    {
        // (the Range asked: its line of the request)
        char range[96] = "";
        const char *r = strstr(lower, "\nrange:");
        if (r)
        {
            r += 7;
            while (*r == ' ') r++;
            size_t k = 0;
            while (r[k] && r[k] != '\r' && r[k] != '\n' && k + 1 < sizeof range) { range[k] = r[k]; k++; }
            range[k] = 0;
        }
        pics_bridge(s, p0 + 7, path, range[0] ? range : NULL, true);
    }
    else if (!strcmp(p0, "/web") || !strncmp(p0, "/web/", 5)) web_app(s, path);
    else if (!strcmp(p0, "/") || !strcmp(p0, "/stream.mp3") || !strcmp(p0, "/stream") || !strcmp(p0, "/;")) stream(s, meta);
    else if (!strcmp(p0, "/screen")) screen(s);
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
        else if (!strcmp(k, "vol"))
        {
            // the volume (the player's, or the followed program's in the mixer), mute
            query(path, "v", v, sizeof v);
            if (v[0]) audio_set_volume(atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v));
            ok = v[0] != 0;
        }
        else if (!strcmp(k, "mute"))
        {
            query(path, "on", v, sizeof v);
            if (v[0]) audio_set_mute(atoi(v) != 0);
            ok = v[0] != 0;
        }
        else if (!strcmp(k, "shuffle") || !strcmp(k, "repeat"))
        {
            // on=0 off, else on (repeat: 1 the list, 2 the track: both the player's repeat)
            int bit = k[0] == 's' ? MODE_SHUFFLE : MODE_REPEAT;
            query(path, "on", v, sizeof v);
            int m = atoi(v) ? (st.mode | bit) : (st.mode & ~bit);
            if (v[0] && m != st.mode) audio_set_mode(m);
            ok = v[0] != 0;
        }
        else if (!strcmp(k, "seek"))
        {
            query(path, "s", v, sizeof v);
            if (v[0]) audio_seek_rel(atoi(v));
            else { query(path, "to", v, sizeof v); if (v[0]) audio_seek_to(atoi(v)); else ok = false; }
        }
        else ok = false;
        if (ok) send_text(s, "200 OK", "text/plain", "ok\n", 3, NULL);
        else send_text(s, "400 Bad Request", "text/plain", "k: toggle play pause next prev seek shuffle repeat vol mute\n", 60, NULL);
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
                                      "\"elapsed\":%d,\"total\":%d,\"cover\":%u,\"has_cover\":%s,\"listeners\":%d,\"shuffle\":%s,\"repeat\":%s,\"error\":%s}\n",
                         st.state == ST_PLAYING ? "playing" : st.state == ST_PAUSED ? "paused" : "stopped",
                         t, a, al, src, st.elapsed, st.total, cg, hc ? "true" : "false", listeners,
                         (st.mode & MODE_SHUFFLE) ? "true" : "false", (st.mode & MODE_REPEAT) ? "true" : "false", e);
        send_text(s, "200 OK", "application/json", b, (size_t)n, NULL);
    }
    else if (!strcmp(p0, "/cover.jpg"))
    {
        // ?w=281&h=192 (or anything wider than 80): the full screen's, else the cover view's
        char wv[16];
        query(path, "w", wv, sizeof wv);
        bool big = atoi(wv) > COVER_PX;
        sys_lock(lock);
        uint8_t *from = big ? cover_big : cover_jpg;
        size_t fl = big ? cover_big_len : cover_len;
        uint8_t *c = fl ? malloc(fl) : NULL;
        size_t n = c ? fl : 0;
        unsigned cg = cover_n;
        if (c) memcpy(c, from, n);
        sys_unlock(lock);
        char x[48];
        snprintf(x, sizeof x, "X-Cover-Gen: %u\r\n", cg);
        if (c) send_text(s, "200 OK", "image/jpeg", (const char *)c, n, x);
        else send_text(s, "404 Not Found", "text/plain", "no cover\n", 9, x);
        free(c);
    }
    else if (!strcmp(p0, "/picoamp.m3u"))
    {
        char b[512];
        char su[96];
        u_copy(su, sizeof su, url);
        char *sl = strrchr(su, '/');
        if (sl) u_copy(sl, sizeof su - (size_t)(sl - su), "/screen");
        int n = snprintf(b, sizeof b, "#EXTM3U\r\n#EXTINF:-1,PicoAmp on the PC\r\n%s\r\n#EXTINF:-1,PicoAmp on the PC: screen and remote\r\n%s\r\n", url, su);
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
