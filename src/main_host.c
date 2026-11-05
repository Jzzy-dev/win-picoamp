// main_host.c - the test build: PicoAmp without a window, driven by a script,
// taking screenshots. Sound goes to miniaudio's null device (real time).
//
//   picoamp_test "wait 500; key Down; key Enter; wait 3000; shot out.png"
//
// commands: wait <ms> | key <char or name> | click <x> <y> [right] |
//           drop <path> | shot <file.png> [scale] | status
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "png.h"
#include "sys.h"
#include "util.h"

void plat_open_folder(const char *path) { printf("open folder: %s\n", path); }
void plat_quit(void) {}

static void frames(unsigned ms)
{
    uint32_t t0 = sys_millis();
    do { ui_frame(); sys_sleep_ms(16); } while (sys_millis() - t0 < ms);
}

static int key_code(const char *k)
{
    static const struct { const char *n; int c; } names[] = {
        { "Up", K_UP }, { "Down", K_DOWN }, { "Left", K_LEFT }, { "Right", K_RIGHT }, { "Enter", K_ENTER },
        { "BS", K_BS }, { "Esc", K_ESC }, { "Space", ' ' }, { "Del", K_DEL }, { "Home", K_HOME }, { "End", K_END },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!u_stricmp(k, names[i].n)) return names[i].c;
    return (unsigned char)k[0];
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s \"script\"\n", argv[0]); return 2; }
    net_init();
    gfx_init();
    config_load();
    audio_init(true);
    ui_init();
    char *script = strdup(argv[1]);
    char *cur = script;
    for (char *cmd; (cmd = u_token(&cur, ";\n")); )
    {
        char a[1024] = "", c[64] = "";
        cmd = u_trim(cmd);
        if (!*cmd) continue;
        char *sp = strchr(cmd, ' ');
        if (sp) { *sp = 0; u_copy(a, sizeof a, u_trim(sp + 1)); }
        if (!strcmp(cmd, "wait")) frames((unsigned)atoi(a));
        else if (!strcmp(cmd, "key")) { ui_key(key_code(a)); frames(20); }
        else if (!strcmp(cmd, "click"))
        {
            int mx = 0, my = 0;
            sscanf(a, "%d %d %63s", &mx, &my, c);
            ui_mouse(mx, my, !strcmp(c, "right") ? MOUSE_RIGHT : MOUSE_LEFT);
            frames(20);
        }
        else if (!strcmp(cmd, "drop")) { const char *p[1] = { a }; ui_drop(p, 1); frames(20); }
        else if (!strcmp(cmd, "shot"))
        {
            int scale = 1;
            char *s2 = strchr(a, ' ');
            if (s2) { *s2 = 0; scale = atoi(s2 + 1); }
            ui_frame();
            static uint32_t rgb[GFX_W * GFX_H];
            gfx_to_rgb(rgb);
            printf("shot %s: %s\n", a, png_write(a, rgb, GFX_W, GFX_H, scale) ? "ok" : "FAILED");
        }
        else if (!strcmp(cmd, "status"))
        {
            audio_status s;
            audio_status_get(&s);
            printf("state=%d elapsed=%d/%d rate=%d ch=%d br=%d stream=%d buf=%d(%d%%) pl=%d/%d vol=%d title='%s' artist='%s' album='%s' err='%s'\n",
                   s.state, s.elapsed, s.total, s.rate, s.channels, s.bitrate, s.stream, s.buffering, s.buf_pct,
                   s.pl_pos + 1, s.pl_count, s.volume, s.title, s.artist, s.album, s.error);
        }
        else fprintf(stderr, "unknown command %s\n", cmd);
        fflush(stdout);
    }
    free(script);
    ui_save_state();
    config_save();
    audio_shutdown();
    return 0;
}
