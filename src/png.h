// png.h - write an RGB picture as a PNG file
#pragma once
#include <stdint.h>
#include <stdbool.h>

// rgb: w * h pixels 0x00RRGGBB; every pixel as sx x sy
bool png_write(const char *path, const uint32_t *rgb, int w, int h, int sx, int sy);
