// stream.h - an internet radio stream: its own thread reads it into a ring,
// takes the ICY titles out, and follows .pls / .m3u lists to the stream.
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef struct stream stream;

typedef struct
{
    char title[256];      // StreamTitle of the ICY metadata ("Artist - Song")
    char station[256];    // icy-name
    char type[64];        // Content-Type
    int  bitrate;         // kbit/s (icy-br, or from the first MP3 frame)
    bool tls;             // over https
    bool connected;       // the stream's head has been read
    bool failed;          // gave up: see error
    bool ended;
    char error[160];
    int  fill_pct;        // how full the ring is
    unsigned meta_gen;    // changes with every new title
} stream_info;

stream *stream_start(const char *url);
// the stream's bytes: blocks until there are some; 0 = the end (or failed, or stopped)
size_t  stream_read(stream *s, void *buf, size_t n, volatile bool *stop);
size_t  stream_buffered(stream *s);
size_t  stream_capacity(stream *s);
void    stream_info_get(stream *s, stream_info *out);
void    stream_release(stream *s);      // stop it; the thread frees it when done
