// nowplaying_host.c - the test build's pretend player (see nowplaying.h):
// PICOAMP_FAKE_MEDIA="App|Title|Artist|Album|seconds" plays; its keys work
// (pause, next / previous: "Title 2" ..., seek, shuffle, repeat, the volume);
// PICOAMP_FAKE_ART is its picture (a JPEG or PNG file).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nowplaying.h"
#include "sys.h"
#include "util.h"

static bool     on;
static np_info  base;
static int      track;               // next / previous
static int      total_ms;
static uint32_t t0;                  // sys_millis() at elapsed 0 (while it plays)
static int      paused_at;
static int      state;
static int      volume = 50;
static bool     muted, shuffle;
static int      repeat;

bool np_supported(void) { const char *e = getenv("PICOAMP_FAKE_MEDIA"); return e && *e; }

void np_start(void)
{
    if (!np_supported()) return;
    char t[1200];
    u_copy(t, sizeof t, getenv("PICOAMP_FAKE_MEDIA"));
    memset(&base, 0, sizeof base);
    char *f[5] = { 0 };
    char *p = t;
    for (int i = 0; i < 5; i++) { f[i] = p; p = p ? strchr(p, '|') : NULL; if (p) *p++ = 0; }
    u_copy(base.app, sizeof base.app, f[0] ? f[0] : "");
    u_copy(base.title, sizeof base.title, f[1] ? f[1] : "");
    u_copy(base.artist, sizeof base.artist, f[2] ? f[2] : "");
    u_copy(base.album, sizeof base.album, f[3] ? f[3] : "");
    total_ms = f[4] ? atoi(f[4]) * 1000 : 0;
    track = 0;
    state = NP_PLAYING;
    t0 = sys_millis();
    on = true;
}

void np_end(void) { on = false; }

static int elapsed(void)
{
    int e = state == NP_PLAYING ? (int)(sys_millis() - t0) : paused_at;
    if (total_ms && e > total_ms) e = total_ms;
    return e;
}

bool np_get(np_info *o)
{
    if (!on) return false;
    *o = base;
    if (track) snprintf(o->title, sizeof o->title, "%s %d", base.title, track + 1);
    o->state = state;
    o->elapsed_ms = elapsed();
    o->at = sys_millis();
    o->total_ms = total_ms;
    o->can_seek = o->can_next = o->can_prev = o->can_shuffle = o->can_repeat = true;
    o->shuffle = shuffle;
    o->repeat = repeat;
    o->volume = volume;
    o->muted = muted;
    o->art_gen = (unsigned)track + 1;
    return true;
}

bool np_art(uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    const char *f = getenv("PICOAMP_FAKE_ART");
    if (!on || !f || !*f) return false;
    *data = (uint8_t *)sys_read_file(f, len);
    return *data != NULL;
}

void np_cmd(int cmd, int arg)
{
    if (!on) return;
    switch (cmd)
    {
    case NP_TOGGLE:
        if (state == NP_PLAYING) { paused_at = elapsed(); state = NP_PAUSED; }
        else { t0 = sys_millis() - (uint32_t)paused_at; state = NP_PLAYING; }
        break;
    case NP_PLAY:  if (state != NP_PLAYING) np_cmd(NP_TOGGLE, 0); break;
    case NP_PAUSE: if (state == NP_PLAYING) np_cmd(NP_TOGGLE, 0); break;
    case NP_STOP:  state = NP_STOPPED; paused_at = 0; break;
    case NP_NEXT:  track++; t0 = sys_millis(); paused_at = 0; break;
    case NP_PREV:  if (track) track--; t0 = sys_millis(); paused_at = 0; break;
    case NP_SEEK_TO:
        if (state == NP_PLAYING) t0 = sys_millis() - (uint32_t)arg; else paused_at = arg;
        break;
    case NP_SHUFFLE: shuffle = arg != 0; break;
    case NP_REPEAT:  repeat = arg; break;
    }
}

void np_volume(int v, bool m) { volume = v; muted = m; }
