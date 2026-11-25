#ifndef AH_TUI_TERMINAL_H
#define AH_TUI_TERMINAL_H
#include "graphics.h"
#include "tui/view/view.h"
#include <stdio.h>
#include <termios.h>

typedef struct {
    struct termios saved;
    char *up, *right, *erase;
    unsigned cursor_row;
    bool active, frame;
} tui_terminal;

typedef struct {
    char sequence[128], utf8[4];
    size_t sequence_len, utf8_len, utf8_need;
    uint64_t escape_at;
    bool paste, paste_overflow;
    bool control_overflow, control_escape;
    char *pasted;
    size_t paste_len, paste_capacity;
    tui_graphics *graphics;
} tui_input;

typedef void (*tui_key_sink)(void *, tui_key, const char *, size_t);

bool tui_terminal_open(tui_terminal *);
void tui_terminal_close(tui_terminal *);
void tui_terminal_size(unsigned *, unsigned *);
void tui_terminal_clear(tui_terminal *);
void tui_terminal_draw(tui_terminal *, const tui_frame *);
void tui_input_read(tui_input *, const char *, size_t, uint64_t, tui_key_sink, void *);
int tui_input_timeout(const tui_input *, uint64_t);
void tui_input_expire(tui_input *, uint64_t, tui_key_sink, void *);
void tui_safe_text(FILE *, const char *, size_t);
#endif
