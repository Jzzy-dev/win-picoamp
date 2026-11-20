// mp3enc.h - PCM to MP3, for the MSX link's stream (msxlink.c).
// Windows: Media Foundation's own MP3 encoder (Windows 8 and later). The test
// build: frames of silence, in time (what is checked there is the link, not the sound).
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct mp3enc mp3enc;

// 16-bit PCM, interleaved; rate 32000, 44100 or 48000, 1 or 2 channels
mp3enc *mp3enc_open(int rate, int channels, int kbps);
// the MP3 bytes that are ready (whole frames when the encoder gives them so): their number, -1 an error
int  mp3enc_encode(mp3enc *e, const int16_t *pcm, int frames, uint8_t *out, int cap);
void mp3enc_close(mp3enc *e);
const char *mp3enc_error(void);
