// png.h - write an RGB picture as a PNG file
#pragma once
#include <stdint.h>
#include <stdbool.h>

// rgb: w * h pixels 0x00RRGGBB; scale: every pixel as scale x scale
bool png_write(const char *path, const uint32_t *rgb, int w, int h, int scale);
