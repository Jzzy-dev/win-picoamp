// nowplaying.h - what another program of the PC plays (Spotify first), through
// Windows's media controls (the ones of the volume flyout and the lock screen):
// its title, artist, album, picture and place in the track, and its keys
// (play / pause, next, previous, seek, shuffle, repeat); and its own volume in
// the mixer. PicoAmp shows it as its own track (audio.c, SRC_SYSTEM) and the
// bars move on the sound the PC plays (a loopback of the sound card).
//
// Windows: nowplaying_win32.c. The test build: nowplaying_host.c, a pretend
// player (PICOAMP_FAKE_MEDIA="App|Title|Artist|Album|seconds", PICOAMP_FAKE_ART=a picture).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { NP_NONE = 0, NP_STOPPED, NP_PLAYING, NP_PAUSED };
enum { NP_TOGGLE = 1, NP_PLAY, NP_PAUSE, NP_STOP, NP_NEXT, NP_PREV, NP_SEEK_TO, NP_SHUFFLE, NP_REPEAT };
enum { NP_REPEAT_OFF = 0, NP_REPEAT_TRACK = 1, NP_REPEAT_LIST = 2 };

typedef struct
{
    int      state;              // NP_xxx (NP_NONE: no program plays anything)
    char     app[64];            // "Spotify", "Chrome" ... (as shown)
    char     title[256];
    char     artist[256];
    char     album[256];
    int      elapsed_ms, total_ms;   // total 0: unknown
    bool     can_seek, can_next, can_prev, can_shuffle, can_repeat;
    bool     shuffle;
    int      repeat;             // NP_REPEAT_xxx
    int      volume;             // the program's own in the mixer 0..100 (-1: not known)
    bool     muted;
    unsigned art_gen;            // +1 when the picture changes
    uint32_t at;                 // sys_millis() when elapsed_ms was so
    uint32_t audio_pid;          // its process (the loopback of its sound only); 0: all of the PC
} np_info;

bool np_supported(void);         // false: this PC cannot (the entry does not show)
void np_start(void);             // follow (a thread of its own)
void np_end(void);
bool np_get(np_info *out);       // false: not started
// the picture of what plays (malloc'd JPEG / PNG bytes); false: none
bool np_art(uint8_t **data, size_t *len);
void np_cmd(int cmd, int arg);   // NP_SEEK_TO: ms; NP_SHUFFLE: 0/1; NP_REPEAT: NP_REPEAT_xxx
void np_volume(int volume, bool muted);   // the program's own in the mixer
