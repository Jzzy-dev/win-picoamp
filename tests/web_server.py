#!/usr/bin/env python3
# A stand-in for the web services PicoAmp uses, for the tests (no internet):
#   the Radio Browser directory    /json/countries, /json/stations/search, /json/url/<uuid>
#   ip-api.com                     /ipapi/json
#   Open-Meteo                     /meteo/v1/forecast, /geo/v1/search
#   news feeds                     /feeds/rss.xml, /feeds/atom.xml, /feeds/rdf.xml, /articles/<n>.html
#
#   LRCLIB (the lyrics)            /lrclib/api/get, /lrclib/api/search
#   station logos                  /json/stations/byuuid/<uuid>, /json/stations/byurl, /logos/1.jpg
#
#   web_server.py <port> <stream url for the stations> [logo.jpg]
import http.server, json, sys, urllib.parse, time, threading

PORT, STREAM = int(sys.argv[1]), sys.argv[2]
LOGO = sys.argv[3] if len(sys.argv) > 3 else None

def station(i, name=None, codec='MP3', hls=0):
    return {"stationuuid": "uuid-%d" % i, "name": name or ("Station %d" % i), "url": STREAM,
            "url_resolved": STREAM, "codec": codec, "bitrate": 128, "hls": hls, "lastcheckok": 1,
            "countrycode": "NL", "tags": "jazz,pop"}

clicks = []
LRC = ("[ar:Unit Tester]\n[00:00.50] First line\n[00:02.00] Second line, a rather long one that does not fit on one row of the page at all\n"
       "[00:04.00][00:12.00] Café line, twice\n[00:06.00]\n[00:08.00] Last line")

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, body, ctype='application/json'):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def reply_code(self, code, body):
        b = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        p = u.path
        if p == '/json/countries':
            self.reply(json.dumps([{"name": "Germany", "iso_3166_1": "DE", "stationcount": 5000},
                                   {"name": "Netherlands", "iso_3166_1": "NL", "stationcount": 1500},
                                   {"name": "Nowhere", "iso_3166_1": "XX", "stationcount": 0}]))
        elif p == '/json/stations/search':
            limit, off = int(q.get('limit', 60)), int(q.get('offset', 0))
            if 'name' in q:
                n = q['name'].lower()
                v = [station(1, "Jazz FM"), station(2, "Smooth Jazz 24"), station(3, "Jazz AAC", "AAC"), station(4, "Jazz HLS", hls=1)]
                v = [s for s in v if n in s['name'].lower()]
            elif q.get('tag') == 'jazz':
                v = [station(i, "Jazz %d" % i) for i in range(100)][off:off + limit]
            elif q.get('countrycode') == 'NL':
                v = [station(10, "NPO Radio 1"), station(11, "Radio 538")]
            else:
                v = [station(i) for i in range(5)] + [station(9, "An AAC one", "AAC")]
            self.reply(json.dumps(v))
        elif p.startswith('/json/stations/byuuid/'):
            u = p[22:]
            print("byuuid", u, flush=True)
            s = station(int(u[5:]) if u.startswith('uuid-') else 99)
            if u == 'uuid-1':
                s['favicon'] = 'http:\\/\\/127.0.0.1:%d\\/logos\\/1.jpg' % PORT
            self.reply(json.dumps([s]).replace('\\\\/', '\\/'))
        elif p == '/json/stations/byurl':
            print("byurl", q.get('url'), flush=True)
            self.reply('[]')
        elif p == '/logos/1.jpg' and LOGO:
            print("logo", flush=True)
            self.reply(open(LOGO, 'rb').read(), 'image/jpeg')
        elif p == '/itunes/search':
            t = q.get('term', '')
            print("itunes", t, flush=True)
            if t.startswith('Artist One') or t.startswith('Unit Tester'):
                self.reply(json.dumps({"resultCount": 1, "results": [{"artistName": "x",
                           "artworkUrl100": "http://127.0.0.1:%d/art/100x100bb.jpg" % PORT}]}))
            else:
                self.reply('{"resultCount":0,"results":[]}')
        elif p == '/lrclib/api/get':
            print("lrclib get", q.get('artist_name'), q.get('track_name'), q.get('album_name'), q.get('duration'), flush=True)
            if q.get('track_name') == 'Sine Café Song' and q.get('duration') == '30':
                self.reply(json.dumps({"id": 1, "trackName": "Sine Café Song", "artistName": "Unit Tester", "duration": 30,
                                       "instrumental": False, "plainLyrics": "First line\nSecond line",
                                       "syncedLyrics": LRC}))
            else:
                self.reply_code(404, '{"code":404,"name":"TrackNotFound","message":"Failed to find specified track"}')
        elif p == '/lrclib/api/search':
            t = q.get('track_name') or q.get('q', '')
            print("lrclib search", q.get('artist_name'), t, flush=True)
            if t == 'Fake Song':
                # another version (plain, the wrong length) and the right one (synced)
                self.reply(json.dumps([{"id": 2, "duration": 999, "instrumental": False, "plainLyrics": "Wrong version", "syncedLyrics": None},
                                       {"id": 3, "duration": 200, "instrumental": False, "plainLyrics": "Fake one",
                                        "syncedLyrics": "[00:00.00] Fake one \u00e9\n[00:05.00] Fake two\n[01:00.00] Fake three"}]))
            elif t == 'Fake Song 2':
                self.reply(json.dumps([{"id": 4, "duration": 200, "instrumental": True, "plainLyrics": None, "syncedLyrics": None}]))
            else:
                self.reply('[]')
        elif p.startswith('/art/') and LOGO:
            print("art", p, flush=True)
            self.reply(open(LOGO, 'rb').read(), 'image/jpeg')
        elif p.startswith('/json/url/'):
            clicks.append(p[10:])
            print("click", p[10:], flush=True)
            self.reply(json.dumps({"ok": True}))
        elif p in ('/ipapi/json', '/ipapi/json/'):
            self.reply(json.dumps({"status": "success", "city": "Utrecht", "regionName": "Utrecht", "countryCode": "NL",
                                   "lat": 52.09, "lon": 5.12, "timezone": "Europe/Amsterdam"}))
        elif p == '/geo/v1/search':
            self.reply(json.dumps({"results": [
                {"name": "Sydney", "admin1": "New South Wales", "country_code": "AU", "latitude": -33.87, "longitude": 151.21, "timezone": "Australia/Sydney"},
                {"name": "Sydney", "admin1": "Nova Scotia", "country_code": "CA", "latitude": 46.15, "longitude": -60.18, "timezone": "America/Glace_Bay"}]}))
        elif p == '/meteo/v1/forecast':
            self.reply(json.dumps({
                "utc_offset_seconds": 3600, "timezone": "Europe/Amsterdam",
                "current": {"time": "2026-11-05T18:00", "temperature_2m": 8.4, "apparent_temperature": 5.9,
                            "relative_humidity_2m": 87, "wind_speed_10m": 15.2, "weather_code": 61, "is_day": 0},
                "daily": {"time": ["2026-11-05", "2026-11-06", "2026-11-07", "2026-11-08", "2026-11-09", "2026-11-10"],
                          "weather_code": [61, 3, 0, 71, 95, 2],
                          "temperature_2m_max": [10.2, 11.0, 12.5, 3.1, 15.0, 9.9],
                          "temperature_2m_min": [4.1, 3.0, 4.4, -6.2, 10.0, 2.2],
                          "precipitation_probability_max": [80, 20, 0, 65, 100, 10]}}))
        elif p == '/v8/finance/spark':                     # Yahoo Finance's chart service (as it answered in November 2026)
            syms = q.get('symbols', '').split(',')
            rng, out = q.get('range', '1d'), {}
            for k, sy in enumerate(syms):
                h = sum(ord(c) for c in sy)
                price = 100 + h % 900 + (h % 97) / 100.0
                prev = round(price * (1 - ((h % 7) - 3) / 100.0), 2)
                n = {'1d': 1, '5d': 46, '1mo': 24, '6mo': 133, 'ytd': 223, '1y': 256, '5y': 262, 'max': 410}.get(rng, 1)
                if q.get('interval') == '15m': n = 34
                step = {'15m': 900, '60m': 3600, '1wk': 604800, '1mo': 2629800}.get(q.get('interval'), 86400)
                end = 1794587400
                ts = [end - (n - 1 - i) * step for i in range(n)] if n > 1 else [1794572130]
                cl = [round(prev + (price - prev) * i / max(n - 1, 1) + (2 if (i // 4) % 2 else -2), 2) for i in range(n)] if n > 1 else [price]
                cl[-1] = price
                if n > 3: cl[2] = None
                out[sy] = {"timestamp": ts, "symbol": sy, "end": end if q.get('interval') == '15m' else None,
                           "start": 1794556800 if q.get('interval') == '15m' else None, "close": cl, "previousClose": None,
                           "chartPreviousClose": prev, "fulldayChange": round(price - prev, 2), "fulldayPrice": price, "dataGranularity": 300}
                if sy == 'NOPE.XX': del out[sy]
            self.reply(json.dumps(out))
        elif p == '/rss/2.0/headline':                     # Yahoo Finance's feed for symbols
            s = q.get('s', '')
            items = ''.join('<item><title>%s story %d &amp; more</title><link>http://localhost:%d/articles/%d.html</link>'
                            '<description>About %s.</description><pubDate>Fri, 13 Nov 2026 1%d:00:00 +0000</pubDate></item>' % (s, i, PORT, i, s, i) for i in range(6))
            self.reply('<?xml version="1.0" encoding="UTF-8"?><rss version="2.0"><channel><title>Yahoo! Finance: %s News</title>%s</channel></rss>' % (s, items), 'application/xml')
        elif p == '/feeds/rss.xml':
            items = ''.join('<item><title>Story %d &amp; more</title><link>http://localhost:%d/articles/%d.html</link>'
                            '<description>&lt;p&gt;Summary of story %d, with some words to wrap over the lines of the screen.&lt;/p&gt;</description>'
                            '<pubDate>Thu, 05 Nov 2026 %02d:%02d:00 GMT</pubDate></item>' % (i, PORT, i, i, 8 + i // 6, (i * 7) % 60) for i in range(30))
            self.reply('<?xml version="1.0"?><rss version="2.0"><channel><title>Test News</title>%s</channel></rss>' % items, 'application/rss+xml')
        elif p == '/feeds/atom.xml':
            self.reply('<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><title>Atom Feed</title>'
                       '<entry><title>An atom story</title><link href="http://localhost:%d/articles/1.html"/>'
                       '<updated>2026-11-05T09:30:00Z</updated><summary>Atom summary.</summary></entry></feed>' % PORT, 'application/atom+xml')
        elif p.startswith('/articles/'):
            n = p.split('/')[-1].split('.')[0]
            paras = ''.join('<p>Paragraph %d of the article %s. It has a few sentences, enough to fill a line or two of the reader.</p>' % (k, n) for k in range(12))
            self.reply('<html><head><title>x</title><script>var a=1;</script></head><body><nav>Menu Home News</nav>'
                       '<article><h1>Article %s</h1>%s</article><footer>Footer text</footer></body></html>' % (n, paras), 'text/html')
        else:
            self.send_response(404)
            self.end_headers()

http.server.ThreadingHTTPServer.allow_reuse_address = True
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
