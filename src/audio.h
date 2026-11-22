// audio.h - the player: what the PicoVerse Audio Engine does on the RP2350,
// here on the PC. A play list of files or one internet station; its own
// decoding thread; the equalizer; the sound for the visualiser.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "eq.h"

enum { ST_STOPPED = 0, ST_PLAYING = 1, ST_PAUSED = 2 };
enum { MODE_REPEAT = 1, MODE_SHUFFLE = 2 };

typedef struct
{
    unsigned gen;            // changes whenever something below changes
    int  state;
    int  volume;             // 0..100
    bool muted;
    int  elapsed, total;     // seconds (total 0: unknown)
    int  elapsed_ms;         // in the song, ms (a station: since its title came, as heard; for the lyrics)
    bool untimed;            // a station's song that began before it was tuned in (its first title): elapsed_ms is not its time
    int  bitrate;            // kbit/s
    int  rate;               // Hz
    int  channels;
    bool seekable, stream, buffering, tls;
    int  buf_pct;            // streams: how full the buffer is
    int  pl_pos, pl_count;   // place in the play list
    int  mode;               // MODE_*
    char title[256];
    char artist[256];        // a station: empty
    char album[256];         // a station: its name
    char url[2048];          // the station that plays (or the file)
    char error[160];         // the last error (shown once: err_gen)
    unsigned err_gen;
    bool system;             // another program's music (Spotify ...): audio_follow_system()
    char source[64];         // that program ("Spotify")
    unsigned art_gen;        // system: +1 when its picture changes (np_art)
} audio_status;

bool audio_init(bool null_device);
void audio_shutdown(void);

// play files[start] of a list of files (UTF-8 paths); the list is copied
void audio_play_files(const char *const *files, int count, int start);
void audio_play_url(const char *url, const char *name);
// a list's stations: the play list (N / P the next / previous station)
void audio_play_stations(const char *const *urls, const char *const *names, int count, int start);
// follow what another program of the PC plays (Spotify first; nowplaying.h): its
// track is the player's, the keys go to it, the bars move on the PC's sound.
// Playing a file or a station here ends it.
void audio_follow_system(void);
void audio_follow_sonos(const char *id);          // a Sonos room (sonos.h): as a program of the PC, its sound stays there
bool audio_follow_art(uint8_t **data, size_t *len);   // the picture of what the followed player plays
bool audio_system_supported(void);
void audio_toggle_pause(void);
void audio_stop(void);
void audio_next(void);
void audio_prev(void);
void audio_seek_rel(int seconds);
void audio_seek_to(int seconds);
void audio_set_volume(int v);
void audio_set_mute(bool m);
void audio_set_balance(int b);       // 0 left .. 50 centre .. 100 right
void audio_set_fade(int permille);   // the sleep timer and the alarm: 0..1000 of the volume
void audio_set_mode(int mode);
void audio_set_eq(const int8_t gains[EQ_BANDS]);

void audio_status_get(audio_status *s);

// what is heard (at the volume), for the MSX link: PicoAmp's own music, or the
// followed program's sound; stereo 16-bit (the audio threads call it)
typedef void (*audio_tap_fn)(const int16_t *stereo, uint32_t frames, uint32_t rate);
void audio_set_tap(audio_tap_fn fn);
