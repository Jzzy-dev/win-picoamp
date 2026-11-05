// pages_stub.c - (temporary) the pages still to come
#include <stddef.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"

static void st_frame(void)
{
    gfx_clear(NAVY);
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    gfx_str(11, 6, "COMING SOON", 20, COL(YELLOW, NAVY));
}
static void st_key(int k) { if (k == K_ESC || k == K_BS || k == K_ENTER) ui_page(PG_PLAYER); else ui_playback_key(k); }
const page_t page_clock = { NULL, st_frame, st_key, NULL, NULL };
const page_t page_wx = { NULL, st_frame, st_key, NULL, NULL };
const page_t page_news = { NULL, st_frame, st_key, NULL, NULL };
void clock_service_tick(void) {}
