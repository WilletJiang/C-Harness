#ifndef AH_TUI_MARKDOWN_H
#define AH_TUI_MARKDOWN_H
#include "tui/terminal/graphics.h"
#include <stddef.h>
#include <stdio.h>

void tui_markdown(FILE *, const char *, size_t, unsigned);
void tui_markdown_graphics(FILE *, const char *, size_t, unsigned, const tui_graphics *);
#endif
