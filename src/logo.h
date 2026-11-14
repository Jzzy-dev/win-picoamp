// logo.h - a station's logo as its cover (Radio Browser's "favicon", kept in
// <config>/Logos), as the MSX engine 0.15 finds it.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// a station starts: its address, its directory id (or NULL), its name
void logo_station(const char *url, const char *uuid, const char *name);
void logo_cancel(void);
// the result, once: true and *data (malloc'd picture bytes, or NULL: no logo)
bool logo_take(uint8_t **data, size_t *len);
