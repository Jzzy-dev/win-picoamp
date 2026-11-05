// net.c - HTTP(S) GET (see net.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "net.h"
#include "sys.h"
#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define USER_AGENT "PicoAmp/0.1 (Windows; +https://github.com/Jzzy-dev/win-picoamp)"
#define MAX_HEAD   16384

struct net_conn
{
    bool   tls;
    sock_t s;
#ifdef _WIN32
    HINTERNET ses, con, req;
#endif
    char  *head;           // the reply's header lines, "name: value\r\n" ...
    char  *pending;        // body bytes read with the head
    size_t pend_len, pend_pos;
    char   url[2048];
    volatile bool *abort;
};

void net_init(void)
{
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

typedef struct { bool tls; char host[256]; int port; char path[1800]; } url_t;

static bool parse_url(const char *u, url_t *o)
{
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "http://", 7)) { u += 7; o->port = 80; }
    else if (!strncmp(u, "https://", 8)) { u += 8; o->port = 443; o->tls = true; }
    else return false;
    const char *slash = strchr(u, '/');
    const char *q = strchr(u, '?');
    if (q && (!slash || q < slash)) slash = q;
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (!hl || hl >= sizeof o->host) return false;
    memcpy(o->host, u, hl);
    o->host[hl] = 0;
    char *at = strrchr(o->host, '@');                 // user:pass@ - not used
    if (at) memmove(o->host, at + 1, strlen(at + 1) + 1);
    char *colon = o->host[0] == '[' ? strstr(o->host, "]:") : strrchr(o->host, ':');
    if (colon)
    {
        if (*colon == ']') colon++;
        o->port = atoi(colon + 1);
        *colon = 0;
    }
    if (o->host[0] == '[') { memmove(o->host, o->host + 1, strlen(o->host)); char *e = strchr(o->host, ']'); if (e) *e = 0; }
    if (slash && *slash == '/') snprintf(o->path, sizeof o->path, "%s", slash);
    else snprintf(o->path, sizeof o->path, "/%s", slash ? slash : "");
    for (char *p = o->path; *p; p++) if (*p == ' ') *p = '+';    // a space in a list's URL
    return o->port > 0 && o->port < 65536;
}

const char *net_header(net_conn *c, const char *name)
{
    if (!c || !c->head) return NULL;
    size_t nl = strlen(name);
    for (char *p = c->head; *p; )
    {
        char *eol = strstr(p, "\r\n");
        if (!eol) break;
        if (!u_strnicmp(p, name, nl) && p[nl] == ':')
        {
            char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            // the value up to the line's end: copy into the line itself
            static __thread char val[1024];
            size_t l = (size_t)(eol - v);
            if (l >= sizeof val) l = sizeof val - 1;
            memcpy(val, v, l);
            val[l] = 0;
            return val;
        }
        p = eol + 2;
    }
    return NULL;
}

const char *net_final_url(net_conn *c) { return c ? c->url : ""; }
bool net_is_tls(net_conn *c) { return c && c->tls; }

static bool aborted(net_conn *c) { return c->abort && *c->abort; }

// ---------------- http over sockets ----------------
static sock_t connect_to(const char *host, int port, char *err, size_t errlen, volatile bool *abort)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res)
    {
        snprintf(err, errlen, "Unknown host %s", host);
        return BAD_SOCK;
    }
    sock_t s = BAD_SOCK;
    for (struct addrinfo *a = res; a && !(abort && *abort); a = a->ai_next)
    {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == BAD_SOCK) continue;
        if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesock(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) { snprintf(err, errlen, "No connection to %s", host); return BAD_SOCK; }
    // reads wake up now and then to look at the abort flag
#ifdef _WIN32
    DWORD tv = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv = { 0, 500000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    return s;
}

// recv with the abort flag and a 15 s limit: >0, 0 (closed), -1
static long sock_recv(net_conn *c, void *buf, size_t n)
{
    uint32_t t0 = sys_millis();
    for (;;)
    {
        if (aborted(c)) return -1;
        long r = recv(c->s, buf, (int)n, 0);
        if (r >= 0) return r;
#ifdef _WIN32
        int e = WSAGetLastError();
        bool timeout = e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
        bool timeout = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
        if (!timeout || sys_millis() - t0 > 15000) return -1;
    }
}

static int http_open(net_conn *c, const url_t *u, const char *extra, char *err, size_t errlen, char *location, size_t loclen)
{
    c->s = connect_to(u->host, u->port, err, errlen, c->abort);
    if (c->s == BAD_SOCK) return 0;
    char req[4096], hostport[300];
    if (u->port == 80) snprintf(hostport, sizeof hostport, "%s", u->host);
    else snprintf(hostport, sizeof hostport, "%s:%d", u->host, u->port);
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\nAccept: */*\r\n%s%sConnection: close\r\n\r\n",
                     u->path, hostport, extra ? extra : "", extra && *extra ? "\r\n" : "");
    if (send(c->s, req, n, 0) != n) { snprintf(err, errlen, "Could not send the request"); return 0; }
    // the head, up to the empty line
    char *h = malloc(MAX_HEAD + 1);
    size_t hl = 0;
    char *end = NULL;
    while (!end)
    {
        if (hl >= MAX_HEAD) { free(h); snprintf(err, errlen, "Reply head too long"); return 0; }
        long r = sock_recv(c, h + hl, MAX_HEAD - hl);
        if (r <= 0) { free(h); snprintf(err, errlen, "No reply from %s", u->host); return 0; }
        hl += (size_t)r;
        h[hl] = 0;
        end = strstr(h, "\r\n\r\n");
        if (!end)
        {
            char *lf = strstr(h, "\n\n");          // a server with bare LFs
            if (lf) { end = lf; }
        }
    }
    size_t head_len = (size_t)(end - h) + (end[0] == '\r' ? 4 : 2);
    c->pend_len = hl - head_len;
    c->pend_pos = 0;
    free(c->pending);
    c->pending = malloc(c->pend_len ? c->pend_len : 1);
    memcpy(c->pending, h + head_len, c->pend_len);
    // status line
    int status = 0;
    if (!strncmp(h, "HTTP/", 5)) { char *sp = strchr(h, ' '); if (sp) status = atoi(sp + 1); }
    else if (!strncmp(h, "ICY ", 4)) status = atoi(h + 4);
    // the header lines with CRLF each
    char *first = strchr(h, '\n');
    size_t rest = first ? (size_t)(end - first - 1) : 0;
    c->head = malloc(rest * 2 + 8);
    char *o = c->head;
    if (first)
        for (char *p = first + 1; p < end; p++)
        {
            if (*p == '\r') continue;
            if (*p == '\n') { *o++ = '\r'; *o++ = '\n'; continue; }
            *o++ = *p;
        }
    *o++ = '\r'; *o++ = '\n'; *o = 0;
    free(h);
    const char *loc = net_header(c, "Location");
    if (loc) snprintf(location, loclen, "%s", loc);
    if (!status) snprintf(err, errlen, "Not an HTTP reply");
    return status;
}

// ---------------- https: WinHTTP ----------------
#ifdef _WIN32
static wchar_t *wdup(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

static int winhttp_open(net_conn *c, const url_t *u, const char *extra, char *err, size_t errlen)
{
    c->ses = WinHttpOpen(L"" USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->ses) c->ses = WinHttpOpen(L"" USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->ses) { snprintf(err, errlen, "WinHTTP not available"); return 0; }
    WinHttpSetTimeouts(c->ses, 10000, 10000, 10000, 15000);
    wchar_t *wh = wdup(u->host), *wp = wdup(u->path);
    c->con = WinHttpConnect(c->ses, wh, (INTERNET_PORT)u->port, 0);
    c->req = c->con ? WinHttpOpenRequest(c->con, L"GET", wp, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u->tls ? WINHTTP_FLAG_SECURE : 0) : NULL;
    free(wh);
    free(wp);
    if (!c->req) { snprintf(err, errlen, "No connection to %s", u->host); return 0; }
    if (extra && *extra)
    {
        wchar_t *we = wdup(extra);
        WinHttpAddRequestHeaders(c->req, we, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        free(we);
    }
    if (!WinHttpSendRequest(c->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(c->req, NULL))
    {
        DWORD e = GetLastError();
        if (e == ERROR_WINHTTP_SECURE_FAILURE) snprintf(err, errlen, "HTTPS: the certificate of %s is not valid", u->host);
        else if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) snprintf(err, errlen, "Unknown host %s", u->host);
        else if (e == ERROR_WINHTTP_TIMEOUT) snprintf(err, errlen, "%s does not answer", u->host);
        else snprintf(err, errlen, "No connection to %s (%lu)", u->host, (unsigned long)e);
        return 0;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(c->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    sz = 0;
    WinHttpQueryHeaders(c->req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &sz, WINHTTP_NO_HEADER_INDEX);
    wchar_t *wraw = malloc(sz + 2);
    if (WinHttpQueryHeaders(c->req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, wraw, &sz, WINHTTP_NO_HEADER_INDEX))
    {
        wraw[sz / 2] = 0;
        int n = WideCharToMultiByte(CP_UTF8, 0, wraw, -1, NULL, 0, NULL, NULL);
        char *raw = malloc(n > 0 ? n : 1);
        WideCharToMultiByte(CP_UTF8, 0, wraw, -1, raw, n, NULL, NULL);
        char *first = strstr(raw, "\r\n");          // skip the status line
        c->head = strdup(first ? first + 2 : "");
        free(raw);
    }
    free(wraw);
    // the URL after WinHTTP's own redirects
    DWORD ul = 0;
    WinHttpQueryOption(c->req, WINHTTP_OPTION_URL, NULL, &ul);
    if (ul)
    {
        wchar_t *wu = malloc(ul + 2);
        if (WinHttpQueryOption(c->req, WINHTTP_OPTION_URL, wu, &ul))
        {
            wu[ul / 2] = 0;
            WideCharToMultiByte(CP_UTF8, 0, wu, -1, c->url, sizeof c->url, NULL, NULL);
        }
        free(wu);
    }
    return (int)status;
}
#endif

static void conn_free_parts(net_conn *c)
{
    if (c->s != BAD_SOCK) closesock(c->s);
    c->s = BAD_SOCK;
#ifdef _WIN32
    if (c->req) WinHttpCloseHandle(c->req);
    if (c->con) WinHttpCloseHandle(c->con);
    if (c->ses) WinHttpCloseHandle(c->ses);
    c->req = c->con = c->ses = NULL;
#endif
    free(c->head);
    c->head = NULL;
    free(c->pending);
    c->pending = NULL;
    c->pend_len = c->pend_pos = 0;
}

net_conn *net_get(const char *url, const char *extra, int *status, char *err, size_t errlen, volatile bool *abort)
{
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", url);
    *status = 0;
    if (err && errlen) err[0] = 0;
    net_conn *c = calloc(1, sizeof *c);
    c->s = BAD_SOCK;
    c->abort = abort;
    for (int hop = 0; hop < 6; hop++)
    {
        url_t u;
        if (!parse_url(cur, &u)) { snprintf(err, errlen, "Not an http(s) address"); break; }
        snprintf(c->url, sizeof c->url, "%s", cur);
        c->tls = u.tls;
        char loc[2048] = "";
        int st;
#ifdef _WIN32
        if (u.tls) st = winhttp_open(c, &u, extra, err, errlen);
        else st = http_open(c, &u, extra, err, errlen, loc, sizeof loc);
#else
        if (u.tls) { snprintf(err, errlen, "HTTPS is not in this test build"); st = 0; }
        else st = http_open(c, &u, extra, err, errlen, loc, sizeof loc);
#endif
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && loc[0])
        {
            // a relative Location: the same server
            char next[4400];
            if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) snprintf(next, sizeof next, "%s", loc);
            else if (loc[0] == '/') snprintf(next, sizeof next, "%s://%s:%d%s", u.tls ? "https" : "http", u.host, u.port, loc);
            else
            {
                char base[2048];
                snprintf(base, sizeof base, "%s", cur);
                char *sl = strrchr(base + 8, '/');
                if (sl) sl[1] = 0;
                snprintf(next, sizeof next, "%s%s", base, loc);
            }
            conn_free_parts(c);
            u_copy(cur, sizeof cur, next);
            continue;
        }
        *status = st;
        if (st >= 200 && st < 300) return c;
        if (st && err && errlen) snprintf(err, errlen, "The server answers %d", st);
        break;
    }
    if (!*status && err && !err[0]) snprintf(err, errlen, "Too many redirects");
    net_close(c);
    return NULL;
}

long net_read(net_conn *c, void *buf, size_t n)
{
    if (!c) return -1;
    if (c->pend_pos < c->pend_len)
    {
        size_t k = c->pend_len - c->pend_pos;
        if (k > n) k = n;
        memcpy(buf, c->pending + c->pend_pos, k);
        c->pend_pos += k;
        return (long)k;
    }
#ifdef _WIN32
    if (c->req)
    {
        if (aborted(c)) return -1;
        DWORD got = 0;
        if (!WinHttpReadData(c->req, buf, (DWORD)n, &got)) return -1;
        return (long)got;
    }
#endif
    return sock_recv(c, buf, n);
}

void net_close(net_conn *c)
{
    if (!c) return;
    conn_free_parts(c);
    free(c);
}

char *net_fetch(const char *url, size_t max, size_t *len, char *err, size_t errlen)
{
    int st;
    net_conn *c = net_get(url, NULL, &st, err, errlen, NULL);
    if (!c) return NULL;
    size_t cap = 16384, n = 0;
    char *b = malloc(cap + 1);
    for (;;)
    {
        if (n == cap)
        {
            if (cap >= max) break;
            cap *= 2;
            if (cap > max) cap = max;
            b = realloc(b, cap + 1);
        }
        long r = net_read(c, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    net_close(c);
    b[n] = 0;
    if (len) *len = n;
    return b;
}
