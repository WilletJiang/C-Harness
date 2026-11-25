#include "markup.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t offset, size, width;
    MD_ALIGN align;
} table_cell;

struct tui_markdown_table {
    FILE *stream;
    char *text;
    size_t bytes, offset, count, capacity;
    unsigned cols, headings;
    table_cell cells[];
};

tui_markdown_table *tui_table_create(const MD_BLOCK_TABLE_DETAIL *detail) {
    size_t rows = (size_t)detail->head_row_count + detail->body_row_count;
    if (!detail->col_count || detail->col_count > 128 || rows > 16384 / detail->col_count)
        return NULL;
    size_t capacity = rows * detail->col_count;
    tui_markdown_table *table = calloc(1, sizeof(*table) + capacity * sizeof(*table->cells));
    if (!table)
        return NULL;
    table->capacity = capacity;
    table->cols = detail->col_count;
    table->headings = detail->head_row_count;
    table->stream = open_memstream(&table->text, &table->bytes);
    if (!table->stream) {
        free(table);
        return NULL;
    }
    return table;
}

FILE *tui_table_cell(tui_markdown_table *table, MD_ALIGN align) {
    if (table->count >= table->capacity)
        return NULL;
    table->cells[table->count].align = align;
    return table->stream;
}

bool tui_table_end_cell(tui_markdown_table *table) {
    if (fflush(table->stream) != 0)
        return false;
    table_cell *cell = &table->cells[table->count++];
    cell->offset = table->offset;
    cell->size = table->bytes - table->offset;
    cell->width = tui_markup_styled_width(table->text + cell->offset, cell->size);
    table->offset = table->bytes;
    return true;
}

static void fill(FILE *out, size_t count, int ch) {
    while (count--)
        fputc(ch, out);
}

typedef struct {
    size_t at;
    unsigned style;
} cell_cursor;

static size_t cell_line(const char *text, const table_cell *cell, size_t start, size_t width,
                        size_t *next) {
    if (!start && cell->width <= width) {
        *next = cell->size;
        return cell->size;
    }
    size_t at = start, used = 0, split = start;
    while (at < cell->size) {
        size_t sgr = tui_markup_sgr(text + at, cell->size - at);
        if (sgr) {
            at += sgr;
            continue;
        }
        unsigned columns;
        size_t n = tui_markup_glyph(text + at, cell->size - at, &columns);
        if (text[at] == ' ' && used)
            split = at;
        if (used + columns > width)
            break;
        used += columns;
        at += n;
    }
    size_t end = at < cell->size && split > start ? split : at;
    *next = end;
    while (*next < cell->size && text[*next] == ' ')
        ++*next;
    return end;
}

static unsigned cell_style(const char *text, size_t length, unsigned active) {
    for (size_t at = 0; at < length;) {
        size_t n = tui_markup_sgr(text + at, length - at);
        if (n == 4 && text[at + 2] >= '0' && text[at + 2] <= '9') {
            unsigned code = (unsigned)(text[at + 2] - '0');
            active = code ? active | (1u << code) : 0;
        }
        at += n ? n : 1;
    }
    return active;
}

static bool print_cell(FILE *out, const tui_markdown_table *table, const table_cell *cell,
                       size_t width, cell_cursor *cursor) {
    const char *text = table->text + cell->offset;
    size_t next, end = cell_line(text, cell, cursor->at, width, &next);
    size_t bytes = end - cursor->at;
    size_t visible = !cursor->at && end == cell->size
                        ? cell->width : tui_markup_styled_width(text + cursor->at, bytes);
    size_t padding = width - visible;
    size_t left = cell->align == MD_ALIGN_RIGHT ? padding
                : cell->align == MD_ALIGN_CENTER ? padding / 2 : 0;
    fill(out, left, ' ');
    if (bytes) {
        for (unsigned code = 1; cursor->style && code <= 9; ++code)
            if (cursor->style & (1u << code))
                fprintf(out, "\033[%um", code);
        fwrite(text + cursor->at, 1, bytes, out);
        if (next < cell->size) {
            cursor->style = cell_style(text + cursor->at, bytes, cursor->style);
            if (cursor->style)
                fputs("\033[0m", out);
        }
    }
    fill(out, padding - left, ' ');
    cursor->at = next;
    return next < cell->size;
}

static void rule(FILE *out, const char *gutter, const size_t *widths, unsigned cols,
                 const char *left, const char *junction, const char *right) {
    fputs(gutter, out);
    fputs(left, out);
    for (unsigned i = 0; i < cols; ++i) {
        for (size_t j = 0; j < widths[i] + 2; ++j)
            fputs("─", out);
        fputs(i + 1 == cols ? right : junction, out);
    }
    fputc('\n', out);
}

static void print_rows(tui_markdown_table *table, FILE *out, const char *gutter,
                       const size_t *widths, bool wrapped) {
    unsigned cols = table->cols;
    rule(out, gutter, widths, cols, "┌", "┬", "┐");
    for (size_t row = 0; row < table->count / cols; ++row) {
        if (row && (row == table->headings || wrapped))
            rule(out, gutter, widths, cols, "├", "┼", "┤");
        cell_cursor cells[128] = {0};
        bool more;
        do {
            more = false;
            fputs(gutter, out);
            fputs("│ ", out);
            for (unsigned i = 0; i < cols; ++i) {
                table_cell *cell = &table->cells[row * cols + i];
                more |= print_cell(out, table, cell, widths[i], &cells[i]);
                fputs(i + 1 < cols ? " │ " : " │", out);
            }
            fputc('\n', out);
        } while (more);
    }
    rule(out, gutter, widths, cols, "└", "┴", "┘");
}

static void print_records(tui_markdown_table *table, tui_markup_output *out) {
    unsigned cols = table->cols;
    for (size_t row = table->headings; row < table->count / cols; ++row) {
        if (row > table->headings)
            tui_markup_separate(out);
        for (unsigned i = 0; i < cols; ++i) {
            const table_cell *header = &table->cells[i];
            const table_cell *cell = &table->cells[row * cols + i];
            if (header->width) {
                tui_markup_styled(out, table->text + header->offset, header->size);
                tui_markup_write(out, ": ", 2, true);
            }
            tui_markup_styled(out, table->text + cell->offset, cell->size);
            tui_markup_break(out);
        }
    }
}

static bool fit_columns(const tui_markdown_table *table, size_t available,
                        const size_t *natural, size_t *widths) {
    size_t used = 3 * table->cols + 1;
    for (unsigned i = 0; i < table->cols; ++i) {
        size_t preferred = table->cells[i].width;
        preferred = preferred < 4 ? 4 : preferred > 12 ? 12 : preferred;
        widths[i] = natural[i] < preferred ? natural[i] : preferred;
        used += widths[i];
    }
    if (used > available)
        return false;
    while (used < available) {
        unsigned best = 0;
        size_t remaining = 0;
        for (unsigned i = 0; i < table->cols; ++i) {
            size_t gap = natural[i] - widths[i];
            if (gap > remaining) {
                remaining = gap;
                best = i;
            }
        }
        if (!remaining)
            break;
        ++widths[best];
        ++used;
    }
    return true;
}

void tui_table_print(tui_markdown_table *table, tui_markup_output *out) {
    if (table->count < table->cols)
        return;
    size_t natural[128] = {0}, widths[128], needed = 3 * table->cols + 1;
    for (size_t i = 0; i < table->count; ++i) {
        size_t *col = &natural[i % table->cols];
        if (table->cells[i].width > *col)
            *col = table->cells[i].width;
    }
    for (unsigned i = 0; i < table->cols; ++i)
        needed += natural[i];
    char gutter[80];
    unsigned width = out->cols - tui_markup_gutter(out, gutter);
    if (needed <= width || fit_columns(table, width, natural, widths)) {
        tui_markup_spacing(out);
        print_rows(table, out->file, gutter, needed <= width ? natural : widths, needed > width);
        out->written = true;
    } else {
        if (table->count > (size_t)table->headings * table->cols) {
            print_records(table, out);
        } else {
            for (unsigned i = 0; i < table->cols; ++i) {
                table_cell *cell = &table->cells[i];
                tui_markup_styled(out, table->text + cell->offset, cell->size);
                tui_markup_break(out);
            }
        }
    }
}

void tui_table_destroy(tui_markdown_table *table) {
    if (!table)
        return;
    fclose(table->stream);
    free(table->text);
    free(table);
}
