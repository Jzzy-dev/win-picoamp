// net.h - HTTP and HTTPS GET for radio streams and small files.
//
// http:  own client over sockets, HTTP/1.0 (no chunks), which also takes the
//        "ICY 200 OK" of old SHOUTcast servers.
// https: WinHTTP on Windows (the system's certificates and TLS); not
//        available in the POSIX test build.
// Redirects are followed (up to 5).
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct net_conn net_conn;

// open url and read the reply's head; *status = the HTTP status (or 0 and a
// message in err). abort (may be NULL) stops a connect or a read that waits.
net_conn   *net_get(const char *url, const char *extra_headers, int *status,
                    char *err, size_t errlen, volatile bool *abort);
const char *net_header(net_conn *c, const char *name);   // NULL: not there
const char *net_final_url(net_conn *c);
bool        net_is_tls(net_conn *c);
// any request: method "GET" or "POST" (body: a form, application/x-www-form-urlencoded).
// cookies (may be NULL): a jar, "a=1; b=2" (size bytes): sent along, and each reply's
// Set-Cookie (redirects too: they are followed here, a POST going on as a GET) is added.
net_conn   *net_request(const char *method, const char *url, const char *extra_headers,
                        const char *body, size_t body_len, char *cookies, size_t cookies_size,
                        int *status, char *err, size_t errlen, volatile bool *abort);
// some bytes (blocks until there are some): >0 bytes, 0 the end, -1 an error
long        net_read(net_conn *c, void *buf, size_t n);
void        net_close(net_conn *c);

// a whole (small) reply into memory, 0-terminated; NULL on error
char       *net_fetch(const char *url, size_t max, size_t *len, char *err, size_t errlen);

void        net_init(void);
