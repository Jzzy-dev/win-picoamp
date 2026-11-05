#!/usr/bin/env python3
# A small internet radio for the tests: serves an MP3 file round and round as
# an ICY stream (metaint 8192) with changing titles, at its bit rate.
#
#   icy_server.py <port> <file.mp3> [kbit/s]
#
#   /stream.mp3    HTTP/1.0 200, icy-metaint, icy-name "Test FM"
#   /icy           "ICY 200 OK" (old SHOUTcast)
#   /plain         no metadata
#   /list.pls      a .pls pointing at /stream.mp3
#   /redirect      302 to /stream.mp3
#   /aac           Content-Type audio/aacp
import socket, sys, threading, time

port, path = int(sys.argv[1]), sys.argv[2]
kbps = int(sys.argv[3]) if len(sys.argv) > 3 else 192
data = open(path, 'rb').read()
METAINT = 8192
TITLES = ["Artist One - Song A", "Artist Two - Song B with a title long enough to scroll on the screen", "Café Ünïcode - Ça va"]

def meta_block(title):
    m = ("StreamTitle='%s';StreamUrl='';" % title).encode('utf-8')
    n = (len(m) + 15) // 16
    return bytes([n]) + m.ljust(n * 16, b'\0')

def serve(c):
    try:
        req = b''
        while b'\r\n\r\n' not in req:
            d = c.recv(4096)
            if not d: return
            req += d
        line = req.split(b'\r\n')[0].decode()
        p = line.split(' ')[1]
        want_meta = b'icy-metadata: 1' in req.lower()
        host = 'localhost:%d' % port
        if p == '/list.pls':
            body = ('[playlist]\nNumberOfEntries=1\nFile1=http://%s/stream.mp3\nTitle1=Test FM\nLength1=-1\nVersion=2\n' % host).encode()
            c.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: audio/x-scpls\r\n\r\n' + body)
            return
        if p == '/redirect':
            c.sendall(b'HTTP/1.0 302 Found\r\nLocation: /stream.mp3\r\n\r\n')
            return
        if p == '/aac':
            c.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: audio/aacp\r\n\r\n' + b'\0' * 1000)
            return
        if p not in ('/stream.mp3', '/icy', '/plain'):
            c.sendall(b'HTTP/1.0 404 Not Found\r\n\r\n')
            return
        meta = want_meta and p != '/plain'
        status = b'ICY 200 OK\r\n' if p == '/icy' else b'HTTP/1.0 200 OK\r\n'
        head = status + b'Content-Type: audio/mpeg\r\nicy-name: Test FM\r\nicy-br: %d\r\n' % kbps
        if meta: head += b'icy-metaint: %d\r\n' % METAINT
        c.sendall(head + b'\r\n')
        pos, sent, t0, ti = 0, 0, time.time(), 0
        burst = kbps * 125 * 4          # 4 s at once, then real time
        while True:
            chunk = data[pos:pos + METAINT]
            if len(chunk) < METAINT:
                chunk += data[:METAINT - len(chunk)]
                pos = METAINT - (len(chunk) - (len(data) - pos)) if False else (METAINT - len(data[pos:]))
            else:
                pos += METAINT
            pos %= len(data)
            c.sendall(chunk)
            if meta:
                c.sendall(meta_block(TITLES[(sent // (kbps * 125 * 6)) % len(TITLES)]))
            sent += len(chunk)
            ahead = sent - burst - (time.time() - t0) * kbps * 125
            if ahead > 0: time.sleep(ahead / (kbps * 125))
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    finally:
        c.close()

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(8)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
