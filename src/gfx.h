// gfx.h - the MSX2 SCREEN 4 look: a 256 x 192 picture made of 32 x 24 cells
// of 8 x 8 pixels, every 8-pixel row of a cell one foreground and one
// background colour, 16 colours from PICOAMP's palette.
//
// A colour byte is fg * 16 + bg, as in the VDP's colour table.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GFX_W 256
#define GFX_H 192

// palette indices (PICOAMP's own palette)
enum {
    BLK = 1, BLUE_D, BLUE, CYAN, WHITE, GREY, GREEN_D, GREEN, YELLOW, ORANGE,
    RED, MAGENTA, GREY_D, NAVY, GREY_L
};
#define COL(fg, bg) ((uint8_t)((fg) * 16 + (bg)))

extern uint8_t gfx_fb[GFX_H][GFX_W];      // palette indices
extern uint32_t gfx_pal[16];               // 0x00RRGGBB

void gfx_init(void);
void gfx_clear(uint8_t colour);            // the whole screen, one colour index
void gfx_cell(int row, int col, const uint8_t pat[8], uint8_t colour);
void gfx_cell2(int row, int col, const uint8_t pat[8], const uint8_t colours[8]);
void gfx_char(int row, int col, unsigned ch, uint8_t colour);
// text in 8-pixel cells, padded with spaces to width cells (UTF-8 in)
void gfx_str(int row, int col, const char *text, int width, uint8_t colour);
void gfx_fill(int row, int col, int cells, uint8_t colour);
// 6-pixel text (42 characters a row) in width cells; off = pixels scrolled
// left; cycle > 0: the text again cycle pixels further (a ticker)
void gfx_text6(int row, int col, int width, const char *text, uint8_t colour, int off, int cycle);
int  gfx_text6_px(const char *text);       // width in pixels (6 a character)
void gfx_digit(int row, int col, int d, uint8_t colour);   // 2 x 3 cells
void gfx_colon(int row, int col, uint8_t colour);          // 1 x 3 cells
void gfx_pset(int x, int y, uint8_t index);
void gfx_hline(int x, int y, int w, uint8_t index);
void gfx_rect(int x, int y, int w, int h, uint8_t index);

// UTF-8 -> the MSX character set (CP437-like accents, '?' for the rest);
// returns the number of characters written (out has room for max-1 and a 0)
int  gfx_to_msx(const char *utf8, uint8_t *out, int max);

// the picture as 0x00RRGGBB pixels (GFX_W * GFX_H)
void gfx_to_rgb(uint32_t *out);
