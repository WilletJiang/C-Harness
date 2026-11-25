#include "markup.h"
#include <utf8proc.h>

static size_t decode(const char *text, size_t size, utf8proc_int32_t *cp) {
    utf8proc_ssize_t n = utf8proc_iterate((const unsigned char *)text,
                                         (utf8proc_ssize_t)(size < 4 ? size : 4), cp);
    if (n > 0)
        return (size_t)n;
    *cp = 0xfffd;
    return 1;
}

size_t tui_markup_glyph(const char *text, size_t size, unsigned *width) {
    if (!size) {
        *width = 0;
        return 0;
    }
    utf8proc_int32_t previous, state = 0;
    size_t n = decode(text, size, &previous);
    bool regional = previous >= 0x1f1e6 && previous <= 0x1f1ff;
    *width = previous < 32 || (previous >= 127 && previous <= 159)
                 ? 1 : (unsigned)utf8proc_charwidth(previous);
    while (n < size && (unsigned char)text[n] >= 128) {
        utf8proc_int32_t next;
        size_t count = decode(text + n, size - n, &next);
        if (utf8proc_grapheme_break_stateful(previous, next, &state))
            break;
        unsigned next_width = (unsigned)utf8proc_charwidth(next);
        if (regional || next == 0xfe0f || next == 0x20e3)
            next_width = 2;
        if (next_width > *width)
            *width = next_width;
        previous = next;
        n += count;
    }
    return n;
}

size_t tui_markup_sgr(const char *text, size_t size) {
    if (size < 3 || text[0] != '\033' || text[1] != '[')
        return 0;
    size_t end = 2;
    while (end < size && ((text[end] >= '0' && text[end] <= '9') || text[end] == ';'))
        ++end;
    return end < size && text[end] == 'm' ? end + 1 : 0;
}

static size_t measure(const char *text, size_t size, bool styled) {
    size_t result = 0;
    for (size_t i = 0; i < size;) {
        /* Table cells carry SGR emitted by the Markdown renderer. */
        size_t n = styled ? tui_markup_sgr(text + i, size - i) : 0;
        if (n) {
            i += n;
            continue;
        }
        unsigned width;
        i += tui_markup_glyph(text + i, size - i, &width);
        result += width;
    }
    return result;
}

size_t tui_markup_width(const char *text, size_t size) {
    return measure(text, size, false);
}

size_t tui_markup_styled_width(const char *text, size_t size) {
    return measure(text, size, true);
}
