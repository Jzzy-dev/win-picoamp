// mp3enc_host.c - the test build's MP3 "encoder": MPEG-1 layer III frames of
// silence (no side information, no main data), as many as the PCM lasts.
#include <stdlib.h>
#include <string.h>
#include "mp3enc.h"

struct mp3enc { int rate, ch, kbps, have; long frac; };

static const int br_index[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };

mp3enc *mp3enc_open(int rate, int channels, int kbps)
{
    if (rate != 44100 && rate != 48000 && rate != 32000) return NULL;
    mp3enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->rate = rate;
    e->ch = channels;
    e->kbps = kbps;
    return e;
}

int mp3enc_encode(mp3enc *e, const int16_t *pcm, int frames, uint8_t *out, int cap)
{
    (void)pcm;
    int n = 0, bi = 0;
    for (int i = 1; i < 15; i++) if (br_index[i] == e->kbps) bi = i;
    if (!bi) return -1;
    int sri = e->rate == 44100 ? 0 : e->rate == 48000 ? 1 : 2;
    e->have += frames;
    while (e->have >= 1152)
    {
        // 144 * bitrate / rate bytes, the fraction carried by the padding bit
        long num = 144L * e->kbps * 1000;
        int len = (int)(num / e->rate);
        e->frac += num % e->rate;
        int pad = 0;
        if (e->frac >= e->rate) { e->frac -= e->rate; pad = 1; }
        len += pad;
        if (n + len > cap) break;
        uint8_t *f = out + n;
        memset(f, 0, (size_t)len);
        f[0] = 0xFF;
        f[1] = 0xFB;                                   // MPEG-1, layer III, no CRC
        f[2] = (uint8_t)(bi << 4 | sri << 2 | pad << 1);
        f[3] = e->ch == 1 ? 0xC4 : 0x04;               // (stereo / mono), original
        n += len;
        e->have -= 1152;
    }
    return n;
}

void mp3enc_close(mp3enc *e) { free(e); }
const char *mp3enc_error(void) { return ""; }
