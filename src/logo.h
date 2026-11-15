// logo.h - pictures from the internet for the cover view (as the MSX engine
// 0.16): a station's logo (Radio Browser's "favicon") and a song's or an
// album's cover (the iTunes search), kept in <config>/Logos and <config>/Covers.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

enum { LOGO_STATION, LOGO_SONG };       // the two lookups, each on its own

// a station starts: its address, its directory id (or NULL), its name
void logo_station(const char *url, const char *uuid, const char *name);
// a song (what: its title) or an album (what: its name, album true)
void logo_song(const char *artist, const char *what, bool album);
void logo_cancel(int which);
// the result of a lookup, once: true and *data (malloc'd picture bytes, or NULL: none)
bool logo_take(int which, uint8_t **data, size_t *len);

// the default pictures (defaults.c): 80 x 80 JPEGs, shown when nothing has a cover
#define DEFAULT_PICTURES 3
extern const uint8_t *const default_picture[DEFAULT_PICTURES];
extern const unsigned default_picture_len[DEFAULT_PICTURES];
// the same whole and bigger (282 x 159), for the full screen cover
extern const uint8_t *const default_picture_big[DEFAULT_PICTURES];
extern const unsigned default_picture_big_len[DEFAULT_PICTURES];
