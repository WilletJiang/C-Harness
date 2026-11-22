#include "tui_markup.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    size_t size, start, capacity;
    unsigned quote;
    char closer;
    tui_math_source result;
} math_scan;

static int boundary(MD_BLOCKTYPE type, void *detail, void *context) {
    (void)type;
    (void)detail;
    ((math_scan *)context)->closer = 0;
    return 0;
}

static int enter(MD_BLOCKTYPE type, void *detail, void *context) {
    if (type == MD_BLOCK_QUOTE)
        ++((math_scan *)context)->quote;
    return boundary(type, detail, context);
}

static int leave(MD_BLOCKTYPE type, void *detail, void *context) {
    if (type == MD_BLOCK_QUOTE)
        --((math_scan *)context)->quote;
    return boundary(type, detail, context);
}

static int span(MD_SPANTYPE type, void *detail, void *context) {
    (void)detail;
    if (type == MD_SPAN_CODE || type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY)
        ((math_scan *)context)->closer = 0;
    return 0;
}

static bool escaped_bracket(const char *text, size_t at) {
    size_t start = at;
    while (start && text[start - 1] == '\\')
        --start;
    return (at - start) % 2 != 0;
}

static bool record(math_scan *scan, size_t end) {
    size_t size = end - scan->start + 1;
    const char *source = scan->text + scan->start;
    if (size <= 4)
        return true;
    if (scan->result.count == scan->capacity) {
        size_t capacity = scan->capacity ? scan->capacity * 2 : 16;
        tui_math_region *regions = realloc(scan->result.regions, capacity * sizeof(*regions));
        if (!regions)
            return false;
        scan->result.regions = regions;
        scan->capacity = capacity;
    }
    scan->result.regions[scan->result.count++] = (tui_math_region){
        .offset = scan->start, .source = source, .size = size, .quote = scan->quote,
        .display = scan->closer == ']',
    };
    return true;
}

static int collect(MD_TEXTTYPE type, const char *text, MD_SIZE size, void *context) {
    if (type != MD_TEXT_NORMAL)
        return 0;
    math_scan *scan = context;
    uintptr_t base = (uintptr_t)scan->text, ptr = (uintptr_t)text;
    if (ptr < base || ptr - base > scan->size || size > scan->size - (size_t)(ptr - base))
        return 0;
    for (size_t i = 0; i < size; ++i) {
        char mark = text[i];
        if (mark != '(' && mark != ')' && mark != '[' && mark != ']')
            continue;
        size_t at = (size_t)(ptr - base) + i;
        if (!escaped_bracket(scan->text, at))
            continue;
        if (mark == scan->closer && at > scan->start + 2) {
            if (!record(scan, at))
                return 1;
            scan->closer = 0;
        } else if (!scan->closer && (mark == '(' || mark == '[')) {
            scan->start = at - 1;
            scan->closer = mark == '(' ? ')' : ']';
        }
    }
    return 0;
}

tui_math_source tui_math_prepare(const char *text, size_t size) {
    bool needed = false;
    for (size_t i = 0; i + 1 < size && !needed; ++i)
        needed = text[i] == '\\' && (text[i + 1] == '(' || text[i + 1] == '[');
    if (!needed)
        return (tui_math_source){0};
    const MD_PARSER parser = {
        .flags = TUI_MD_FLAGS, .enter_block = enter, .leave_block = leave,
        .enter_span = span, .leave_span = span, .text = collect,
    };
    math_scan scan = {.text = text, .size = size};
    if (md_parse(text, (MD_SIZE)size, &parser, &scan) || !scan.result.count) {
        free(scan.result.regions);
        return (tui_math_source){0};
    }
    char *rewritten = malloc(size);
    if (!rewritten) {
        free(scan.result.regions);
        return (tui_math_source){0};
    }
    /* Keep the source for fallback. The offset identifies generated markers, so
       a control byte supplied in the original text never becomes a math span. */
    size_t start = 0, used = 0;
    for (size_t i = 0; i < scan.result.count; ++i) {
        tui_math_region *region = &scan.result.regions[i];
        size_t plain = region->offset - start;
        memcpy(rewritten + used, text + start, plain);
        used += plain;
        start = region->offset + region->size;
        region->offset = used;
        rewritten[used++] = TUI_MATH_MARK;
    }
    memcpy(rewritten + used, text + start, size - start);
    scan.result.size = used + size - start;
    scan.result.text = rewritten;
    return scan.result;
}

void tui_math_source_free(tui_math_source *source) {
    free(source->text);
    free(source->regions);
}

const char *tui_math_region_text(const tui_math_region *region, char buffer[TUI_MATH_BYTES],
                                 size_t *size) {
    *size = region->size;
    if (!region->quote || region->size > TUI_MATH_BYTES ||
        !memchr(region->source, '\n', region->size))
        return region->source;
    size_t read = 0, used = 0;
    while (read < region->size) {
        char c = region->source[read++];
        buffer[used++] = c;
        if (c != '\n')
            continue;
        for (unsigned depth = 0; depth < region->quote; ++depth) {
            size_t marker = read;
            while (marker < region->size && region->source[marker] == ' ' && marker - read < 3)
                ++marker;
            if (marker == region->size || region->source[marker] != '>')
                break;
            read = marker + 1;
            if (read < region->size && region->source[read] == ' ')
                ++read;
        }
    }
    *size = used;
    return buffer;
}
