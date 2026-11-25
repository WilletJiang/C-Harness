#ifndef AH_TUI_GRAPHICS_H
#define AH_TUI_GRAPHICS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    unsigned cols, rows, cell_width, cell_height;
    uint32_t foreground, background;
    bool probing, kitty, have_foreground, have_background, fixed_ink;
    uint64_t probe_until;
} tui_graphics;

typedef struct {
    unsigned char *png;
    size_t size;
    unsigned rows, left, offset_x;
} tui_graphic;

void tui_graphics_start(tui_graphics *, unsigned cols, unsigned rows, uint64_t now);
void tui_graphics_resize(tui_graphics *, unsigned cols, unsigned rows);
void tui_graphics_colors(tui_graphics *);
int tui_graphics_wait(const tui_graphics *, uint64_t now);
void tui_graphics_reply(tui_graphics *, const char *sequence);
bool tui_graphics_render(const tui_graphics *, const char *, size_t, unsigned gutter,
                         unsigned available, tui_graphic *);
void tui_graphics_place(FILE *, tui_graphic *, const char *gutter);
#endif
