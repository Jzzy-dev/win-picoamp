// jpegenc.h - a small baseline JPEG encoder (4:2:0, the standard Huffman tables)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// RGB rows (3 bytes a pixel) -> JPEG bytes (malloc'd), quality 1..100; NULL: no memory
uint8_t *jpeg_encode(const uint8_t *rgb, int w, int h, int stride, int quality, size_t *len);
