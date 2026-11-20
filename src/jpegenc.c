// jpegenc.c - a small baseline JPEG encoder (see jpegenc.h): the covers the
// MSX link gives the MSX (80 x 80, as the engine gets them from its image
// proxy: baseline, 4:2:0, the standard Huffman tables).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "jpegenc.h"

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static const uint8_t q_lum[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,  12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,  14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77,  24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101,  72, 92, 95, 98,112,100,103, 99 };
static const uint8_t q_chr[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,  18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,  47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,  99, 99, 99, 99, 99, 99, 99, 99 };

// the standard Huffman tables (ITU T.81 K.3): counts of codes of 1..16 bits, then the values
static const uint8_t dc_lum_bits[16] = { 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const uint8_t dc_lum_val[12]  = { 0,1,2,3,4,5,6,7,8,9,10,11 };
static const uint8_t dc_chr_bits[16] = { 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const uint8_t dc_chr_val[12]  = { 0,1,2,3,4,5,6,7,8,9,10,11 };
static const uint8_t ac_lum_bits[16] = { 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d };
static const uint8_t ac_lum_val[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };
static const uint8_t ac_chr_bits[16] = { 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const uint8_t ac_chr_val[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa };

typedef struct { uint16_t code[256]; uint8_t len[256]; } huff;

typedef struct
{
    uint8_t *b;
    size_t   n, cap;
    bool     bad;
    uint32_t acc;            // bits not yet written (the high ones first)
    int      nacc;
} out_t;

static void put(out_t *o, uint8_t v)
{
    if (o->n == o->cap)
    {
        size_t c = o->cap ? o->cap * 2 : 4096;
        uint8_t *nb = realloc(o->b, c);
        if (!nb) { o->bad = true; return; }
        o->b = nb;
        o->cap = c;
    }
    o->b[o->n++] = v;
}

static void put16(out_t *o, unsigned v) { put(o, (uint8_t)(v >> 8)); put(o, (uint8_t)v); }

static void bits(out_t *o, unsigned code, int len)
{
    o->acc = (o->acc << len) | (code & ((1u << len) - 1));
    o->nacc += len;
    while (o->nacc >= 8)
    {
        uint8_t c = (uint8_t)(o->acc >> (o->nacc - 8));
        put(o, c);
        if (c == 0xFF) put(o, 0);                     // (a byte 0xFF in the data: stuffed)
        o->nacc -= 8;
    }
}

static void make_huff(huff *h, const uint8_t *nbits, const uint8_t *val)
{
    unsigned code = 0, k = 0;
    memset(h, 0, sizeof *h);
    for (int l = 1; l <= 16; l++)
    {
        for (int i = 0; i < nbits[l - 1]; i++, k++) { h->code[val[k]] = (uint16_t)code++; h->len[val[k]] = (uint8_t)l; }
        code <<= 1;
    }
}

static void dht(out_t *o, int cls_id, const uint8_t *nbits, const uint8_t *val)
{
    int n = 0;
    for (int i = 0; i < 16; i++) n += nbits[i];
    put16(o, 0xFFC4);
    put16(o, (unsigned)(2 + 1 + 16 + n));
    put(o, (uint8_t)cls_id);
    for (int i = 0; i < 16; i++) put(o, nbits[i]);
    for (int i = 0; i < n; i++) put(o, val[i]);
}

// the size class of v and its bits
static int cat(int v) { int a = v < 0 ? -v : v, c = 0; while (a) { c++; a >>= 1; } return c; }
static unsigned vbits(int v, int c) { return v < 0 ? (unsigned)(v + (1 << c) - 1) : (unsigned)v; }

static float cosv[8][8];

// one 8 x 8 block (values -128..127): DCT, quantized, coded; *dc the last DC of its component
static void block(out_t *o, const float in[64], const float qd[64], const huff *dch, const huff *ach, int *dc)
{
    float tmp[64];
    int q[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++)
        {
            float s = 0;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * cosv[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++)
        {
            float s = 0;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * cosv[v][y];
            float c = s / 4.0f * (u ? 1.0f : 0.70710678f) * (v ? 1.0f : 0.70710678f);
            q[v * 8 + u] = (int)lroundf(c / qd[v * 8 + u]);
        }
    int d = q[0] - *dc, c = cat(d);
    *dc = q[0];
    bits(o, dch->code[c], dch->len[c]);
    if (c) bits(o, vbits(d, c), c);
    int run = 0;
    for (int k = 1; k < 64; k++)
    {
        int v = q[zigzag[k]];
        if (!v) { run++; continue; }
        while (run > 15) { bits(o, ach->code[0xF0], ach->len[0xF0]); run -= 16; }
        c = cat(v);
        int sym = run << 4 | c;
        bits(o, ach->code[sym], ach->len[sym]);
        bits(o, vbits(v, c), c);
        run = 0;
    }
    if (run) bits(o, ach->code[0], ach->len[0]);       // the end of the block
}

uint8_t *jpeg_encode(const uint8_t *rgb, int w, int h, int stride, int quality, size_t *len)
{
    *len = 0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++) cosv[u][x] = cosf((2.0f * x + 1.0f) * u * 3.14159265f / 16.0f);
    int sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t ql[64], qc[64];
    float fl[64], fc[64];
    for (int i = 0; i < 64; i++)
    {
        int a = (q_lum[i] * sc + 50) / 100, b = (q_chr[i] * sc + 50) / 100;
        ql[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
        qc[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255 : b);
        fl[i] = ql[i];
        fc[i] = qc[i];
    }
    huff hdl, hdc, hal, hac;
    make_huff(&hdl, dc_lum_bits, dc_lum_val);
    make_huff(&hdc, dc_chr_bits, dc_chr_val);
    make_huff(&hal, ac_lum_bits, ac_lum_val);
    make_huff(&hac, ac_chr_bits, ac_chr_val);

    out_t o = { 0 };
    static const uint8_t jfif[] = { 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0 };
    put16(&o, 0xFFD8);
    for (size_t i = 0; i < sizeof jfif; i++) put(&o, jfif[i]);
    put16(&o, 0xFFDB); put16(&o, 67); put(&o, 0); for (int i = 0; i < 64; i++) put(&o, ql[zigzag[i]]);
    put16(&o, 0xFFDB); put16(&o, 67); put(&o, 1); for (int i = 0; i < 64; i++) put(&o, qc[zigzag[i]]);
    // the frame: 3 components, Y 2 x 2 (4:2:0)
    put16(&o, 0xFFC0); put16(&o, 17); put(&o, 8); put16(&o, (unsigned)h); put16(&o, (unsigned)w); put(&o, 3);
    put(&o, 1); put(&o, 0x22); put(&o, 0);
    put(&o, 2); put(&o, 0x11); put(&o, 1);
    put(&o, 3); put(&o, 0x11); put(&o, 1);
    dht(&o, 0x00, dc_lum_bits, dc_lum_val);
    dht(&o, 0x10, ac_lum_bits, ac_lum_val);
    dht(&o, 0x01, dc_chr_bits, dc_chr_val);
    dht(&o, 0x11, ac_chr_bits, ac_chr_val);
    put16(&o, 0xFFDA); put16(&o, 12); put(&o, 3);
    put(&o, 1); put(&o, 0x00); put(&o, 2); put(&o, 0x11); put(&o, 3); put(&o, 0x11);
    put(&o, 0); put(&o, 63); put(&o, 0);

    int dy = 0, dcb = 0, dcr = 0;
    for (int my = 0; my < h; my += 16)
        for (int mx = 0; mx < w; mx += 16)
        {
            // the MCU's pixels as Y, Cb, Cr (the edges repeated)
            float Y[256], cb[64] = { 0 }, cr[64] = { 0 };
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                {
                    int px = mx + x < w ? mx + x : w - 1, py = my + y < h ? my + y : h - 1;
                    const uint8_t *p = rgb + (size_t)py * (size_t)stride + (size_t)px * 3;
                    float r = p[0], g = p[1], b = p[2];
                    Y[y * 16 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    cb[(y / 2) * 8 + x / 2] += (-0.168736f * r - 0.331264f * g + 0.5f * b) / 4.0f;
                    cr[(y / 2) * 8 + x / 2] += (0.5f * r - 0.418688f * g - 0.081312f * b) / 4.0f;
                }
            for (int k = 0; k < 4; k++)
            {
                float blk[64];
                int ox = (k & 1) * 8, oy = (k >> 1) * 8;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) blk[y * 8 + x] = Y[(oy + y) * 16 + ox + x];
                block(&o, blk, fl, &hdl, &hal, &dy);
            }
            block(&o, cb, fc, &hdc, &hac, &dcb);
            block(&o, cr, fc, &hdc, &hac, &dcr);
        }
    if (o.nacc) bits(&o, 0x7F, 8 - o.nacc);           // (the last byte filled with ones)
    put16(&o, 0xFFD9);
    if (o.bad) { free(o.b); return NULL; }
    *len = o.n;
    return o.b;
}
