// msxlink.h - the MSX link (K): PicoAmp on the MSX (the PicoVerse 2350
// cartridge) plays what this PC plays, as an internet radio station of the
// PC: an MP3 stream of its sound with the song in its ICY titles, the cover,
// and the keys back (the MSX's pause, next and previous go to the player here,
// and through it to Spotify when it follows Spotify).
//
//   http://<this PC>:<port>/stream.mp3   the sound (MP3, ICY titles; the reply has X-PicoAmp: 1)
//   /cover.jpg                           the cover, 80 x 80, baseline JPEG (X-Cover-Gen: n)
//   /cmd?k=toggle|play|pause|next|prev|seek&s=+-seconds|to=seconds
//   /now                                 what plays (JSON)
//   /picoamp.m3u                         a station list with the stream, for the card
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool msxlink_start(int port, int kbps);   // false: the port cannot be opened (msxlink_error())
void msxlink_stop(void);
bool msxlink_on(void);
int  msxlink_listeners(void);             // MSXs that play the stream now
const char *msxlink_url(void);            // http://192.168.1.20:8765/stream.mp3
const char *msxlink_error(void);
void msxlink_tick(void);                  // the screen's thread, every frame: the cover, the notes
// the sound (the audio threads): stereo 16-bit at its rate
void msxlink_feed(const int16_t *stereo, uint32_t frames, uint32_t rate);
