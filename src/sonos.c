// sonos.c - Sonos speakers in this network (see sonos.h).
//
// The search: an SSDP M-SEARCH for ZonePlayers (UDP to 239.255.255.250:1900),
// the speakers answer with their address; any one of them tells all the rooms
// (ZoneGroupTopology GetZoneGroupState). sonos_ip in picoamp.ini adds speakers
// to ask when the network keeps the search from them.
//
// A room is asked with SOAP over plain HTTP (port 1400): AVTransport for the
// song, its time, the state and the play mode; RenderingControl (one speaker)
// or GroupRenderingControl (a group) for the volume and mute. Commands go to the
// coordinator, as the Sonos apps send them.
//
// The test build: PICOAMP_SSDP=host:port sends the search there (a pretend
// speaker, tests/sonos_fake.py), as plain unicast.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sonos.h"
#include "config.h"
#include "net.h"
#include "sys.h"
#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define MAX_ROOMS     32
#define MAX_CAND      16
#define SOAP_MAX      (256 * 1024)
#define ART_MAX       (4 * 1024 * 1024)
#define POLL_MS       1000
#define CONNECT_MS    1500
#define READ_MS       4000

// ---------------------------------------------------------------- sockets

static void sock_timeout(sock_t s, int ms)
{
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static void sock_block(sock_t s, bool on)
{
#ifdef _WIN32
    u_long nb = on ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

// a TCP connection to ip:port, given up after ms
static sock_t tcp_connect(const char *ip, int port, int ms)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(ip, ps, &hints, &res) || !res) return BAD_SOCK;
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == BAD_SOCK) { freeaddrinfo(res); return BAD_SOCK; }
    sock_block(s, false);
    int r = connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    if (r != 0)
    {
        fd_set w, e;
        FD_ZERO(&w); FD_SET(s, &w);
        FD_ZERO(&e); FD_SET(s, &e);
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        if (select((int)s + 1, NULL, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesock(s); return BAD_SOCK; }
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) { closesock(s); return BAD_SOCK; }
    }
    sock_block(s, true);
    sock_timeout(s, READ_MS);
    return s;
}

static bool send_all(sock_t s, const char *p, size_t n)
{
    while (n)
    {
#ifdef MSG_NOSIGNAL
        int r = send(s, p, (int)(n > 65536 ? 65536 : n), MSG_NOSIGNAL);
#else
        int r = send(s, p, (int)(n > 65536 ? 65536 : n), 0);
#endif
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

// an HTTP request to a speaker (or any plain http host): the status, and the
// body in *out (malloc'd, 0-terminated); 0 when it did not answer
static int http_do(const char *ip, int port, const char *method, const char *path,
                   const char *soap_action, const char *body, char **out, size_t *outlen, size_t max)
{
    if (out) *out = NULL;
    if (outlen) *outlen = 0;
    sock_t s = tcp_connect(ip, port, CONNECT_MS);
    if (s == BAD_SOCK) return 0;
    size_t bl = body ? strlen(body) : 0;
    char head[1024];
    int hn;
    if (soap_action)
        hn = snprintf(head, sizeof head,
                      "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: PicoAmp\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                      "SOAPACTION: \"%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                      method, path, ip, port, soap_action, (unsigned)bl);
    else
        hn = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: PicoAmp\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                      method, path, ip, port);
    if (hn <= 0 || (size_t)hn >= sizeof head || !send_all(s, head, (size_t)hn) || (bl && !send_all(s, body, bl))) { closesock(s); return 0; }
    size_t cap = 8192, n = 0;
    char *b = malloc(cap + 1);
    for (;;)
    {
        if (n == cap)
        {
            if (cap >= max + 16384) break;
            cap *= 2;
            char *nb = realloc(b, cap + 1);
            if (!nb) break;
            b = nb;
        }
        int r = recv(s, b + n, (int)(cap - n), 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    closesock(s);
    b[n] = 0;
    char *eoh = strstr(b, "\r\n\r\n");
    if (!eoh || strncmp(b, "HTTP/", 5)) { free(b); return 0; }
    const char *sp = strchr(b, ' ');
    int status = sp ? atoi(sp + 1) : 0;
    *eoh = 0;
    bool chunked = false;
    long clen = -1;
    for (char *l = strstr(b, "\r\n"); l && *l; )
    {
        l += 2;
        if (!u_strnicmp(l, "Content-Length:", 15)) clen = atol(l + 15);
        else if (!u_strnicmp(l, "Transfer-Encoding:", 18) && strstr(l, "hunked")) chunked = true;
        l = strstr(l, "\r\n");
    }
    char *p = eoh + 4;
    size_t len = n - (size_t)(p - b);
    if (chunked)
    {
        // the chunks put together, in place
        char *w = p, *r = p, *end = p + len;
        while (r < end)
        {
            char *e = strstr(r, "\r\n");
            if (!e) break;
            unsigned long k = strtoul(r, NULL, 16);
            r = e + 2;
            if (!k || r + k > end) { if (r + k > end) { memmove(w, r, (size_t)(end - r)); w += end - r; } break; }
            memmove(w, r, k);
            w += k;
            r += k + 2;
        }
        len = (size_t)(w - p);
    }
    else if (clen >= 0 && (size_t)clen < len) len = (size_t)clen;
    if (len > max) len = max;
    if (out)
    {
        char *o = malloc(len + 1);
        memcpy(o, p, len);
        o[len] = 0;
        *out = o;
        if (outlen) *outlen = len;
    }
    free(b);
    return status;
}

// ---------------------------------------------------------------- XML

static void put_utf8(char **w, unsigned c)
{
    char *o = *w;
    if (c < 0x80) *o++ = (char)c;
    else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
    else { *o++ = (char)(0xF0 | c >> 18); *o++ = (char)(0x80 | (c >> 12 & 63)); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
    *w = o;
}

// &lt; &gt; &amp; &quot; &apos; &#n; &#xh; in place (the result is never longer)
static void x_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; )
    {
        if (*r != '&') { *w++ = *r++; continue; }
        char *semi = strchr(r, ';');
        if (!semi || semi - r > 10) { *w++ = *r++; continue; }
        size_t k = (size_t)(semi - r - 1);
        const char *e = r + 1;
        if (k == 2 && !strncmp(e, "lt", 2)) *w++ = '<';
        else if (k == 2 && !strncmp(e, "gt", 2)) *w++ = '>';
        else if (k == 3 && !strncmp(e, "amp", 3)) *w++ = '&';
        else if (k == 4 && !strncmp(e, "quot", 4)) *w++ = '"';
        else if (k == 4 && !strncmp(e, "apos", 4)) *w++ = '\'';
        else if (*e == '#')
        {
            unsigned c = e[1] == 'x' || e[1] == 'X' ? (unsigned)strtoul(e + 2, NULL, 16) : (unsigned)strtoul(e + 1, NULL, 10);
            if (c && c < 0x110000) put_utf8(&w, c); else { *w++ = *r++; continue; }
        }
        else { *w++ = *r++; continue; }
        r = semi + 1;
    }
    *w = 0;
}

// the element <name ...> (any prefix: <dc:title>) in [x, end): its start and its content
static const char *x_find(const char *x, const char *end, const char *name, const char **content, const char **cend)
{
    size_t nl = strlen(name);
    for (const char *p = x; p && p < end; p++)
    {
        p = memchr(p, '<', (size_t)(end - p));
        if (!p) return NULL;
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') continue;
        const char *ln = q;
        while (q < end && *q != '>' && *q != ' ' && *q != '/' && *q != ':') q++;
        if (q < end && *q == ':') ln = ++q;
        while (q < end && *q != '>' && *q != ' ' && *q != '/' && *q != '\t' && *q != '\r' && *q != '\n') q++;
        if ((size_t)(q - ln) != nl || strncmp(ln, name, nl)) continue;
        const char *gt = memchr(q, '>', (size_t)(end - q));
        if (!gt) return NULL;
        if (gt[-1] == '/') { if (content) { *content = gt; *cend = gt; } return p; }   // <name/>
        if (content)
        {
            *content = gt + 1;
            // its end: the next </...name>
            const char *c = gt + 1;
            for (;;)
            {
                c = strstr(c, "</");
                if (!c || c >= end) { *cend = end; break; }
                const char *cn = c + 2, *ce = cn;
                while (*ce && *ce != '>') ce++;
                const char *colon = memchr(cn, ':', (size_t)(ce - cn));
                if (colon) cn = colon + 1;
                if ((size_t)(ce - cn) == nl && !strncmp(cn, name, nl)) { *cend = c; break; }
                c += 2;
            }
        }
        return p;
    }
    return NULL;
}

// the text of the first <name> (unescaped once)
static bool x_get(const char *x, const char *end, const char *name, char *out, size_t size)
{
    const char *c, *ce;
    out[0] = 0;
    if (!x || !x_find(x, end, name, &c, &ce)) return false;
    size_t l = (size_t)(ce - c);
    if (l >= size) l = size - 1;
    memcpy(out, c, l);
    out[l] = 0;
    x_unescape(out);
    return true;
}

// an attribute of the tag that starts at tag (up to its '>')
static bool x_attr(const char *tag, const char *name, char *out, size_t size)
{
    out[0] = 0;
    const char *gt = strchr(tag, '>');
    size_t nl = strlen(name);
    for (const char *p = tag; p && p < gt; p++)
    {
        p = strstr(p, name);
        if (!p || p >= gt) return false;
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\''))
        {
            char qch = p[nl + 1];
            const char *v = p + nl + 2, *ve = strchr(v, qch);
            if (!ve) return false;
            size_t l = (size_t)(ve - v);
            if (l >= size) l = size - 1;
            memcpy(out, v, l);
            out[l] = 0;
            x_unescape(out);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- SOAP

enum { SV_AVT, SV_RC, SV_GRC, SV_ZGT };
static const struct { const char *path, *urn; } svc[] =
{
    { "/MediaRenderer/AVTransport/Control",           "urn:schemas-upnp-org:service:AVTransport:1" },
    { "/MediaRenderer/RenderingControl/Control",      "urn:schemas-upnp-org:service:RenderingControl:1" },
    { "/MediaRenderer/GroupRenderingControl/Control", "urn:schemas-upnp-org:service:GroupRenderingControl:1" },
    { "/ZoneGroupTopology/Control",                   "urn:schemas-upnp-org:service:ZoneGroupTopology:1" },
};

// the answer's body (malloc'd), NULL if the speaker said no or did not answer
static char *soap(const char *ip, int port, int sv, const char *action, const char *args)
{
    char *body = malloc(1024 + strlen(args));
    sprintf(body,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
            action, svc[sv].urn, args, action);
    char sa[200];
    snprintf(sa, sizeof sa, "%s#%s", svc[sv].urn, action);
    char *out = NULL;
    int st = http_do(ip, port, "POST", svc[sv].path, sa, body, &out, NULL, SOAP_MAX);
    free(body);
    if (st != 200) { free(out); return NULL; }
    return out;
}

static bool soap_ok(const char *ip, int port, int sv, const char *action, const char *args)
{
    char *r = soap(ip, port, sv, action, args);
    free(r);
    return r != NULL;
}

// ---------------------------------------------------------------- the rooms

static sys_mutex  *lock;
static sonos_room  rooms[MAX_ROOMS];
static int         n_rooms;
static volatile int dstate = SONOS_IDLE;
static unsigned    dgen;
static char        derr[128];
static uint32_t    dtime;              // when the rooms were last found
static sys_thread *dthread;

static sys_mutex  *dlock;

static void init_once(void)
{
    if (!lock) { lock = sys_mutex_new(); dlock = sys_mutex_new(); }
}

bool sonos_supported(void) { return true; }
int  sonos_state(void) { return dstate; }
unsigned sonos_gen(void) { return dgen; }
const char *sonos_error(void) { return derr; }

int sonos_rooms(sonos_room *out, int max)
{
    init_once();
    sys_lock(lock);
    int n = n_rooms < max ? n_rooms : max;
    memcpy(out, rooms, sizeof *out * (size_t)n);
    sys_unlock(lock);
    return n;
}

static bool find_room(const char *id, sonos_room *out)
{
    init_once();
    bool ok = false;
    sys_lock(lock);
    for (int i = 0; i < n_rooms && !ok; i++)
        if (!strcmp(rooms[i].id, id)) { *out = rooms[i]; ok = true; }
    sys_unlock(lock);
    return ok;
}

bool sonos_room_name(const char *id, char *out, size_t size)
{
    sonos_room r;
    if (!find_room(id, &r)) return false;
    u_copy(out, size, r.name);
    return true;
}

// "http://192.168.1.30:1400/xml/device_description.xml" -> ip, port
static bool parse_location(const char *loc, char *ip, size_t ipn, int *port)
{
    if (u_strnicmp(loc, "http://", 7)) return false;
    const char *h = loc + 7, *e = h;
    while (*e && *e != ':' && *e != '/') e++;
    size_t l = (size_t)(e - h);
    if (!l || l >= ipn) return false;
    memcpy(ip, h, l);
    ip[l] = 0;
    *port = *e == ':' ? atoi(e + 1) : 80;
    return *port > 0;
}

static int cmp_room(const void *a, const void *b) { return u_stricmp(((const sonos_room *)a)->name, ((const sonos_room *)b)->name); }

// GetZoneGroupState from one speaker: all the rooms
static int topology(const char *ip, int port, sonos_room *out, int max)
{
    char *r = soap(ip, port, SV_ZGT, "GetZoneGroupState", "");
    if (!r) return -1;
    const char *c, *ce;
    if (!x_find(r, r + strlen(r), "ZoneGroupState", &c, &ce)) { free(r); return -1; }
    size_t l = (size_t)(ce - c);
    char *z = malloc(l + 1);
    memcpy(z, c, l);
    z[l] = 0;
    free(r);
    x_unescape(z);
    int n = 0;
    const char *zend = z + strlen(z);
    for (const char *g = z; n < max && (g = x_find(g, zend, "ZoneGroup", &c, &ce)); g = ce)
    {
        char coord[48];
        if (!x_attr(g, "Coordinator", coord, sizeof coord) || ce <= c) { g = c; continue; }
        sonos_room room;
        memset(&room, 0, sizeof room);
        int visible = 0;
        bool found = false;
        // the members: <ZoneGroupMember .../> or <ZoneGroupMember ...> <Satellite/> </ZoneGroupMember>
        for (const char *m = c; (m = strstr(m, "<ZoneGroupMember")) && m < ce; m++)
        {
            char uuid[48], loc[256], zn[96], inv[8], bridge[8];
            x_attr(m, "UUID", uuid, sizeof uuid);
            x_attr(m, "Location", loc, sizeof loc);
            x_attr(m, "ZoneName", zn, sizeof zn);
            x_attr(m, "Invisible", inv, sizeof inv);
            x_attr(m, "IsZoneBridge", bridge, sizeof bridge);
            if (!strcmp(bridge, "1")) continue;
            if (strcmp(inv, "1")) visible++;
            if (!strcmp(uuid, coord))
            {
                found = parse_location(loc, room.ip, sizeof room.ip, &room.port);
                u_copy(room.id, sizeof room.id, uuid);
                u_copy(room.name, sizeof room.name, zn);
            }
        }
        if (!found || !room.name[0]) continue;
        room.members = visible ? visible : 1;
        if (room.members > 1)
        {
            char t[96];
            snprintf(t, sizeof t, "%s + %d", room.name, room.members - 1);
            u_copy(room.name, sizeof room.name, t);
        }
        out[n++] = room;
    }
    free(z);
    qsort(out, (size_t)n, sizeof *out, cmp_room);
    return n;
}

typedef struct { char ip[48]; int port; } cand_t;

static void add_cand(cand_t *c, int *n, const char *ip, int port)
{
    if (!ip[0] || *n >= MAX_CAND) return;
    for (int i = 0; i < *n; i++) if (!strcmp(c[i].ip, ip) && c[i].port == port) return;
    u_copy(c[*n].ip, sizeof c[*n].ip, ip);
    c[*n].port = port;
    (*n)++;
}

// the SSDP search: the speakers that answer within ms
static void ssdp_search(cand_t *c, int *n, int ms)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    to.sin_addr.s_addr = inet_addr("239.255.255.250");
    const char *ov = getenv("PICOAMP_SSDP");           // (the tests: a pretend speaker)
    if (ov && *ov)
    {
        char h[64];
        u_copy(h, sizeof h, ov);
        char *colon = strchr(h, ':');
        if (colon) { *colon = 0; to.sin_port = htons((uint16_t)atoi(colon + 1)); }
        to.sin_addr.s_addr = inet_addr(h);
    }
    else
    {
        unsigned char ttl = 4;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    }
    static const char msg[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    sock_timeout(s, 200);
    uint32_t t0 = sys_millis();
    int sent = 0;
    while ((int)(sys_millis() - t0) < ms)
    {
        if (sent < 2 && (int)(sys_millis() - t0) >= sent * 400)
        {
            sendto(s, msg, (int)sizeof msg - 1, 0, (struct sockaddr *)&to, sizeof to);
            sent++;
        }
        char b[1500];
        int r = recv(s, b, (int)sizeof b - 1, 0);
        if (r <= 0) continue;
        b[r] = 0;
        // its LOCATION: header
        for (char *l = b; l && *l; )
        {
            if (!u_strnicmp(l, "LOCATION:", 9))
            {
                char v[256], ip[48];
                int port;
                const char *p = l + 9;
                while (*p == ' ') p++;
                size_t k = strcspn(p, "\r\n");
                if (k >= sizeof v) k = sizeof v - 1;
                memcpy(v, p, k);
                v[k] = 0;
                if (parse_location(v, ip, sizeof ip, &port)) add_cand(c, n, ip, port);
            }
            l = strstr(l, "\n");
            if (l) l++;
        }
    }
    closesock(s);
}

static int disc_main(void *arg)
{
    (void)arg;
    cand_t c[MAX_CAND];
    int n = 0;
    // the ones known: first
    sys_lock(lock);
    for (int i = 0; i < n_rooms; i++) add_cand(c, &n, rooms[i].ip, rooms[i].port);
    sys_unlock(lock);
    char list[256];
    u_copy(list, sizeof list, cfg.sonos_ip);
    char *p = list;
    for (char *t; (t = u_token(&p, ", ;")); )
    {
        char *colon = strchr(t, ':');
        int port = colon ? atoi(colon + 1) : 1400;
        if (colon) *colon = 0;
        add_cand(c, &n, t, port);
    }
    int had = n;
    ssdp_search(c, &n, 1300);
    sonos_room found[MAX_ROOMS];
    int nf = -1;
    for (int i = 0; i < n && nf < 0; i++) nf = topology(c[i].ip, c[i].port, found, MAX_ROOMS);
    sys_lock(lock);
    if (nf > 0)
    {
        bool same = nf == n_rooms && !memcmp(found, rooms, sizeof *found * (size_t)nf);
        memcpy(rooms, found, sizeof *found * (size_t)nf);
        n_rooms = nf;
        if (!same) dgen++;
        derr[0] = 0;
        dtime = sys_millis();
        dstate = SONOS_DONE;
    }
    else
    {
        snprintf(derr, sizeof derr, n > had || n ? "The Sonos speakers did not answer" : "No Sonos speakers found");
        if (n_rooms) { n_rooms = 0; dgen++; }
        dstate = SONOS_FAILED;
    }
    sys_unlock(lock);
    return 0;
}

void sonos_discover(bool fresh)
{
    init_once();
    // (the screen, the followed room's thread and the bridge may all ask at once)
    sys_lock(dlock);
    if (dstate != SONOS_LOADING && (fresh || dstate != SONOS_DONE || sys_millis() - dtime >= 60000))
    {
        if (dthread) sys_thread_join(dthread);
        dstate = SONOS_LOADING;
        dthread = sys_thread_start(disc_main, NULL);
    }
    sys_unlock(dlock);
}

// ---------------------------------------------------------------- one room, now

static int hms(const char *t)          // "0:03:45" -> seconds (-1: none)
{
    int h, m, s;
    if (sscanf(t, "%d:%d:%d", &h, &m, &s) != 3) return -1;
    return h * 3600 + m * 60 + s;
}

static bool radio_uri(const char *u)
{
    static const char *const pre[] = { "x-sonosapi-stream:", "x-sonosapi-radio:", "x-rincon-mp3radio:", "x-sonosapi-hls:",
                                       "aac:", "hls-radio:", "x-sonos-http:sonos-radio", NULL };
    for (int i = 0; pre[i]; i++) if (!strncmp(u, pre[i], strlen(pre[i]))) return true;
    return false;
}

// "Artist - Title", or "TYPE=SNG|TITLE t|ARTIST a|ALBUM b": the song a station plays
static void stream_song(const char *sc, char *title, size_t tn, char *artist, size_t an)
{
    title[0] = artist[0] = 0;
    if (!sc[0] || !strncmp(sc, "ZPSTR_", 6)) return;          // "ZPSTR_CONNECTING", "ZPSTR_BUFFERING"
    if (strstr(sc, "TITLE ") && strchr(sc, '|'))
    {
        char t[512];
        u_copy(t, sizeof t, sc);
        char *p = t;
        for (char *f; (f = u_token(&p, "|")); )
        {
            if (!strncmp(f, "TITLE ", 6)) u_copy(title, tn, f + 6);
            else if (!strncmp(f, "ARTIST ", 7)) u_copy(artist, an, f + 7);
        }
        return;
    }
    const char *d = strstr(sc, " - ");
    if (d)
    {
        size_t l = (size_t)(d - sc);
        if (l >= an) l = an - 1;
        memcpy(artist, sc, l);
        artist[l] = 0;
        u_copy(title, tn, d + 3);
    }
    else u_copy(title, tn, sc);
}

typedef struct
{
    np_info i;
    char    art_url[1024];
    char    joined[48];          // the room became a member of another group: its coordinator
    bool    radio;
} rnow_t;

static bool ask_room(const sonos_room *r, rnow_t *o)
{
    memset(o, 0, sizeof *o);
    np_info *i = &o->i;
    char *x = soap(r->ip, r->port, SV_AVT, "GetTransportInfo", "<InstanceID>0</InstanceID>");
    if (!x) return false;
    char v[2048];
    x_get(x, x + strlen(x), "CurrentTransportState", v, sizeof v);
    free(x);
    i->state = !strcmp(v, "PLAYING") || !strcmp(v, "TRANSITIONING") ? NP_PLAYING : !strcmp(v, "PAUSED_PLAYBACK") ? NP_PAUSED : NP_STOPPED;
    i->at = sys_millis();
    snprintf(i->app, sizeof i->app, "Sonos %s", r->name);
    i->volume = -1;

    x = soap(r->ip, r->port, SV_AVT, "GetPositionInfo", "<InstanceID>0</InstanceID>");
    if (!x) return false;
    const char *xe = x + strlen(x);
    char uri[1024];
    x_get(x, xe, "TrackURI", uri, sizeof uri);
    if (!strncmp(uri, "x-rincon:", 9)) { u_copy(o->joined, sizeof o->joined, uri + 9); free(x); return true; }
    x_get(x, xe, "TrackDuration", v, sizeof v);
    int total = hms(v);
    x_get(x, xe, "RelTime", v, sizeof v);
    int rel = hms(v);
    // TrackMetaData: DIDL-Lite, escaped
    const char *c, *ce;
    char *didl = NULL;
    if (x_find(x, xe, "TrackMetaData", &c, &ce) && ce > c)
    {
        size_t l = (size_t)(ce - c);
        didl = malloc(l + 1);
        memcpy(didl, c, l);
        didl[l] = 0;
        x_unescape(didl);
        if (!strcmp(didl, "NOT_IMPLEMENTED")) didl[0] = 0;
    }
    free(x);
    const char *de = didl ? didl + strlen(didl) : NULL;
    char title[256] = "", artist[256] = "", album[256] = "", sc[512] = "";
    if (didl)
    {
        x_get(didl, de, "title", title, sizeof title);
        if (!x_get(didl, de, "creator", artist, sizeof artist) || !artist[0]) x_get(didl, de, "albumArtist", artist, sizeof artist);
        x_get(didl, de, "album", album, sizeof album);
        x_get(didl, de, "streamContent", sc, sizeof sc);
        x_get(didl, de, "albumArtURI", o->art_url, sizeof o->art_url);
    }
    free(didl);
    o->radio = radio_uri(uri) || (total <= 0 && sc[0]);
    if (!strncmp(uri, "x-rincon-stream:", 16)) { u_copy(title, sizeof title, "Line-in"); artist[0] = album[0] = 0; }
    else if (!strncmp(uri, "x-sonos-htastream:", 18)) { u_copy(title, sizeof title, "TV"); artist[0] = album[0] = 0; }
    if (o->radio)
    {
        // the station's name, and the song it plays
        char station[256] = "";
        x = soap(r->ip, r->port, SV_AVT, "GetMediaInfo", "<InstanceID>0</InstanceID>");
        if (x)
        {
            const char *e2 = x + strlen(x);
            if (x_find(x, e2, "CurrentURIMetaData", &c, &ce) && ce > c)
            {
                size_t l = (size_t)(ce - c);
                char *m = malloc(l + 1);
                memcpy(m, c, l);
                m[l] = 0;
                x_unescape(m);
                x_get(m, m + strlen(m), "title", station, sizeof station);
                if (!o->art_url[0]) x_get(m, m + strlen(m), "albumArtURI", o->art_url, sizeof o->art_url);
                free(m);
            }
            free(x);
        }
        if (!station[0]) u_copy(station, sizeof station, title);
        char st[256], sa[256];
        stream_song(sc, st, sizeof st, sa, sizeof sa);
        if (st[0]) { u_copy(title, sizeof title, st); u_copy(artist, sizeof artist, sa); }
        else { u_copy(title, sizeof title, station); artist[0] = 0; }
        u_copy(album, sizeof album, station);
        total = 0;
    }
    u_copy(i->title, sizeof i->title, title);
    u_copy(i->artist, sizeof i->artist, artist);
    u_copy(i->album, sizeof i->album, album);
    if (!title[0] && i->state == NP_STOPPED) snprintf(i->artist, sizeof i->artist, "Nothing plays in %s", r->name);
    i->total_ms = total > 0 ? total * 1000 : 0;
    i->elapsed_ms = rel > 0 ? rel * 1000 : 0;
    i->can_seek = total > 0 && !o->radio;
    i->can_next = i->can_prev = i->can_shuffle = i->can_repeat = !o->radio && title[0];

    x = soap(r->ip, r->port, SV_AVT, "GetTransportSettings", "<InstanceID>0</InstanceID>");
    if (x)
    {
        x_get(x, x + strlen(x), "PlayMode", v, sizeof v);
        free(x);
        i->shuffle = !strncmp(v, "SHUFFLE", 7);
        i->repeat = strstr(v, "REPEAT_ONE") ? NP_REPEAT_TRACK : (!strcmp(v, "REPEAT_ALL") || !strcmp(v, "SHUFFLE")) ? NP_REPEAT_LIST : NP_REPEAT_OFF;
    }
    // the volume: the group's, or the speaker's
    bool g = r->members > 1;
    x = g ? soap(r->ip, r->port, SV_GRC, "GetGroupVolume", "<InstanceID>0</InstanceID>")
          : soap(r->ip, r->port, SV_RC, "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>");
    if (x)
    {
        if (x_get(x, x + strlen(x), g ? "CurrentVolume" : "CurrentVolume", v, sizeof v)) i->volume = atoi(v);
        free(x);
        x = g ? soap(r->ip, r->port, SV_GRC, "GetGroupMute", "<InstanceID>0</InstanceID>")
              : soap(r->ip, r->port, SV_RC, "GetMute", "<InstanceID>0</InstanceID><Channel>Master</Channel>");
        if (x) { x_get(x, x + strlen(x), "CurrentMute", v, sizeof v); i->muted = atoi(v) != 0; free(x); }
    }
    if (o->art_url[0] == '/')
    {
        char a[1024];
        snprintf(a, sizeof a, "http://%s:%d%s", r->ip, r->port, o->art_url);
        u_copy(o->art_url, sizeof o->art_url, a);
    }
    return true;
}

// the room, following it when it joined another group
static bool ask(const char *id, sonos_room *r, rnow_t *o)
{
    if (!find_room(id, r)) return false;
    if (!ask_room(r, o)) return false;
    if (o->joined[0])
    {
        sonos_room c;
        if (find_room(o->joined, &c)) { *r = c; return ask_room(r, o) && !o->joined[0]; }
        return false;
    }
    return true;
}

static void fmt_hms(char *o, size_t n, int ms)
{
    int s = ms / 1000;
    snprintf(o, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
}

static bool do_cmd(const sonos_room *r, const np_info *now, int cmd, int arg)
{
    const char *ip = r->ip;
    int port = r->port;
    char a[256];
    switch (cmd)
    {
    case NP_TOGGLE:
        if (now->state == NP_PLAYING) return do_cmd(r, now, NP_PAUSE, 0);
        /* fall through */
    case NP_PLAY:
        return soap_ok(ip, port, SV_AVT, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>");
    case NP_PAUSE:
    case NP_STOP:
        // a station that cannot pause stops
        return soap_ok(ip, port, SV_AVT, "Pause", "<InstanceID>0</InstanceID>")
            || soap_ok(ip, port, SV_AVT, "Stop", "<InstanceID>0</InstanceID>");
    case NP_NEXT:
        return soap_ok(ip, port, SV_AVT, "Next", "<InstanceID>0</InstanceID>");
    case NP_PREV:
        return soap_ok(ip, port, SV_AVT, "Previous", "<InstanceID>0</InstanceID>");
    case NP_SEEK_TO:
    {
        char t[32];
        fmt_hms(t, sizeof t, arg < 0 ? 0 : arg);
        snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%s</Target>", t);
        return soap_ok(ip, port, SV_AVT, "Seek", a);
    }
    case NP_SHUFFLE:
    case NP_REPEAT:
    {
        bool sh = cmd == NP_SHUFFLE ? arg != 0 : now->shuffle;
        int rp = cmd == NP_REPEAT ? arg : now->repeat;
        const char *m = sh ? (rp == NP_REPEAT_TRACK ? "SHUFFLE_REPEAT_ONE" : rp == NP_REPEAT_LIST ? "SHUFFLE" : "SHUFFLE_NOREPEAT")
                           : (rp == NP_REPEAT_TRACK ? "REPEAT_ONE" : rp == NP_REPEAT_LIST ? "REPEAT_ALL" : "NORMAL");
        snprintf(a, sizeof a, "<InstanceID>0</InstanceID><NewPlayMode>%s</NewPlayMode>", m);
        return soap_ok(ip, port, SV_AVT, "SetPlayMode", a);
    }
    }
    return false;
}

static bool do_volume(const sonos_room *r, int vol, int mute)
{
    bool g = r->members > 1, ok = true;
    char a[160];
    if (vol >= 0)
    {
        if (vol > 100) vol = 100;
        if (g) snprintf(a, sizeof a, "<InstanceID>0</InstanceID><DesiredVolume>%d</DesiredVolume>", vol);
        else snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", vol);
        ok = soap_ok(r->ip, r->port, g ? SV_GRC : SV_RC, g ? "SetGroupVolume" : "SetVolume", a);
    }
    if (mute >= 0)
    {
        if (g) snprintf(a, sizeof a, "<InstanceID>0</InstanceID><DesiredMute>%d</DesiredMute>", mute ? 1 : 0);
        else snprintf(a, sizeof a, "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredMute>%d</DesiredMute>", mute ? 1 : 0);
        ok = soap_ok(r->ip, r->port, g ? SV_GRC : SV_RC, g ? "SetGroupMute" : "SetMute", a) && ok;
    }
    return ok;
}

static bool fetch_art(const char *url, uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    if (!url[0]) return false;
    if (!u_strnicmp(url, "http://", 7))
    {
        char ip[48];
        int port;
        if (!parse_location(url, ip, sizeof ip, &port)) return false;
        const char *path = strchr(url + 7, '/');
        char *b = NULL;
        size_t n = 0;
        if (http_do(ip, port, "GET", path ? path : "/", NULL, NULL, &b, &n, ART_MAX) != 200 || n < 16) { free(b); return false; }
        *data = (uint8_t *)b;
        *len = n;
        return true;
    }
    char err[128];
    size_t n = 0;
    char *b = net_fetch(url, ART_MAX, &n, err, sizeof err);
    if (!b || n < 16) { free(b); return false; }
    *data = (uint8_t *)b;
    *len = n;
    return true;
}

bool sonos_query(const char *id, np_info *out)
{
    sonos_room r;
    rnow_t o;
    if (!ask(id, &r, &o)) return false;
    *out = o.i;
    out->art_gen = 0;
    for (const char *p = o.art_url; *p; p++) out->art_gen = out->art_gen * 31u + (unsigned char)*p;
    return true;
}

bool sonos_command(const char *id, int cmd, int arg)
{
    sonos_room r;
    rnow_t o;
    if (!ask(id, &r, &o)) return false;
    return do_cmd(&r, &o.i, cmd, arg);
}

bool sonos_set_volume(const char *id, int volume, int muted)
{
    sonos_room r;
    if (!find_room(id, &r)) return false;
    return do_volume(&r, volume, muted);
}

bool sonos_room_art(const char *id, uint8_t **data, size_t *len)
{
    sonos_room r;
    rnow_t o;
    if (!ask(id, &r, &o)) return false;
    return fetch_art(o.art_url, data, len);
}

// ---------------------------------------------------------------- following one room

#define QMAX 16
static sys_thread   *fthread;
static volatile bool fon;
static char          fid[48];
static np_info       fnow;
static bool          fhave;
static char          fart_url[1024];
static uint8_t      *fart;
static size_t        fart_len;
static unsigned      fart_gen;
static struct { int cmd, arg; } fq[QMAX];
static int           fqn;
static int           fvol = -1, fmute = -1;

static int follow_main(void *arg)
{
    (void)arg;
    uint32_t last = 0;
    bool asked = false;
    while (fon)
    {
        // the keys first
        sys_lock(lock);
        int n = fqn;
        struct { int cmd, arg; } q[QMAX];
        memcpy(q, fq, sizeof q);
        fqn = 0;
        int vol = fvol, mute = fmute;
        fvol = fmute = -1;
        char id[48];
        u_copy(id, sizeof id, fid);
        np_info now = fnow;
        sys_unlock(lock);
        sonos_room r;
        bool known = find_room(id, &r);
        if (!known && sonos_state() != SONOS_LOADING && (!asked || sys_millis() - last > 5000)) { sonos_discover(true); asked = true; last = sys_millis(); }
        if (known)
        {
            for (int i = 0; i < n; i++) do_cmd(&r, &now, q[i].cmd, q[i].arg);
            if (vol >= 0 || mute >= 0) do_volume(&r, vol, mute);
        }
        if (known && (n || vol >= 0 || mute >= 0 || !last || sys_millis() - last >= POLL_MS))
        {
            last = sys_millis();
            rnow_t o;
            bool ok = ask_room(&r, &o);
            if (ok && o.joined[0])
            {
                // the room joined another group: follow that one
                sonos_room c;
                if (find_room(o.joined, &c)) { sys_lock(lock); u_copy(fid, sizeof fid, c.id); sys_unlock(lock); continue; }
                sonos_discover(true);
                ok = false;
            }
            if (!ok)
            {
                // gone (its group changed, or it is off): look again
                if (sonos_state() != SONOS_LOADING) sonos_discover(true);
                sys_lock(lock);
                if (fhave) { fnow.state = NP_STOPPED; snprintf(fnow.artist, sizeof fnow.artist, "%s does not answer", r.name); }
                sys_unlock(lock);
            }
            else
            {
                uint8_t *img = NULL;
                size_t il = 0;
                bool new_art = strcmp(o.art_url, fart_url) != 0;
                if (new_art) fetch_art(o.art_url, &img, &il);
                sys_lock(lock);
                fnow = o.i;
                fhave = true;
                if (new_art)
                {
                    u_copy(fart_url, sizeof fart_url, o.art_url);
                    free(fart);
                    fart = img;
                    fart_len = il;
                    fart_gen++;
                }
                fnow.art_gen = fart_gen;
                sys_unlock(lock);
            }
        }
        for (int i = 0; i < 10 && fon; i++)
        {
            sys_sleep_ms(20);
            sys_lock(lock);
            bool more = fqn || fvol >= 0 || fmute >= 0;
            sys_unlock(lock);
            if (more) break;
        }
    }
    return 0;
}

void sonos_start(const char *id)
{
    init_once();
    sonos_end();
    char nm[96];
    if (!sonos_room_name(id, nm, sizeof nm)) u_copy(nm, sizeof nm, "");
    sys_lock(lock);
    u_copy(fid, sizeof fid, id);
    memset(&fnow, 0, sizeof fnow);
    snprintf(fnow.app, sizeof fnow.app, nm[0] ? "Sonos %s" : "Sonos", nm);
    fnow.state = NP_STOPPED;
    fnow.volume = -1;
    fhave = false;
    fart_url[0] = 0;
    free(fart);
    fart = NULL;
    fart_len = 0;
    fqn = 0;
    fvol = fmute = -1;
    sys_unlock(lock);
    fon = true;
    fthread = sys_thread_start(follow_main, NULL);
}

void sonos_end(void)
{
    if (!fthread) return;
    fon = false;
    sys_thread_join(fthread);
    fthread = NULL;
}

bool sonos_get(np_info *out)
{
    if (!fthread) return false;
    sys_lock(lock);
    *out = fnow;
    sys_unlock(lock);
    return true;
}

bool sonos_art(uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    sys_lock(lock);
    if (fart && fart_len)
    {
        *data = malloc(fart_len);
        memcpy(*data, fart, fart_len);
        *len = fart_len;
    }
    sys_unlock(lock);
    return *data != NULL;
}

void sonos_cmd(int cmd, int arg)
{
    if (!fthread) return;
    sys_lock(lock);
    // what the room cannot do (a station: no next, no seeking) is not sent
    bool ok = !((cmd == NP_NEXT && !fnow.can_next) || (cmd == NP_PREV && !fnow.can_prev) || (cmd == NP_SEEK_TO && !fnow.can_seek)
                || (cmd == NP_SHUFFLE && !fnow.can_shuffle) || (cmd == NP_REPEAT && !fnow.can_repeat));
    // play / pause: as the screen shows it now
    if (cmd == NP_TOGGLE) cmd = fnow.state == NP_PLAYING ? NP_PAUSE : NP_PLAY;
    if (ok && fqn < QMAX) { fq[fqn].cmd = cmd; fq[fqn].arg = arg; fqn++; }
    // what the screen shows at once (the speaker says it on the next ask)
    if (ok && cmd == NP_PLAY) fnow.state = NP_PLAYING;
    if (ok && cmd == NP_PAUSE && fnow.state == NP_PLAYING) fnow.state = NP_PAUSED;
    if (ok && cmd == NP_SHUFFLE) fnow.shuffle = arg != 0;
    if (ok && cmd == NP_REPEAT) fnow.repeat = arg;
    sys_unlock(lock);
}

void sonos_volume(int volume, bool muted)
{
    if (!fthread) return;
    sys_lock(lock);
    if (volume != fnow.volume) fvol = volume;
    if (muted != fnow.muted) fmute = muted ? 1 : 0;
    fnow.volume = volume;
    fnow.muted = muted;
    sys_unlock(lock);
}
