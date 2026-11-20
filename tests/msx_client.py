#!/usr/bin/env python3
# The MSX's side of the MSX link, for the tests: what PicoAmp on the MSX does
# with it (the stream with ICY titles, the cover, the keys).
#
#   msx_client.py <port> <seconds> [key ...]
#
# Waits for the link, plays the stream for <seconds> (checks its MP3 frames
# and prints its titles), sends the keys (/cmd?k=...) a second apart while it
# plays, then asks /now and /cover.jpg. Prints what it saw, line by line.
import socket, sys, time, json, urllib.request

PORT, SECS, KEYS = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3:]
BASE = 'http://127.0.0.1:%d' % PORT
SCREEN = '--screen' in KEYS
if SCREEN:
    KEYS.remove('--screen')

def get(path):
    try:
        with urllib.request.urlopen(BASE + path, timeout=5) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()

for _ in range(150):                      # (the link starts with the player)
    try:
        socket.create_connection(('127.0.0.1', PORT), timeout=1).close()
        break
    except OSError:
        time.sleep(0.1)

if SCREEN:
    # the MSX as the PC's screen and remote: no sound, its lines (S, T, V)
    s = socket.create_connection(('127.0.0.1', PORT), timeout=4)
    s.sendall(b'GET /screen HTTP/1.0\r\nHost: x\r\n\r\n')
    f = s.makefile('rb')
    st = f.readline().decode().strip()
    hdr = {}
    while True:
        l = f.readline().decode().strip()
        if not l:
            break
        k, v = l.split(':', 1)
        hdr[k.lower()] = v.strip()
    print('screen status:', st, '| x-picoamp:', hdr.get('x-picoamp'), '| type:', hdr.get('content-type'))
    t0 = time.time()
    nv = 0
    lit = 0
    words = []
    keys = list(KEYS)
    next_key = t0 + 1.0
    try:
        while time.time() - t0 < SECS:
            l = f.readline()
            if not l:
                break
            if l[:1] == b'V':
                nv += 1
                if len(l) == 92 and max(l[1:25]) > ord('0'):
                    lit += 1
            elif l[:1] == b'S':
                print('screen song:', l[1:].rstrip(b'\n').decode('latin-1').split('\t'))
            elif l[:1] == b'T':
                print('screen time:', l[1:].decode().strip())
            elif l[:1] == b'C':
                print('screen cover made:', l[1:].decode().strip(), '(fetched: %d bytes)' % len(get('/cover.jpg')[2]))
            elif l[:1] == b'F':
                print('screen format:', l[1:].decode('latin-1').strip())
            elif l[:1] == b'L':
                if l[1:2] == b'.':
                    print('screen words: %s, %d rows: %s' % (words[0] if words else '?', len(words) - 1, ' | '.join(words[1:])))
                else:
                    words = [l[1:2].decode()]
            elif l[:1] == b'R':
                ms, t = l[1:].rstrip(b'\n').split(b'\t', 1)
                words.append('%s %s' % (ms.decode(), t.decode('latin-1').encode('unicode_escape').decode()))
            if keys and time.time() >= next_key:
                k = keys.pop(0)
                print('key %s: %d' % (k, get('/cmd?k=' + k)[0]))
                next_key = time.time() + 1.0
    except OSError as e:
        print('screen ended:', e)
    print('screen bars: %d lines in %.0f s, %d with bars lit' % (nv, SECS, lit))
    sys.exit(0)

s = socket.create_connection(('127.0.0.1', PORT), timeout=4)
s.sendall(b'GET /stream.mp3 HTTP/1.0\r\nHost: x\r\nIcy-MetaData: 1\r\nUser-Agent: PicoAmp MSX test\r\n\r\n')
head = b''
while b'\r\n\r\n' not in head:
    head += s.recv(1)
lines = head.decode().split('\r\n')
print('status:', lines[0])
hdr = {l.split(':', 1)[0].lower(): l.split(':', 1)[1].strip() for l in lines[1:] if ':' in l}
for k in ('content-type', 'icy-metaint', 'x-picoamp', 'icy-br'):
    print('header %s: %s' % (k, hdr.get(k)))
metaint = int(hdr.get('icy-metaint', '0'))

audio = bytearray()
titles = []
t0 = time.time()
keys = list(KEYS)
next_key = t0 + 1.0

def recv_exact(n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return bytes(b)

try:
    while time.time() - t0 < SECS:
        audio += recv_exact(metaint)
        ln = recv_exact(1)[0] * 16
        if ln:
            m = recv_exact(ln).rstrip(b'\0').decode('utf-8', 'replace')
            titles.append(m)
            print('meta:', m)
        if keys and time.time() >= next_key:
            k = keys.pop(0)
            st, _, body = get('/cmd?k=' + k)
            print('key %s: %d %s' % (k, st, body.decode().strip()))
            next_key = time.time() + 1.0
except (EOFError, OSError) as e:
    print('stream ended:', repr(e))
    try:
        print('now then:', get('/now')[2].decode().strip())
    except OSError as e2:
        print('now then: no answer', e2)
s.close()

# the MP3 frames: a frame header at the start of a frame, again after each frame
BR = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
SR = [44100, 48000, 32000]
i, frames, rates = 0, 0, set()
while i < len(audio) - 4 and not (audio[i] == 0xFF and (audio[i + 1] & 0xFE) == 0xFA):
    i += 1
while i + 4 <= len(audio):
    h = audio[i:i + 4]
    if h[0] != 0xFF or (h[1] & 0xFE) != 0xFA:
        break
    br, sr, pad = BR[h[2] >> 4], SR[(h[2] >> 2) & 3], (h[2] >> 1) & 1
    rates.add((br, sr))
    i += 144000 * br // sr + pad
    frames += 1
print('audio bytes: %d, frames in a row: %d, kbps/rate: %s' % (len(audio), frames, sorted(rates)))
try:
    st, h, body = get('/now')
except OSError as e:
    print('the link is gone:', e)
    sys.exit(0)
print('now: %d %s' % (st, body.decode().strip()))
st, h, body = get('/cover.jpg')
print('cover: %d %d bytes jpeg=%s gen=%s' % (st, len(body), body[:2] == b'\xff\xd8', h.get('X-Cover-Gen')))
if st == 200:
    try:
        open('msx_cover.jpg', 'wb').write(body)
    except OSError:
        pass
def jpeg_size(b):
    i = 2
    while i + 9 < len(b):
        if b[i] != 0xFF:
            return None
        m, ln = b[i + 1], b[i + 2] << 8 | b[i + 3]
        if m in (0xC0, 0xC1, 0xC2):
            return '%dx%d%s' % (b[i + 7] << 8 | b[i + 8], b[i + 5] << 8 | b[i + 6], ' progressive' if m == 0xC2 else '')
        i += 2 + ln
    return None
st, h, body = get('/cover.jpg')
print('cover size:', jpeg_size(body) if st == 200 else None)
st, h, body = get('/cover.jpg?w=281&h=192')
print('big cover: %d %d bytes, %s' % (st, len(body), jpeg_size(body) if st == 200 else None))
st, h, body = get('/picoamp.m3u')
print('m3u: %d %s' % (st, body.decode().replace('\r\n', ' | ')))
