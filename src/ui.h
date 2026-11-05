// ui.h - PICOAMP's screen and keys, drawn into the gfx picture
#pragma once
#include <stdbool.h>

// keys: printable ones as their character; these for the rest
enum
{
    K_ENTER = 13, K_BS = 8, K_ESC = 27,
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END, K_DEL,
    K_MEDIA_PLAY, K_MEDIA_STOP, K_MEDIA_NEXT, K_MEDIA_PREV, K_VOL_UP, K_VOL_DOWN, K_VOL_MUTE
};
enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_WHEEL_UP = 4, MOUSE_WHEEL_DOWN = 5, MOUSE_DRAG = 6, MOUSE_UP = 7 };

void ui_init(void);
void ui_frame(void);                      // once a frame: read the player, draw
void ui_key(int key);
void ui_mouse(int x, int y, int button);  // x, y in the 256 x 192 picture
void ui_drop(const char *const *paths, int n);   // files dropped on the window
void ui_message(const char *text, bool error);
bool ui_quit_requested(void);
void ui_save_state(void);                 // into cfg before it is saved

// from the platform
void plat_open_folder(const char *path);
void plat_quit(void);
