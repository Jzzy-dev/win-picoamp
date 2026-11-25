#!/usr/bin/env python3
# PicoSing's side of the MSX link, for the tests: /sing/open and the /usdb/ bridge.
#   sing_client.py <port>
import socket, sys, time, urllib.request
BASE = 'http://127.0.0.1:%d' % int(sys.argv[1])
def get(path):
    try:
        with urllib.request.urlopen(BASE + path, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
for _ in range(150):
    try:
        socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1).close()
        break
    except OSError:
        time.sleep(0.1)
time.sleep(0.5)
for p in ['/sing/open?yt=dQw4w9WgXcQ', '/sing/open?yt=bad', '/sing/open?q=ABBA+Dancing+Queen',
          '/usdb/list?interpret=nobody', '/usdb/login?u=jzzy&p=wrong', '/usdb/login?u=jzzy&p=s3cret%26x',
          '/usdb/list?interpret=ABBA&title=&start=0&limit=20', '/usdb/txt?id=7', '/usdb/cover?id=7']:
    st, b = get(p)
    print('%s -> %d %r' % (p, st, b[:160]))
    sys.stdout.flush()
