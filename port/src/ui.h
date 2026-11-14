// SPDX-License-Identifier: AGPL-3.0-only
// Minimal immediate-mode UI helpers with UTF-8 text and on-demand glyph cache.
#ifndef CHIAKI_BRICK_UI_H
#define CHIAKI_BRICK_UI_H

#include <SDL.h>

#define UI_SIZE_SMALL  22
#define UI_SIZE_NORMAL 30
#define UI_SIZE_LARGE  46

int  ui_init(SDL_Renderer *ren, const char *font_path);
void ui_shutdown(void);

/* Draw UTF-8 text, returns the advance width in pixels. */
int ui_text(int x, int y, int size, SDL_Color c, const char *fmt, ...);
int ui_text_width(int size, const char *text);
int ui_text_height(int size);

int ui_text_centered(int cx, int y, int size, SDL_Color c, const char *fmt, ...);
int ui_text_right(int rx, int y, int size, SDL_Color c, const char *fmt, ...);

void ui_rect(int x, int y, int w, int h, SDL_Color c);
void ui_frame(int x, int y, int w, int h, SDL_Color c, int thickness);

#endif
