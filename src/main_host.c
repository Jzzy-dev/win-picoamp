// main_host.c - the test build: PicoAmp without a window, driven by a script
// (see script.h), taking screenshots. Sound goes to miniaudio's null device.
//
//   picoamp_test "wait 500; key Down; key Enter; wait 3000; shot out.png 3"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "script.h"
#include "sys.h"

void plat_open_folder(const char *path) { printf("open folder: %s\n", path); }
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
    audio_shutdown();
    return 0;
}
