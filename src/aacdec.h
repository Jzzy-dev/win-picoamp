// aacdec.h - AAC and HE-AAC (AAC+) for internet radio: Windows' own decoder
// (Media Foundation's AAC decoder), fed with ADTS frames. Not in the POSIX
// test build: aacdec_open returns NULL there.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct aacdec aacdec;

bool    aacdec_available(void);
aacdec *aacdec_open(int rate, int channels);        // from the first ADTS header
// one ADTS frame (with its header) in; PCM (16-bit, interleaved) out:
// the frames written (0: none yet, -1: an error); *rate, *ch: the PCM's
int     aacdec_decode(aacdec *d, const uint8_t *frame, size_t len, int16_t *out, int max_frames, int *rate, int *ch);
void    aacdec_close(aacdec *d);

// an ADTS header at p (7 bytes at least): the frame's length, rate, channels (0: none)
size_t  adts_frame(const uint8_t *p, int *rate, int *channels);
