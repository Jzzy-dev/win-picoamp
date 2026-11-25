// main_host.c - the test build: PicoAmp without a window, driven by a script
// (see script.h), taking screenshots. Sound goes to miniaudio's null device.
//
//   picoamp_test "wait 500; key Down; key Enter; wait 3000; shot out.png 3"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"
#include "msxlink.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "script.h"
#include "sys.h"
#include "cover.h"
#include "picture.h"

void plat_open_folder(const char *path) { printf("open folder: %s\n", path); }
// (the test build: says what it would open; "spotify:" is not there, as on a PC without the app)
bool plat_open_url(const char *url) { printf("open url: %s\n", url); fflush(stdout); return strncmp(url, "spotify:", 8) != 0; }
void plat_quit(void) {}
void plat_minimize(void) { printf("minimize\n"); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s \"script\"\n", argv[0]); return 2; }
    net_init();
    gfx_init();
    config_load();
    audio_init(true);
    ui_init();
    script_start(argv[1], stdout);
    while (script_step())
    {
        ui_frame();
        sys_sleep_ms(16);
    }
    ui_save_state();
    config_save();
    msxlink_stop();
    audio_shutdown();
    return 0;
}

// covers: no decoder of its own here (TJpgDec does the JPEGs)
bool plat_decode_image(const uint8_t *data, size_t len) { (void)data; (void)len; return false; }
uint8_t *plat_decode_rgb(const uint8_t *data, size_t len, int max_w, int max_h, int *w, int *h) { (void)data; (void)len; (void)max_w; (void)max_h; (void)w; (void)h; return NULL; }
