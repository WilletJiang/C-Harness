#include "markup.h"
#include "tui/terminal/terminal.h"
#include <string.h>

FILE *tui_markup_sink(const tui_markup_output *out) {
    return out->capture ? out->capture : out->file;
}

unsigned tui_markup_gutter(const tui_markup_output *out, char buffer[80]) {
    unsigned width = out->cols > 2 ? out->cols - 2 : 0;
    if (width > 79)
        width = 79;
    unsigned used = 0;
    if (!out->capture && width >= 4) {
        buffer[used++] = ' ';
        buffer[used++] = ' ';
    }
    for (unsigned i = 0; i < out->quote && used + 2 <= width; ++i) {
        buffer[used++] = '|';
        buffer[used++] = ' ';
    }
    for (unsigned i = 0; i < out->indent && used < width; ++i)
        buffer[used++] = ' ';
    buffer[used] = 0;
    return used;
}

void tui_markup_spacing(tui_markup_output *out) {
    if (out->separate && out->written)
        fputc('\n', out->file);
    out->separate = false;
}

void tui_markup_open_line(tui_markup_output *out) {
    if (out->line_open)
        return;
    char gutter[80];
    tui_markup_spacing(out);
    out->column = tui_markup_gutter(out, gutter);
    fputs(gutter, out->file);
    out->line_open = out->written = true;
}

void tui_markup_break(tui_markup_output *out) {
    if (out->capture) {
        fputc(' ', out->capture);
        return;
    }
    if (out->line_open)
        fputc('\n', out->file);
    out->line_open = out->gap = false;
    out->column = 0;
}

void tui_markup_separate(tui_markup_output *out) {
    if (!out->capture) {
        tui_markup_break(out);
        out->separate = true;
    }
}

static void spaces(FILE *out, unsigned count) {
    while (count--)
        fputc(' ', out);
}

static void write_glyphs(tui_markup_output *out, const char *text, size_t size) {
    for (size_t at = 0; at < size;) {
        unsigned width;
        size_t n = tui_markup_glyph(text + at, size - at, &width);
        bool at_start = !out->line_open;
        tui_markup_open_line(out);
        if (!at_start && out->column + width > out->cols) {
            tui_markup_break(out);
            tui_markup_open_line(out);
        }
        tui_safe_text(out->file, text + at, n);
        out->column += width;
        at += n;
    }
}

void tui_markup_word(tui_markup_output *out, const char *text, size_t size) {
    if (out->capture) {
        tui_safe_text(out->capture, text, size);
        return;
    }
    if (!size)
        return;
    size_t width = tui_markup_width(text, size);
    char gutter[80];
    unsigned prefix = tui_markup_gutter(out, gutter);
    unsigned gap = out->gap && out->line_open && out->column > prefix ? 1 : 0;
    if (out->line_open && out->column + gap + width > out->cols &&
        (gap || width <= out->cols - prefix)) {
        tui_markup_break(out);
        gap = 0;
    }
    tui_markup_open_line(out);
    out->gap = false;
    if (gap) {
        fputc(' ', out->file);
        ++out->column;
    }
    if (width <= out->cols && out->column <= out->cols - width) {
        tui_safe_text(out->file, text, size);
        out->column += (unsigned)width;
    } else {
        write_glyphs(out, text, size);
    }
}

void tui_markup_write(tui_markup_output *out, const char *text, size_t size, bool literal) {
    if (out->capture) {
        for (size_t at = 0; at < size;) {
            size_t n = 0;
            while (at + n < size && text[at + n] != '\n' && text[at + n] != '\t')
                ++n;
            tui_safe_text(out->capture, text + at, n);
            at += n;
            if (at < size)
                spaces(out->capture, text[at++] == '\t' ? 4 : 1);
        }
        return;
    }
    if (literal && out->gap) {
        if (out->line_open && out->column < out->cols) {
            fputc(' ', out->file);
            ++out->column;
        }
        out->gap = false;
    }
    for (size_t at = 0; at < size;) {
        char c = text[at];
        if (c == '\n') {
            if (literal)
                tui_markup_open_line(out);
            tui_markup_break(out);
            ++at;
        } else if (c == '\t' && literal) {
            char gutter[80];
            tui_markup_open_line(out);
            unsigned count = 4 - (out->column - tui_markup_gutter(out, gutter)) % 4;
            write_glyphs(out, "    ", count);
            ++at;
        } else if ((c == ' ' || c == '\t' || c == '\r') && !literal) {
            out->gap = true;
            ++at;
        } else {
            size_t end = at + 1;
            while (end < size && text[end] != '\n' && text[end] != '\t' &&
                   (literal || (text[end] != ' ' && text[end] != '\r')))
                ++end;
            if (literal)
                write_glyphs(out, text + at, end - at);
            else
                tui_markup_word(out, text + at, end - at);
            at = end;
        }
    }
}

void tui_markup_styled(tui_markup_output *out, const char *text, size_t size) {
    /* The table capture contains sanitized text and renderer-generated SGR. */
    bool literal = false;
    for (size_t at = 0; at < size;) {
        size_t n = tui_markup_sgr(text + at, size - at);
        if (n) {
            if (n == 4 && text[at + 2] == '0')
                literal = false;
            else if (n == 4 && text[at + 2] == '7')
                literal = true;
            fwrite(text + at, 1, n, out->file);
        } else {
            const char *next = memchr(text + at + 1, '\033', size - at - 1);
            n = next ? (size_t)(next - (text + at)) : size - at;
            tui_markup_write(out, text + at, n, literal);
        }
        at += n;
    }
}

bool tui_markup_formula(tui_markup_output *out, const char *text, size_t size, bool display) {
    if (!display || out->capture) {
        char gutter[80];
        if (!out->capture && tui_markup_width(text, size) >
                                 out->cols - tui_markup_gutter(out, gutter))
            return false;
        tui_markup_word(out, text, size);
        return true;
    }
    char gutter[80];
    unsigned available = out->cols - tui_markup_gutter(out, gutter);
    size_t maximum = 0;
    for (size_t at = 0; at < size;) {
        const char *newline = memchr(text + at, '\n', size - at);
        size_t n = newline ? (size_t)(newline - (text + at)) : size - at;
        size_t width = tui_markup_width(text + at, n);
        if (width > maximum)
            maximum = width;
        at += n + !!newline;
    }
    if (maximum > available)
        return false;
    unsigned left = (available - (unsigned)maximum) / 2;
    tui_markup_separate(out);
    for (size_t at = 0; at < size;) {
        const char *newline = memchr(text + at, '\n', size - at);
        size_t n = newline ? (size_t)(newline - (text + at)) : size - at;
        tui_markup_open_line(out);
        spaces(out->file, left);
        tui_safe_text(out->file, text + at, n);
        tui_markup_break(out);
        at += n + !!newline;
    }
    tui_markup_separate(out);
    return true;
}
