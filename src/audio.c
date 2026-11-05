// audio.c - the player (see audio.h)
//
//   decoding thread: decoder (file or stream) -> stereo s16 at the source's
//   rate -> equalizer -> converter to the device's rate -> PCM ring
//   device callback: PCM ring -> visualiser (what is heard now) -> volume -> out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#include "miniaudio.h"
#include "audio.h"
#include "stream.h"
#include "tags.h"
#include "viz.h"
#include "sys.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define CHUNK        1152
#define PREBUFFER    (48 * 1024)     // stream bytes before the sound starts
#define REBUFFER     (32 * 1024)

enum { SRC_NONE, SRC_FILE, SRC_STREAM };
enum { C_FILES = 1, C_URL, C_PAUSE, C_STOP, C_NEXT, C_PREV, C_SEEK_REL, C_SEEK_TO };

typedef struct { int type; int arg; char *url, *name; char **files; int n; } cmd_t;

static ma_context   ctx;
static ma_device    dev;
static bool         dev_ok;
static uint32_t     dev_rate = 48000;
static ma_pcm_rb    rb;
static sys_thread  *dec_thread;
static volatile bool quit;

// commands from the screen
static sys_mutex   *cmd_lock;
static cmd_t        cmdq[64];
static int          cmd_n;
static volatile bool dec_abort;

// shared with the callback
static volatile int      flush_seq, flush_ack;
static volatile bool     out_paused;
static volatile uint64_t played;          // device frames since the last flush
static volatile int      gain_l = 32768, gain_r = 32768;
static int               balance = 50;
static int               fade = 1000;

// the status
static sys_mutex   *st_lock;
static audio_status st;

// the decoding thread's own
static int          src = SRC_NONE;
static ma_decoder   dec;
static bool         dec_open;
static ma_data_converter conv;
static bool         conv_open;
static uint32_t     src_rate, src_ch;
static uint64_t     base_frames;          // source frames before the flush (seek)
static stream      *strm;
static bool         strm_decoding, strm_rebuffer;
static unsigned     strm_meta_seen;
static char       **pl;
static int          pl_n, pl_pos;
static int          volume = 80;
static bool         muted;

// ---------------- the device ----------------
static void callback(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    (void)d; (void)in;
    int16_t *o = out;
    if (flush_seq != flush_ack)
    {
        ma_uint32 n;
        while ((n = ma_pcm_rb_available_read(&rb)) > 0)
        {
            void *p;
            if (ma_pcm_rb_acquire_read(&rb, &n, &p) != MA_SUCCESS) break;
            ma_pcm_rb_commit_read(&rb, n);
        }
        played = 0;
        flush_ack = flush_seq;
    }
    ma_uint32 done = 0;
    if (!out_paused)
        while (done < frames)
        {
            ma_uint32 n = frames - done;
            void *p;
            if (ma_pcm_rb_acquire_read(&rb, &n, &p) != MA_SUCCESS || n == 0) break;
            memcpy(o + done * 2, p, (size_t)n * 4);
            ma_pcm_rb_commit_read(&rb, n);
            done += n;
        }
    if (done)
    {
        viz_feed(o, done, dev_rate);
        int gl = gain_l, gr = gain_r;
        for (ma_uint32 i = 0; i < done; i++)
        {
            o[2 * i] = (int16_t)(((int32_t)o[2 * i] * gl) >> 15);
            o[2 * i + 1] = (int16_t)(((int32_t)o[2 * i + 1] * gr) >> 15);
        }
        played += done;
    }
    if (done < frames) memset(o + done * 2, 0, (size_t)(frames - done) * 4);
}

static void apply_gain(void)
{
    // as the engine: (100 - volume) / 2 dB down
    int g = (muted || volume <= 0) ? 0 : (int)(32768.0 * pow(10.0, -(100 - volume) / 40.0) * fade / 1000.0);
    // the balance takes the other side down (centre: both full)
    gain_l = balance <= 50 ? g : g * (100 - balance) / 50;
    gain_r = balance >= 50 ? g : g * balance / 50;
}

// throw away what is in the ring (and wait until the callback did)
static void flush_out(void)
{
    flush_seq++;
    uint32_t t0 = sys_millis();
    while (flush_ack != flush_seq && sys_millis() - t0 < 300) sys_sleep_ms(2);
    if (flush_ack != flush_seq)
    {
        // no callback (no device): the ring is ours
        ma_pcm_rb_reset(&rb);
        played = 0;
        flush_ack = flush_seq;
    }
}

// ---------------- status helpers ----------------
static void st_changed(void) { st.gen++; }

static void set_error(const char *msg)
{
    sys_lock(st_lock);
    u_copy(st.error, sizeof st.error, msg);
    st.err_gen++;
    st_changed();
    sys_unlock(st_lock);
}

static void close_source(void)
{
    if (dec_open) ma_decoder_uninit(&dec);
    if (conv_open) ma_data_converter_uninit(&conv, NULL);
    dec_open = conv_open = false;
    if (strm) stream_release(strm);
    strm = NULL;
    strm_decoding = strm_rebuffer = false;
    src = SRC_NONE;
}

static bool open_converter(void)
{
    ma_format f;
    ma_uint32 ch, rate;
    if (ma_decoder_get_data_format(&dec, &f, &ch, &rate, NULL, 0) != MA_SUCCESS || !rate || !ch) return false;
    src_rate = rate;
    src_ch = ch;
    ma_data_converter_config cc = ma_data_converter_config_init(ma_format_s16, ma_format_s16, 2, 2, rate, dev_rate);
    cc.resampling.algorithm = ma_resample_algorithm_linear;
    cc.resampling.linear.lpfOrder = 4;
    conv_open = ma_data_converter_init(&cc, NULL, &conv) == MA_SUCCESS;
    return conv_open;
}

static void stop_all(void)
{
    close_source();
    flush_out();
    out_paused = false;
    base_frames = 0;
    sys_lock(st_lock);
    st.state = ST_STOPPED;
    st.elapsed = 0;
    st.buffering = false;
    st_changed();
    sys_unlock(st_lock);
}

static uint64_t file_size(const char *path)
{
    FILE *f = sys_fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0 ? (uint64_t)n : 0;
}

static bool open_track(int i)
{
    close_source();
    flush_out();
    eq_reset();
    base_frames = 0;
    if (i < 0 || i >= pl_n) return false;
    pl_pos = i;
    const char *path = pl[i];
    ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_result r;
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (wn > 0 ? wn : 1));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, wn);
    r = ma_decoder_init_file_w(w, &dc, &dec);
    free(w);
#else
    r = ma_decoder_init_file(path, &dc, &dec);
#endif
    if (r != MA_SUCCESS)
    {
        char m[160];
        snprintf(m, sizeof m, "Cannot play %s", u_basename(path));
        set_error(m);
        return false;
    }
    dec_open = true;
    if (!open_converter()) { close_source(); set_error("Unknown sound format"); return false; }
    ma_uint64 len = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &len);
    track_tags t;
    tags_read(path, &t);
    int total = src_rate ? (int)(len / src_rate) : 0;
    uint64_t fs = file_size(path);
    sys_lock(st_lock);
    if (t.title[0]) u_copy(st.title, sizeof st.title, t.title);
    else
    {
        u_copy(st.title, sizeof st.title, u_basename(path));
        char *dot = strrchr(st.title, '.');
        if (dot && dot != st.title) *dot = 0;
    }
    u_copy(st.artist, sizeof st.artist, t.artist);
    u_copy(st.album, sizeof st.album, t.album);
    u_copy(st.url, sizeof st.url, path);
    st.total = total;
    st.elapsed = 0;
    st.rate = (int)src_rate;
    st.channels = (int)src_ch;
    st.bitrate = u_ends_with_ci(path, ".wav") ? (int)(src_rate * src_ch * 16 / 1000)
               : (len && src_rate) ? (int)(fs * 8 / 1000 * src_rate / len) : 0;
    st.seekable = len > 0;
    st.stream = st.buffering = st.tls = false;
    st.buf_pct = 0;
    st.pl_pos = i;
    st.pl_count = pl_n;
    st.state = ST_PLAYING;
    st_changed();
    sys_unlock(st_lock);
    out_paused = false;
    src = SRC_FILE;
    return true;
}

// a stream for the decoder: it can go forward, and back within its first
// 64 KB (the decoder looks for an ID3 tag at the start and goes back if
// there is none)
static unsigned char s_hist[65536];
static size_t        s_hist_len, s_pos;

static size_t s_take(void *buf, size_t n)
{
    if (s_pos < s_hist_len)
    {
        size_t k = s_hist_len - s_pos < n ? s_hist_len - s_pos : n;
        memcpy(buf, s_hist + s_pos, k);
        s_pos += k;
        return k;
    }
    size_t k = strm ? stream_read(strm, buf, n, &dec_abort) : 0;
    if (s_hist_len == s_pos && s_hist_len < sizeof s_hist)
    {
        size_t keep = sizeof s_hist - s_hist_len < k ? sizeof s_hist - s_hist_len : k;
        memcpy(s_hist + s_hist_len, buf, keep);
        s_hist_len += keep;
    }
    s_pos += k;
    return k;
}

static ma_result s_read(ma_decoder *d, void *buf, size_t n, size_t *got)
{
    (void)d;
    *got = s_take(buf, n);
    return *got ? MA_SUCCESS : MA_AT_END;
}

static ma_result s_seek(ma_decoder *d, ma_int64 off, ma_seek_origin o)
{
    (void)d;
    ma_int64 target;
    if (o == ma_seek_origin_start) target = off;
    else if (o == ma_seek_origin_current) target = (ma_int64)s_pos + off;
    else return MA_NOT_IMPLEMENTED;                 // the end of a live stream
    if (target < 0) return MA_INVALID_ARGS;
    if ((size_t)target <= s_hist_len || (size_t)target <= s_pos)
    {
        if ((size_t)target > s_hist_len) return MA_NOT_IMPLEMENTED;   // behind, and no longer kept
        s_pos = (size_t)target;
        return MA_SUCCESS;
    }
    unsigned char junk[4096];
    while ((ma_int64)s_pos < target)
    {
        ma_int64 left = target - (ma_int64)s_pos;
        if (!s_take(junk, left < (ma_int64)sizeof junk ? (size_t)left : sizeof junk)) return MA_AT_END;
    }
    return MA_SUCCESS;
}

static void open_url(const char *url, const char *name)
{
    close_source();
    flush_out();
    eq_reset();
    base_frames = 0;
    strm = stream_start(url);
    strm_meta_seen = 0;
    s_hist_len = s_pos = 0;
    src = SRC_STREAM;
    out_paused = false;
    sys_lock(st_lock);
    st.title[0] = st.artist[0] = 0;
    u_copy(st.album, sizeof st.album, name);
    u_copy(st.url, sizeof st.url, url);
    st.total = st.elapsed = 0;
    st.bitrate = st.rate = st.channels = 0;
    st.seekable = false;
    st.stream = true;
    st.buffering = true;
    st.buf_pct = 0;
    st.tls = !u_strnicmp(url, "https:", 6);
    st.pl_pos = 0;
    st.pl_count = 1;
    st.state = ST_PLAYING;
    st_changed();
    sys_unlock(st_lock);
}

static void free_playlist(void)
{
    for (int i = 0; i < pl_n; i++) free(pl[i]);
    free(pl);
    pl = NULL;
    pl_n = pl_pos = 0;
}

static int pick_next(int dir)
{
    if (pl_n <= 0) return -1;
    if (st.mode & MODE_SHUFFLE && pl_n > 1)
    {
        int n;
        do n = rand() % pl_n; while (n == pl_pos);
        return n;
    }
    int n = pl_pos + dir;
    if (n >= pl_n) return (st.mode & MODE_REPEAT) ? 0 : -1;
    if (n < 0) return (st.mode & MODE_REPEAT) ? pl_n - 1 : 0;
    return n;
}

static int elapsed_now(void)
{
    uint64_t f = played;
    double s = (src_rate ? (double)base_frames / src_rate : 0) + (double)f / dev_rate;
    return (int)s;
}

static void seek_to(int sec)
{
    if (src != SRC_FILE || !dec_open || !src_rate) return;
    if (sec < 0) sec = 0;
    if (st.total && sec >= st.total) sec = st.total - 1;
    uint64_t target = (uint64_t)sec * src_rate;
    if (ma_decoder_seek_to_pcm_frame(&dec, target) != MA_SUCCESS) return;
    flush_out();
    if (conv_open) ma_data_converter_uninit(&conv, NULL);
    conv_open = false;
    open_converter();
    eq_reset();
    base_frames = target;
}

static void do_cmd(cmd_t *c)
{
    switch (c->type)
    {
    case C_FILES:
        free_playlist();
        pl = c->files;
        pl_n = c->n;
        c->files = NULL;
        if (!open_track(c->arg)) stop_all();
        break;
    case C_URL:
        free_playlist();
        open_url(c->url, c->name ? c->name : "");
        break;
    case C_PAUSE:
        if (src == SRC_NONE)
        {
            if (pl_n) { if (!open_track(pl_pos)) stop_all(); }
            else if (st.stream && st.url[0])
            {
                char u[2048], n[256];
                u_copy(u, sizeof u, st.url);
                u_copy(n, sizeof n, st.album);
                open_url(u, n);
            }
            break;
        }
        if (src == SRC_STREAM)
        {
            // a station does not pause: it stops (and starts again live)
            close_source();
            flush_out();
            sys_lock(st_lock);
            st.state = ST_STOPPED;
            st.buffering = false;
            st_changed();
            sys_unlock(st_lock);
            break;
        }
        out_paused = !out_paused;
        sys_lock(st_lock);
        st.state = out_paused ? ST_PAUSED : ST_PLAYING;
        st_changed();
        sys_unlock(st_lock);
        break;
    case C_STOP:
        stop_all();
        break;
    case C_NEXT:
    case C_PREV:
        if (pl_n)
        {
            int n = pick_next(c->type == C_NEXT ? 1 : -1);
            if (c->type == C_PREV && elapsed_now() >= 3 && src == SRC_FILE) n = pl_pos;   // back to the start first
            if (n < 0) n = 0;
            if (!open_track(n)) stop_all();
        }
        break;
    case C_SEEK_REL:
        seek_to(elapsed_now() + c->arg);
        break;
    case C_SEEK_TO:
        seek_to(c->arg);
        break;
    }
    if (c->files) { for (int i = 0; i < c->n; i++) free(c->files[i]); free(c->files); }
    free(c->url);
    free(c->name);
}

static bool take_cmds(void)
{
    cmd_t q[64];
    int n;
    sys_lock(cmd_lock);
    n = cmd_n;
    memcpy(q, cmdq, sizeof(cmd_t) * (size_t)n);
    cmd_n = 0;
    dec_abort = false;
    sys_unlock(cmd_lock);
    for (int i = 0; i < n; i++) do_cmd(&q[i]);
    return n > 0;
}

static void post(cmd_t c)
{
    sys_lock(cmd_lock);
    if (cmd_n < 64) cmdq[cmd_n++] = c;
    else { free(c.url); free(c.name); }
    dec_abort = true;
    sys_unlock(cmd_lock);
}

// the decoded frames: to stereo, equalizer, to the device's rate, into the ring
static void push_frames(const int16_t *in, ma_uint64 frames)
{
    static int16_t stereo[CHUNK * 2 * 2];
    static int16_t outb[CHUNK * 8 * 2];
    for (ma_uint64 i = 0; i < frames; i++)
    {
        if (src_ch == 1) stereo[2 * i] = stereo[2 * i + 1] = in[i];
        else { stereo[2 * i] = in[i * src_ch]; stereo[2 * i + 1] = in[i * src_ch + 1]; }
    }
    eq_process(stereo, (uint32_t)frames, src_rate);
    ma_uint64 left = frames;
    const int16_t *p = stereo;
    while (left && !quit)
    {
        ma_uint64 fin = left, fout = CHUNK * 8;
        ma_data_converter_process_pcm_frames(&conv, p, &fin, outb, &fout);
        p += fin * 2;
        left -= fin;
        ma_uint64 w = 0;
        while (w < fout && !quit)
        {
            ma_uint32 n = (ma_uint32)(fout - w);
            void *dst;
            if (ma_pcm_rb_acquire_write(&rb, &n, &dst) != MA_SUCCESS || n == 0)
            {
                if (dec_abort) return;          // a command: the rest does not matter
                sys_sleep_ms(3);
                continue;
            }
            memcpy(dst, outb + w * 2, (size_t)n * 4);
            ma_pcm_rb_commit_write(&rb, n);
            w += n;
        }
        if (fin == 0 && fout == 0) break;
    }
}

static void stream_poll(void)
{
    stream_info si;
    stream_info_get(strm, &si);
    size_t have = stream_buffered(strm);
    if (si.failed && !strm_decoding)
    {
        set_error(si.error);
        stop_all();
        return;
    }
    sys_lock(st_lock);
    if (si.meta_gen != strm_meta_seen)
    {
        strm_meta_seen = si.meta_gen;
        u_copy(st.title, sizeof st.title, si.title);
        st_changed();
    }
    if (si.station[0] && !st.album[0]) { u_copy(st.album, sizeof st.album, si.station); st_changed(); }
    if (si.bitrate != st.bitrate) { st.bitrate = si.bitrate; st_changed(); }
    st.tls = si.tls || st.tls;
    bool buffering = !strm_decoding || strm_rebuffer;
    int pct = buffering ? (int)(have * 100 / (strm_decoding ? REBUFFER : PREBUFFER)) : si.fill_pct;
    if (pct > 100) pct = 100;
    if (buffering != st.buffering || pct != st.buf_pct) { st.buffering = buffering; st.buf_pct = pct; st_changed(); }
    sys_unlock(st_lock);
}

static int dec_main(void *arg)
{
    (void)arg;
    int16_t buf[CHUNK * 8];
    while (!quit)
    {
        take_cmds();
        if (src == SRC_NONE || out_paused) { sys_sleep_ms(10); continue; }
        if (src == SRC_STREAM)
        {
            stream_poll();
            if (src != SRC_STREAM) continue;
            stream_info si;
            stream_info_get(strm, &si);
            size_t have = stream_buffered(strm);
            if (!strm_decoding)
            {
                if (have < PREBUFFER && !si.ended) { sys_sleep_ms(20); continue; }
                ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 0, 0);
                dc.encodingFormat = ma_encoding_format_mp3;
                if (ma_decoder_init(s_read, s_seek, NULL, &dc, &dec) != MA_SUCCESS)
                {
                    if (dec_abort) continue;
                    set_error("The stream is not MP3");
                    stop_all();
                    continue;
                }
                dec_open = true;
                if (!open_converter()) { set_error("Unknown sound format"); stop_all(); continue; }
                strm_decoding = true;
                sys_lock(st_lock);
                st.rate = (int)src_rate;
                st.channels = (int)src_ch;
                st_changed();
                sys_unlock(st_lock);
            }
            if (strm_rebuffer)
            {
                if (have < REBUFFER && !si.ended && !si.failed) { sys_sleep_ms(20); continue; }
                strm_rebuffer = false;
            }
            else if (have < 2048 && !si.ended && !si.failed && ma_pcm_rb_available_read(&rb) < dev_rate / 10)
            {
                strm_rebuffer = true;
                continue;
            }
        }
        if (ma_pcm_rb_available_write(&rb) < CHUNK * 4) { sys_sleep_ms(5); continue; }
        ma_uint64 got = 0;
        if (src_ch > 8) { set_error("Too many channels"); stop_all(); continue; }
        ma_decoder_read_pcm_frames(&dec, buf, CHUNK, &got);     // (buf: up to 8 channels)
        if (got)
        {
            push_frames(buf, got);
            continue;
        }
        if (dec_abort) continue;
        // the end of the source
        if (src == SRC_STREAM)
        {
            stream_info si;
            stream_info_get(strm, &si);
            set_error(si.failed ? si.error : "The stream ended");
            stop_all();
            continue;
        }
        // a file: let the ring play out, then the next one
        while (ma_pcm_rb_available_read(&rb) > 0 && !dec_abort && !quit && dev_ok) sys_sleep_ms(10);
        if (dec_abort) continue;
        int n = pick_next(1);
        if (n < 0 || !open_track(n))
        {
            close_source();
            sys_lock(st_lock);
            st.state = ST_STOPPED;
            st.elapsed = 0;
            st_changed();
            sys_unlock(st_lock);
            if (n < 0) pl_pos = 0;
        }
    }
    close_source();
    return 0;
}

// ---------------- the screen's side ----------------
bool audio_init(bool null_device)
{
    cmd_lock = sys_mutex_new();
    st_lock = sys_mutex_new();
    viz_init();
    // the sound card; without one (a server, a remote session) a silent
    // device, so the player still runs and shows the music
    bool silent = false;
    for (int pass = null_device ? 1 : 0; pass < 2 && !dev_ok; pass++)
    {
        ma_backend nb = ma_backend_null;
        if (ma_context_init(pass ? &nb : NULL, pass ? 1 : 0, NULL, &ctx) != MA_SUCCESS) continue;
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format = ma_format_s16;
        c.playback.channels = 2;
        c.sampleRate = pass ? 48000 : 0;
        c.periodSizeInMilliseconds = 10;
        c.dataCallback = callback;
        if (ma_device_init(&ctx, &c, &dev) == MA_SUCCESS)
        {
            dev_rate = dev.sampleRate ? dev.sampleRate : 48000;
            dev_ok = true;
            silent = pass == 1 && !null_device;
        }
        else ma_context_uninit(&ctx);
    }
    ma_pcm_rb_init(ma_format_s16, 2, dev_rate / 4, NULL, NULL, &rb);   // 250 ms
    if (dev_ok) ma_device_start(&dev);
    st.volume = volume;
    apply_gain();
    srand((unsigned)sys_unix_time());
    dec_thread = sys_thread_start(dec_main, NULL);
    if (!dev_ok) set_error("No sound device");
    else if (silent) set_error("No sound device: playing silently");
    return dev_ok;
}

void audio_shutdown(void)
{
    quit = true;
    dec_abort = true;
    sys_thread_join(dec_thread);
    if (dev_ok) ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    ma_pcm_rb_uninit(&rb);
}

void audio_play_files(const char *const *files, int count, int start)
{
    if (count <= 0) return;
    cmd_t c = { C_FILES, start, NULL, NULL, malloc(sizeof(char *) * (size_t)count), count };
    for (int i = 0; i < count; i++) c.files[i] = strdup(files[i]);
    post(c);
}

void audio_play_url(const char *url, const char *name)
{
    cmd_t c = { C_URL, 0, strdup(url), strdup(name ? name : ""), NULL, 0 };
    post(c);
}

static void simple(int type, int arg) { cmd_t c = { type, arg, NULL, NULL, NULL, 0 }; post(c); }
void audio_toggle_pause(void) { simple(C_PAUSE, 0); }
void audio_stop(void)         { simple(C_STOP, 0); }
void audio_next(void)         { simple(C_NEXT, 0); }
void audio_prev(void)         { simple(C_PREV, 0); }
void audio_seek_rel(int s)    { simple(C_SEEK_REL, s); }
void audio_seek_to(int s)     { simple(C_SEEK_TO, s); }

void audio_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    volume = v;
    apply_gain();
    sys_lock(st_lock);
    st.volume = v;
    st_changed();
    sys_unlock(st_lock);
}

void audio_set_mute(bool m)
{
    muted = m;
    apply_gain();
    sys_lock(st_lock);
    st.muted = m;
    st_changed();
    sys_unlock(st_lock);
}

void audio_set_fade(int permille)
{
    fade = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
    apply_gain();
}

void audio_set_balance(int b)
{
    balance = b < 0 ? 0 : b > 100 ? 100 : b;
    apply_gain();
}

void audio_set_mode(int mode)
{
    sys_lock(st_lock);
    st.mode = mode;
    st_changed();
    sys_unlock(st_lock);
}

void audio_set_eq(const int8_t gains[EQ_BANDS]) { eq_set(gains); }

void audio_status_get(audio_status *out)
{
    sys_lock(st_lock);
    if (st.state != ST_STOPPED && src == SRC_FILE)
    {
        int e = elapsed_now();
        if (st.total && e > st.total) e = st.total;
        st.elapsed = e;
    }
    else if (st.state == ST_PLAYING && src == SRC_STREAM) st.elapsed = elapsed_now();
    *out = st;
    sys_unlock(st_lock);
}
