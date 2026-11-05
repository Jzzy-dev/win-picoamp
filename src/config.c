// config.c - picoamp.ini (see config.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sys.h"
#include "util.h"

config cfg;

static void defaults(void)
{
    memset(&cfg, 0, sizeof cfg);
    cfg.volume = 80;
    cfg.balance = 50;
    u_copy(cfg.music_dir, sizeof cfg.music_dir, sys_music_dir());
    cfg.win_x = cfg.win_y = -1;
    cfg.win_w = 768;
    cfg.win_h = 576;
    cfg.scale = SCALE_FIT;
}

static const char *ini_path(void)
{
    static char p[1100];
    u_join(p, sizeof p, sys_config_dir(), "picoamp.ini");
    return p;
}

const char *config_radio_dir(void)
{
    static char p[1100];
    u_join(p, sizeof p, sys_config_dir(), "Radio");
    return p;
}

static bool yes(const char *v) { return !u_stricmp(v, "1") || !u_stricmp(v, "yes") || !u_stricmp(v, "on") || !u_stricmp(v, "true"); }

void config_load(void)
{
    defaults();
    char *text = sys_read_file(ini_path(), NULL);
    if (!text) { cfg.first_run = true; return; }
    char *cur = text;
    for (char *line; (line = u_token(&cur, "\r\n")); )
    {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        *eq = 0;
        char *k = u_trim(line), *v = u_trim(eq + 1);
        if (!u_stricmp(k, "volume")) cfg.volume = atoi(v);
        else if (!u_stricmp(k, "repeat")) cfg.mode = (cfg.mode & ~1) | (yes(v) ? 1 : 0);
        else if (!u_stricmp(k, "shuffle")) cfg.mode = (cfg.mode & ~2) | (yes(v) ? 2 : 0);
        else if (!u_stricmp(k, "eq"))
        {
            char *p = v;
            for (int i = 0; i < EQ_BANDS && *p; i++)
            {
                int g = (int)strtol(p, &p, 10);
                if (g > EQ_MAX_DB) g = EQ_MAX_DB;
                if (g < -EQ_MAX_DB) g = -EQ_MAX_DB;
                cfg.eq[i] = (int8_t)g;
                while (*p == ',' || *p == ' ') p++;
            }
        }
        else if (!u_stricmp(k, "balance")) { cfg.balance = atoi(v); if (cfg.balance < 0 || cfg.balance > 100) cfg.balance = 50; }
        else if (!u_stricmp(k, "view")) cfg.view = !u_stricmp(v, "scope") ? 1 : 0;
        else if (!u_stricmp(k, "music")) { if (*v) u_copy(cfg.music_dir, sizeof cfg.music_dir, v); }
        else if (!u_stricmp(k, "folder")) u_copy(cfg.start_path, sizeof cfg.start_path, v);
        else if (!u_stricmp(k, "window_x")) cfg.win_x = atoi(v);
        else if (!u_stricmp(k, "window_y")) cfg.win_y = atoi(v);
        else if (!u_stricmp(k, "window_w")) cfg.win_w = atoi(v);
        else if (!u_stricmp(k, "window_h")) cfg.win_h = atoi(v);
        else if (!u_stricmp(k, "maximized")) cfg.maximized = yes(v);
        else if (!u_stricmp(k, "fullscreen")) cfg.fullscreen = yes(v);
        else if (!u_stricmp(k, "scale")) cfg.scale = !u_stricmp(v, "integer") ? SCALE_INTEGER : !u_stricmp(v, "stretch") ? SCALE_STRETCH : SCALE_FIT;
        else if (!u_stricmp(k, "scanlines")) cfg.scanlines = yes(v);
        else if (!u_stricmp(k, "on_top")) cfg.on_top = yes(v);
    }
    free(text);
    if (cfg.volume < 0) cfg.volume = 0;
    if (cfg.volume > 100) cfg.volume = 100;
    if (cfg.win_w < 256 || cfg.win_h < 192) { cfg.win_w = 768; cfg.win_h = 576; }
}

void config_save(void)
{
    sys_mkdir(sys_config_dir());
    char buf[8192];
    static const char *scale_names[] = { "fit", "integer", "stretch" };
    int n = snprintf(buf, sizeof buf,
        "; PicoAmp for Windows - settings (this file is written when PicoAmp closes)\r\n"
        "[player]\r\n"
        "volume = %d\r\nrepeat = %s\r\nshuffle = %s\r\n"
        "; equalizer: 60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz in dB (-12..12)\r\n"
        "eq = %d,%d,%d,%d,%d\r\n"
        "; balance: 0 left, 50 centre, 100 right\r\n"
        "balance = %d\r\n"
        "view = %s\r\n"
        "; the folder shown as [Music] in the list\r\n"
        "music = %s\r\n"
        "folder = %s\r\n"
        "[window]\r\n"
        "window_x = %d\r\nwindow_y = %d\r\nwindow_w = %d\r\nwindow_h = %d\r\n"
        "maximized = %s\r\nfullscreen = %s\r\n"
        "; fit: as large as fits, 4:3, sharp; integer: whole multiples only; stretch: fill the window\r\n"
        "scale = %s\r\nscanlines = %s\r\non_top = %s\r\n",
        cfg.volume, (cfg.mode & 1) ? "yes" : "no", (cfg.mode & 2) ? "yes" : "no",
        cfg.eq[0], cfg.eq[1], cfg.eq[2], cfg.eq[3], cfg.eq[4], cfg.balance,
        cfg.view ? "scope" : "spectrum", cfg.music_dir, cfg.start_path,
        cfg.win_x, cfg.win_y, cfg.win_w, cfg.win_h,
        cfg.maximized ? "yes" : "no", cfg.fullscreen ? "yes" : "no",
        scale_names[cfg.scale % 3], cfg.scanlines ? "yes" : "no", cfg.on_top ? "yes" : "no");
    if (n > 0) sys_write_file(ini_path(), buf, (size_t)n);
}
