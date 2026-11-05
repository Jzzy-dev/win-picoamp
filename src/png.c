// png.c - write an RGB picture as a PNG (stored deflate blocks: no zlib needed)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "png.h"
#include "sys.h"

static uint32_t crc_table[256];

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc((const uint8_t *)type, 4, 0);
    c = crc(data, n, c);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

bool png_write(const char *path, const uint32_t *rgb, int w, int h, int sx, int sy)
{
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    int W = w * sx, H = h * sy;
    size_t row = (size_t)W * 3 + 1, raw_n = row * (size_t)H;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < H; y++)
    {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < W; x++)
        {
            uint32_t p = rgb[(y / sy) * w + x / sx];
            *r++ = (uint8_t)(p >> 16); *r++ = (uint8_t)(p >> 8); *r++ = (uint8_t)p;
        }
    }
    // zlib: header, stored blocks of up to 65535 bytes, adler32
    size_t blocks = raw_n / 65535 + 1;
    size_t zn = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(zn), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw_n || p == 0; )
    {
        size_t k = raw_n - p > 65535 ? 65535 : raw_n - p;
        *o++ = (uint8_t)(p + k >= raw_n);
        *o++ = (uint8_t)k; *o++ = (uint8_t)(k >> 8);
        *o++ = (uint8_t)~k; *o++ = (uint8_t)(~k >> 8);
        memcpy(o, raw + p, k);
        o += k;
        p += k;
        if (!k) break;
    }
    be32(o, (b << 16) | a);
    o += 4;
    FILE *f = sys_fopen(path, "wb");
    if (!f) { free(raw); free(z); return false; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (size_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    bool ok = fclose(f) == 0;
    free(raw);
    free(z);
    return ok;
}
