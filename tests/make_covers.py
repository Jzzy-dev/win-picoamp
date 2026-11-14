#!/usr/bin/env python3
# make_covers.py <dir>: a folder with a short WAV and a cover.png next to it, for the
# check of covers that only Windows's own decoder (WIC) reads (the CI's Windows job)
import os, struct, sys, wave, zlib, math

d = os.path.join(sys.argv[1], 'Png')
os.makedirs(d, exist_ok=True)
with wave.open(os.path.join(d, 'Tone.wav'), 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    w.writeframes(b''.join(struct.pack('<hh', int(8000 * math.sin(i / 20)), int(8000 * math.sin(i / 20)))
                           for i in range(44100 * 6)))
W = H = 240
rows = []
for y in range(H):
    r = bytearray([0])
    for x in range(W):
        inside = (x - 120) ** 2 + (y - 120) ** 2 < 80 ** 2
        r += bytes((255, 200, 0) if inside else (x, 40, 255 - y))
    rows.append(bytes(r))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
open(os.path.join(d, 'cover.png'), 'wb').write(png)
print('made', d)
