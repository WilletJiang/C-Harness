#include "markdown.h"
#include "markup.h"
#include "tui/terminal/terminal.h"
#include <entity.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <utf8proc.h>

enum { LIST_LEVELS = 24 };

typedef struct {
    unsigned number, marker;
    bool ordered;
} list_level;

typedef struct {
    tui_markup_output out;
    const tui_graphics *graphics;
    tui_markdown_table *table;
    tui_math_source math_source;
    size_t math_bytes, next_region;
    char math[TUI_MATH_BYTES];
    list_level list[LIST_LEVELS];
    unsigned depth, bold, italic, underline, strike, inline_code;
    bool code, display, in_math, math_overflow;
} markdown;

static void style(markdown *md) {
    FILE *out = tui_markup_sink(&md->out);
    fputs("\033[0m", out);
    if (md->code)
        return;
    if (md->inline_code) {
        fputs("\033[38;5;208m", out);
        return;
    }
    if (md->underline)
        fputs("\033[4;38;5;208m", out);
    else if (md->bold)
        fputs("\033[1m", out);
    if (md->italic)
        fputs("\033[3m", out);
    if (md->strike)
        fputs("\033[9;2m", out);
}

static void write_text(markdown *md, const char *s, size_t size) {
    tui_markup_write(&md->out, s, size, md->code || md->inline_code);
}

static unsigned list_indent(const markdown *md) {
    unsigned indent = 0;
    for (unsigned i = 0; i < md->depth && i < LIST_LEVELS; ++i)
        indent += md->list[i].marker;
    return indent;
}

static void list_item(markdown *md, const MD_BLOCK_LI_DETAIL *detail) {
    tui_markup_break(&md->out);
    if (md->depth && md->depth <= LIST_LEVELS)
        md->list[md->depth - 1].marker = 0;
    md->out.indent = list_indent(md);
    char marker[32];
    size_t size;
    list_level *level = md->depth && md->depth <= LIST_LEVELS ? &md->list[md->depth - 1] : NULL;
    if (level && level->ordered)
        size = (size_t)snprintf(marker, sizeof(marker), "%u. ", level->number++);
    else {
        memcpy(marker, "- ", 2);
        size = 2;
    }
    if (detail->is_task) {
        memcpy(marker + size, detail->task_mark == ' ' ? "[ ] " : "[x] ", 4);
        size += 4;
    }
    tui_markup_write(&md->out, marker, size, true);
    if (level)
        level->marker = (unsigned)size;
    md->out.indent = list_indent(md);
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *opaque) {
    markdown *md = opaque;
    switch (type) {
    case MD_BLOCK_QUOTE: tui_markup_separate(&md->out); ++md->out.quote; break;
    case MD_BLOCK_H: tui_markup_separate(&md->out); ++md->bold; style(md); break;
    case MD_BLOCK_HR:
        tui_markup_separate(&md->out);
        write_text(md, "---", 3);
        tui_markup_separate(&md->out);
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        if (md->depth < LIST_LEVELS)
            md->list[md->depth] = (list_level){
                .ordered = type == MD_BLOCK_OL,
                .number = type == MD_BLOCK_OL ? ((MD_BLOCK_OL_DETAIL *)detail)->start : 0,
            };
        ++md->depth;
        break;
    case MD_BLOCK_LI: list_item(md, detail); break;
    case MD_BLOCK_CODE:
        tui_markup_separate(&md->out);
        md->code = true;
        md->out.indent += 2;
        style(md);
        break;
    case MD_BLOCK_TABLE:
        tui_markup_separate(&md->out);
        md->table = tui_table_create(detail);
        break;
    case MD_BLOCK_TR:
        if (!md->table)
            write_text(md, "| ", 2);
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (md->table) {
            md->out.capture = tui_table_cell(md->table, ((MD_BLOCK_TD_DETAIL *)detail)->align);
            if (!md->out.capture)
                return 1;
        }
        if (type == MD_BLOCK_TH) {
            ++md->bold;
            style(md);
        }
        break;
    default: break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *opaque) {
    (void)detail;
    markdown *md = opaque;
    switch (type) {
    case MD_BLOCK_QUOTE: --md->out.quote; tui_markup_separate(&md->out); break;
    case MD_BLOCK_H: --md->bold; style(md); tui_markup_separate(&md->out); break;
    case MD_BLOCK_P:
        if (md->depth)
            tui_markup_break(&md->out);
        else
            tui_markup_separate(&md->out);
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        --md->depth;
        md->out.indent = list_indent(md);
        if (!md->depth)
            tui_markup_separate(&md->out);
        break;
    case MD_BLOCK_LI:
    case MD_BLOCK_TR: tui_markup_break(&md->out); break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (type == MD_BLOCK_TH) {
            --md->bold;
            style(md);
        }
        if (md->table) {
            md->out.capture = NULL;
            if (!tui_table_end_cell(md->table))
                return 1;
        } else {
            write_text(md, " | ", 3);
        }
        break;
    case MD_BLOCK_TABLE:
        if (md->table) {
            tui_table_print(md->table, &md->out);
            tui_table_destroy(md->table);
            md->table = NULL;
        }
        tui_markup_separate(&md->out);
        break;
    case MD_BLOCK_CODE:
        md->code = false;
        md->out.indent -= 2;
        style(md);
        tui_markup_separate(&md->out);
        break;
    default: break;
    }
    return 0;
}

static bool math_span(MD_SPANTYPE type) {
    return type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY;
}

static void math_fallback(markdown *md, const char *source, size_t size, bool display) {
    if (display)
        tui_markup_separate(&md->out);
    tui_markup_write(&md->out, source, size, true);
    if (display)
        tui_markup_separate(&md->out);
}

static bool draw_math(markdown *md, const char *text, size_t size, bool display) {
    char rendered[8192], gutter[80];
    unsigned inset = tui_markup_gutter(&md->out, gutter);
    unsigned width = md->out.cols - inset;
    tui_graphic image;
    if (display && !md->out.capture &&
        tui_graphics_render(md->graphics, text, size, inset, width, &image)) {
        tui_markup_separate(&md->out);
        tui_markup_spacing(&md->out);
        tui_graphics_place(md->out.file, &image, gutter);
        md->out.written = true;
        tui_markup_separate(&md->out);
        return true;
    }
    ptrdiff_t n = ah_tui_math(text, size, display && !md->out.capture, width,
                              rendered, sizeof(rendered));
    return n > 0 && tui_markup_formula(&md->out, rendered, (size_t)n, display);
}

static const char *marks(const markdown *md) {
    return md->display ? "$$" : "$";
}

static unsigned *span_count(markdown *md, MD_SPANTYPE type) {
    switch (type) {
    case MD_SPAN_STRONG: return &md->bold;
    case MD_SPAN_EM: return &md->italic;
    case MD_SPAN_A: return &md->underline;
    case MD_SPAN_DEL: return &md->strike;
    case MD_SPAN_CODE: return &md->inline_code;
    default: return NULL;
    }
}

static int enter_span(MD_SPANTYPE type, void *detail, void *opaque) {
    (void)detail;
    markdown *md = opaque;
    if (math_span(type)) {
        md->in_math = true;
        md->display = type == MD_SPAN_LATEXMATH_DISPLAY;
        md->math_bytes = 0;
        md->math_overflow = false;
    } else {
        unsigned *count = span_count(md, type);
        if (count) {
            ++*count;
            style(md);
        }
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *opaque) {
    markdown *md = opaque;
    if (math_span(type)) {
        if (!md->math_overflow && !draw_math(md, md->math, md->math_bytes, md->display)) {
            if (md->display)
                tui_markup_separate(&md->out);
            tui_markup_write(&md->out, marks(md), strlen(marks(md)), true);
            tui_markup_write(&md->out, md->math, md->math_bytes, true);
            md->math_overflow = true;
        }
        if (md->math_overflow) {
            tui_markup_write(&md->out, marks(md), strlen(marks(md)), true);
            if (md->display)
                tui_markup_separate(&md->out);
        }
        md->in_math = false;
    } else {
        unsigned *count = span_count(md, type);
        if (count) {
            --*count;
            style(md);
        }
        if (type == MD_SPAN_A) {
            const MD_SPAN_A_DETAIL *link = detail;
            if (link->href.size && !link->is_autolink) {
                write_text(md, " (", 2);
                write_text(md, link->href.text, link->href.size);
                write_text(md, ")", 1);
            }
        }
    }
    return 0;
}

static void collect_math(markdown *md, const char *text, size_t size) {
    if (!md->math_overflow && size <= sizeof(md->math) - md->math_bytes) {
        memcpy(md->math + md->math_bytes, text, size);
        md->math_bytes += size;
        return;
    }
    if (!md->math_overflow) {
        if (md->display)
            tui_markup_separate(&md->out);
        tui_markup_write(&md->out, marks(md), strlen(marks(md)), true);
        tui_markup_write(&md->out, md->math, md->math_bytes, true);
        md->math_overflow = true;
    }
    tui_markup_write(&md->out, text, size, true);
}

static bool math_markers(markdown *md, const char *text, size_t size) {
    tui_math_source *source = &md->math_source;
    if (!source->text || md->next_region >= source->count || !memchr(text, TUI_MATH_MARK, size))
        return false;
    uintptr_t base = (uintptr_t)source->text, ptr = (uintptr_t)text;
    if (ptr < base || ptr - base >= source->size || size > source->size - (size_t)(ptr - base))
        return false;
    size_t start = (size_t)(ptr - base), end = start + size;
    while (md->next_region < source->count && source->regions[md->next_region].offset < end) {
        const tui_math_region *region = &source->regions[md->next_region++];
        if (region->offset < start)
            continue;
        write_text(md, source->text + start, region->offset - start);
        char math[TUI_MATH_BYTES];
        size_t size;
        const char *formula = tui_math_region_text(region, math, &size);
        if (!draw_math(md, formula + 2, size - 4, region->display))
            math_fallback(md, formula, size, region->display);
        start = region->offset + 1;
    }
    write_text(md, source->text + start, end - start);
    return true;
}

static void codepoint(markdown *md, unsigned value) {
    unsigned char bytes[4];
    utf8proc_ssize_t n = utf8proc_codepoint_valid((utf8proc_int32_t)value) && value
                           ? utf8proc_encode_char((utf8proc_int32_t)value, bytes) : 0;
    if (n > 0)
        write_text(md, (const char *)bytes, (size_t)n);
    else
        write_text(md, "\xef\xbf\xbd", 3);
}

static void entity(markdown *md, const char *s, size_t size) {
    if (size > 3 && s[1] == '#') {
        unsigned value = 0;
        unsigned base = (s[2] == 'x' || s[2] == 'X') ? 16 : 10;
        for (size_t i = base == 16 ? 3 : 2; i + 1 < size && value <= 0x10ffff; ++i) {
            unsigned char ch = (unsigned char)s[i];
            unsigned digit = ch >= 'a' ? ch - 'a' + 10 : ch >= 'A' ? ch - 'A' + 10 : ch - '0';
            value = value * base + digit;
        }
        codepoint(md, value);
        return;
    }
    const ENTITY *named = entity_lookup(s, size);
    if (!named) {
        write_text(md, s, size);
        return;
    }
    codepoint(md, named->codepoints[0]);
    if (named->codepoints[1])
        codepoint(md, named->codepoints[1]);
}

static int md_text(MD_TEXTTYPE type, const char *text, MD_SIZE size, void *opaque) {
    markdown *md = opaque;
    if (md->in_math) {
        collect_math(md, text, size);
    } else {
        switch (type) {
        case MD_TEXT_SOFTBR: write_text(md, " ", 1); break;
        case MD_TEXT_BR: tui_markup_break(&md->out); break;
        case MD_TEXT_ENTITY: entity(md, text, size); break;
        case MD_TEXT_NULLCHAR: codepoint(md, 0); break;
        default:
            if (type != MD_TEXT_NORMAL || !math_markers(md, text, size))
                write_text(md, text, size);
            break;
        }
    }
    return 0;
}

void tui_markdown_graphics(FILE *out, const char *text, size_t size, unsigned cols,
                           const tui_graphics *graphics) {
    if (!size)
        return;
    const MD_PARSER parser = {
        .flags = TUI_MD_FLAGS,
        .enter_block = enter_block, .leave_block = leave_block,
        .enter_span = enter_span, .leave_span = leave_span, .text = md_text,
    };
    markdown md = {.out = {.file = out, .cols = cols > 1 ? cols - 1 : 1}, .graphics = graphics};
    if (size <= UINT_MAX)
        md.math_source = tui_math_prepare(text, size);
    const char *source = md.math_source.text ? md.math_source.text : text;
    size_t length = md.math_source.text ? md.math_source.size : size;
    if (length > UINT_MAX || md_parse(source, (MD_SIZE)length, &parser, &md)) {
        md.out.capture = NULL;
        tui_markup_break(&md.out);
        tui_safe_text(out, text, size);
        fputc('\n', out);
    }
    tui_markup_break(&md.out);
    tui_table_destroy(md.table);
    tui_math_source_free(&md.math_source);
    fputs("\033[0m", out);
}

void tui_markdown(FILE *out, const char *text, size_t size, unsigned cols) {
    tui_markdown_graphics(out, text, size, cols, NULL);
}
