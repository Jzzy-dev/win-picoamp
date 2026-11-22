#!/usr/bin/env python3
"""Pretend Sonos speakers for the tests: an SSDP answer on a UDP port and the
UPnP control (SOAP) of three speakers on three TCP ports.

    sonos_fake.py <ssdp port> <first http port> <cover.jpg>

Rooms: "Living Room" (one speaker, RINCON_A, plays a queue of songs) and
"Kitchen" (a group of two, RINCON_B + RINCON_C, plays a station).
GET /log on the first port says what the speakers were told (one line each).
"""
import html
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SSDP_PORT = int(sys.argv[1])
BASE = int(sys.argv[2])
COVER = open(sys.argv[3], 'rb').read()
LOG = []

class Dev:
    def __init__(self, uuid, name, port, radio=False):
        self.uuid, self.name, self.port, self.radio = uuid, name, port, radio
        self.state = 'PLAYING'
        self.track = 0
        self.t0 = time.time()
        self.paused_at = 0.0
        self.volume = 30
        self.mute = 0
        self.gvolume = 40
        self.gmute = 0
        self.mode = 'NORMAL'

    def elapsed(self):
        e = time.time() - self.t0 if self.state == 'PLAYING' else self.paused_at
        return min(int(e), 210)

DEVS = {
    BASE: Dev('RINCON_A', 'Living Room', BASE),
    BASE + 1: Dev('RINCON_B', 'Kitchen', BASE + 1, radio=True),
    BASE + 2: Dev('RINCON_C', 'Kitchen', BASE + 2, radio=True),
}

def hms(s):
    return '%d:%02d:%02d' % (s // 3600, s // 60 % 60, s % 60)

def didl(title, creator, album, art, extra=''):
    return ('<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" '
            'xmlns:r="urn:schemas-rinconnetworks-com:metadata-1-0/" xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/">'
            '<item id="-1" parentID="-1" restricted="true"><res protocolInfo="sonos.com-http:*:audio/mpeg:*">x</res>'
            + ('<upnp:albumArtURI>%s</upnp:albumArtURI>' % html.escape(art) if art else '')
            + '<dc:title>%s</dc:title>' % html.escape(title)
            + ('<dc:creator>%s</dc:creator>' % html.escape(creator) if creator else '')
            + ('<upnp:album>%s</upnp:album>' % html.escape(album) if album else '')
            + extra + '</item></DIDL-Lite>')

def topology():
    port = lambda p: 'http://127.0.0.1:%d/xml/device_description.xml' % p
    z = ('<ZoneGroupState><ZoneGroups>'
         '<ZoneGroup Coordinator="RINCON_A" ID="RINCON_A:1"><ZoneGroupMember UUID="RINCON_A" Location="%s" ZoneName="Living Room"/>'
         '<ZoneGroupMember UUID="RINCON_S" Location="%s" ZoneName="Living Room" Invisible="1"/></ZoneGroup>'
         '<ZoneGroup Coordinator="RINCON_B" ID="RINCON_B:2"><ZoneGroupMember UUID="RINCON_B" Location="%s" ZoneName="Kitchen">'
         '<Satellite UUID="RINCON_X" Location="%s" ZoneName="Kitchen" Invisible="1"/></ZoneGroupMember>'
         '<ZoneGroupMember UUID="RINCON_C" Location="%s" ZoneName="Dining Room"/></ZoneGroup>'
         '<ZoneGroup Coordinator="RINCON_Z" ID="RINCON_Z:3"><ZoneGroupMember UUID="RINCON_Z" Location="%s" ZoneName="Boost" IsZoneBridge="1"/></ZoneGroup>'
         '</ZoneGroups><VanishedDevices></VanishedDevices></ZoneGroupState>') % (
             port(BASE), port(BASE), port(BASE + 1), port(BASE + 1), port(BASE + 2), port(BASE + 9))
    return '<ZoneGroupState>%s</ZoneGroupState>' % html.escape(z)

def arg(body, name):
    m = re.search(r'<%s>(.*?)</%s>' % (name, name), body, re.S)
    return m.group(1) if m else ''

def answer(dev, action, body):
    if action == 'GetZoneGroupState':
        return topology()
    if dev.uuid == 'RINCON_C' and action == 'GetPositionInfo':
        return ('<Track>1</Track><TrackDuration>0:00:00</TrackDuration><TrackMetaData></TrackMetaData>'
                '<TrackURI>x-rincon:RINCON_B</TrackURI><RelTime>0:00:00</RelTime>')
    if action == 'GetTransportInfo':
        return '<CurrentTransportState>%s</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>' % dev.state
    if action == 'GetPositionInfo':
        if dev.radio:
            m = didl('x-sonosapi-stream:s1234', '', '', '', '<r:streamContent>Radio Artist - Radio Song &amp; Co</r:streamContent>')
            return ('<Track>1</Track><TrackDuration>0:00:00</TrackDuration><TrackMetaData>%s</TrackMetaData>'
                    '<TrackURI>x-sonosapi-stream:s1234?sid=254</TrackURI><RelTime>0:01:00</RelTime>') % html.escape(m)
        t = 'Fake Song' + ('' if dev.track == 0 else ' %d' % (dev.track + 1))
        m = didl(t, 'Fake Artist', 'Fake Album', '/getaa?s=1&u=x-sonos-spotify%%3a%d' % dev.track)
        return ('<Track>%d</Track><TrackDuration>0:03:30</TrackDuration><TrackMetaData>%s</TrackMetaData>'
                '<TrackURI>x-sonos-spotify:track%d</TrackURI><RelTime>%s</RelTime>') % (dev.track + 1, html.escape(m), dev.track, hms(dev.elapsed()))
    if action == 'GetMediaInfo':
        m = didl('Radio Fake', '', '', '') if dev.radio else ''
        return '<NrTracks>1</NrTracks><CurrentURI>x</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>' % html.escape(m)
    if action == 'GetTransportSettings':
        return '<PlayMode>%s</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>' % dev.mode
    if action == 'GetVolume':
        return '<CurrentVolume>%d</CurrentVolume>' % dev.volume
    if action == 'GetMute':
        return '<CurrentMute>%d</CurrentMute>' % dev.mute
    if action == 'GetGroupVolume':
        return '<CurrentVolume>%d</CurrentVolume>' % dev.gvolume
    if action == 'GetGroupMute':
        return '<CurrentMute>%d</CurrentMute>' % dev.gmute
    LOG.append('%s %s %s' % (dev.name, action, re.sub(r'<InstanceID>0</InstanceID>', '', body and re.sub(r'.*<u:%s[^>]*>(.*)</u:%s>.*' % (action, action), r'\1', body, flags=re.S) or '')))
    if action == 'Play':
        if dev.state != 'PLAYING':
            dev.t0 = time.time() - dev.paused_at
        dev.state = 'PLAYING'
    elif action in ('Pause', 'Stop'):
        if dev.radio and action == 'Pause':
            return None                     # (a station cannot pause: it stops)
        if dev.state == 'PLAYING':
            dev.paused_at = time.time() - dev.t0
        dev.state = 'PAUSED_PLAYBACK' if action == 'Pause' else 'STOPPED'
    elif action == 'Next':
        dev.track += 1; dev.t0 = time.time(); dev.paused_at = 0
    elif action == 'Previous':
        dev.track = max(0, dev.track - 1); dev.t0 = time.time(); dev.paused_at = 0
    elif action == 'Seek':
        h, m, s = map(int, arg(body, 'Target').split(':'))
        e = h * 3600 + m * 60 + s
        dev.t0 = time.time() - e; dev.paused_at = e
    elif action == 'SetPlayMode':
        dev.mode = arg(body, 'NewPlayMode')
    elif action == 'SetVolume':
        dev.volume = int(arg(body, 'DesiredVolume'))
    elif action == 'SetMute':
        dev.mute = int(arg(body, 'DesiredMute'))
    elif action == 'SetGroupVolume':
        dev.gvolume = int(arg(body, 'DesiredVolume'))
    elif action == 'SetGroupMute':
        dev.gmute = int(arg(body, 'DesiredMute'))
    else:
        return None
    return ''

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype='text/xml; charset="utf-8"'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith('/getaa'):
            return self.send(200, COVER, 'image/jpeg')
        if self.path == '/log':
            return self.send(200, ('\n'.join(LOG) + '\n').encode(), 'text/plain')
        self.send(404, b'')

    def do_POST(self):
        dev = DEVS[self.server.server_address[1]]
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        sa = self.headers.get('SOAPACTION', '').strip('"')
        action = sa.split('#')[-1]
        a = answer(dev, action, body)
        if a is None:
            return self.send(500, b'<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>701</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>')
        out = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
               's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:%sResponse xmlns:u="%s">%s</u:%sResponse>'
               '</s:Body></s:Envelope>') % (action, sa.split('#')[0], a, action)
        self.send(200, out.encode())

def ssdp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', SSDP_PORT))
    while True:
        d, addr = s.recvfrom(2048)
        if b'M-SEARCH' in d and b'ZonePlayer' in d:
            for p in (BASE, BASE + 1):
                s.sendto(('HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age = 1800\r\nEXT:\r\n'
                          'LOCATION: http://127.0.0.1:%d/xml/device_description.xml\r\nSERVER: Linux UPnP/1.0 Sonos/80.1\r\n'
                          'ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\nUSN: uuid:RINCON_A::urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n' % p).encode(), addr)

threading.Thread(target=ssdp, daemon=True).start()
for p in DEVS:
    srv = ThreadingHTTPServer(('127.0.0.1', p), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
print('sonos_fake: ssdp %d, speakers %d..%d' % (SSDP_PORT, BASE, BASE + 2), flush=True)
while True:
    time.sleep(3600)
