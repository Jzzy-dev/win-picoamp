#!/usr/bin/env python3
# A small stand-in for usdb.animux.de (plain HTTP), for the tests of the
# MSX link's /usdb/ bridge (PicoSing on the MSX):
#   POST index.php?link=login  user=jzzy, pass=s3cret&x -> a PHPSESSID cookie (after a redirect)
#   POST index.php?link=list   one row (logged in only)
#   POST index.php?link=gettxt&id=7 with wd=1 -> a <textarea> with a song
#   GET  data/cover/7.jpg      a few JPEG bytes
#   usdb_fake.py <port>
import sys, urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        sys.stderr.write('usdb: %s %s\n' % (self.command, self.path))
    def send(self, code, body, ctype='text/html', extra=()):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(b)
    def logged(self):
        return 'PHPSESSID=s1' in (self.headers.get('Cookie') or '')
    def do_GET(self):
        if self.path == '/index.php?link=home&in=1':
            self.send(200, '<html>Welcome jzzy</html>')
        elif self.path == '/data/cover/7.jpg':
            self.send(200, b'\xff\xd8\xff\xe0fakejpeg\xff\xd9', 'image/jpeg')
        else:
            self.send(404, 'no')
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        form = urllib.parse.parse_qs(self.rfile.read(n).decode())
        sys.stderr.write('usdb: form %s\n' % form)
        if self.path == '/index.php?link=login':
            if form.get('user') == ['jzzy'] and form.get('pass') == ['s3cret&x']:
                self.send(302, '', extra=[('Set-Cookie', 'PHPSESSID=s1; path=/'), ('Location', '/index.php?link=home&in=1')])
            else:
                self.send(200, '<html>Login or Password invalid, please try again.</html>')
        elif not self.logged():
            self.send(200, '<html>You are not logged in. Login to use this function.</html>')
        elif self.path == '/index.php?link=list':
            who = form.get('interpret', [''])[0]
            self.send(200, '<table><tr class="list_tr1" data-songid="7" data-lastchange="1"><td></td><td><img src="x"></td>'
                           '<td>%s</td>\n<td><a href="?">Song Seven</a></td>\n</tr></table>' % who)
        elif self.path == '/index.php?link=gettxt&id=7' and form.get('wd') == ['1']:
            self.send(200, '<textarea>#TITLE:Song Seven\n#ARTIST:Seven &amp; Co\n: 0 1 0 la\nE\n</textarea>')
        else:
            self.send(404, 'no')

HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
