#ifndef AH_TUI_MARKUP_H
#define AH_TUI_MARKUP_H
#include <md4c.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define TUI_MD_FLAGS (MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | \
                      MD_FLAG_LATEXMATHSPANS | MD_FLAG_NOHTML)
#define TUI_MATH_BYTES 4096
#define TUI_MATH_MARK '\x1f'

typedef struct {
    size_t offset;
    const char *source;
    size_t size;
    unsigned quote;
    bool display;
} tui_math_region;

typedef struct {
    char *text;
    tui_math_region *regions;
    size_t size, count;
} tui_math_source;

tui_math_source tui_math_prepare(const char *, size_t);
const char *tui_math_region_text(const tui_math_region *, char buffer[TUI_MATH_BYTES],
                                 size_t *);
void tui_math_source_free(tui_math_source *);
ptrdiff_t ah_tui_math(const char *, size_t, bool display, unsigned width, char *, size_t);

size_t tui_markup_glyph(const char *, size_t, unsigned *width);
size_t tui_markup_width(const char *, size_t);
size_t tui_markup_styled_width(const char *, size_t);
size_t tui_markup_sgr(const char *, size_t);

typedef struct {
    FILE *file, *capture;
    unsigned cols, column, quote, indent;
    bool line_open, written, separate, gap;
} tui_markup_output;

FILE *tui_markup_sink(const tui_markup_output *);
unsigned tui_markup_gutter(const tui_markup_output *, char buffer[80]);
void tui_markup_open_line(tui_markup_output *);
void tui_markup_break(tui_markup_output *);
void tui_markup_separate(tui_markup_output *);
void tui_markup_spacing(tui_markup_output *);
void tui_markup_write(tui_markup_output *, const char *, size_t, bool literal);
void tui_markup_styled(tui_markup_output *, const char *, size_t);
void tui_markup_word(tui_markup_output *, const char *, size_t);
bool tui_markup_formula(tui_markup_output *, const char *, size_t, bool display);

typedef struct tui_markdown_table tui_markdown_table;
tui_markdown_table *tui_table_create(const MD_BLOCK_TABLE_DETAIL *);
FILE *tui_table_cell(tui_markdown_table *, MD_ALIGN);
bool tui_table_end_cell(tui_markdown_table *);
void tui_table_print(tui_markdown_table *, tui_markup_output *);
void tui_table_destroy(tui_markdown_table *);
#endif
