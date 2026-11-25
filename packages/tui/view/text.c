// Text buffers, UTF-8 cell layout, and shared view drawing helpers.
#include "internal.h"
#include <stdlib.h>
#include <string.h>

enum { TAB_WIDTH = 4 };

/* ---------- buffers ---------- */

bool view_buf_reserve(text_buf *b, size_t extra) {
    if (extra > SIZE_MAX / 2 - b->len)
        return false;
    if (b->len + extra + 1 <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra + 1)
        cap *= 2;
    char *data = realloc(b->data, cap);
    if (!data)
        return false;
    b->data = data;
    b->cap = cap;
    return true;
}

bool view_buf_insert(text_buf *b, size_t at, const char *s, size_t n) {
    if (!view_buf_reserve(b, n))
        return false;
    memmove(b->data + at + n, b->data + at, b->len - at);
    memcpy(b->data + at, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}

bool view_buf_put(text_buf *b, const char *s, size_t n) {
    return view_buf_insert(b, b->len, s, n);
}

bool view_buf_puts(text_buf *b, const char *s) {
    return view_buf_put(b, s, strlen(s));
}

bool view_buf_repeat(text_buf *b, const char *s, size_t n) {
    size_t slen = strlen(s);
    if (!view_buf_reserve(b, slen * n))
        return false;
    for (size_t i = 0; i < n; ++i) {
        memcpy(b->data + b->len, s, slen);
        b->len += slen;
    }
    b->data[b->len] = 0;
    return true;
}

void view_buf_erase(text_buf *b, size_t at, size_t n) {
    memmove(b->data + at, b->data + at + n, b->len - at - n);
    b->len -= n;
    b->data[b->len] = 0;
}

bool view_buf_set(text_buf *b, const char *s, size_t n) {
    b->len = 0;
    if (!view_buf_reserve(b, n))
        return false;
    memcpy(b->data, s, n);
    b->len = n;
    b->data[n] = 0;
    return true;
}

/* ---------- UTF-8 and display width ---------- */

size_t view_decode(const char *s, size_t n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    size_t need;
    uint32_t value, min;
    if (!n) {
        *cp = 0;
        return 0;
    }
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    } else if (u[0] >= 0xc2 && u[0] <= 0xdf) {
        need = 2, value = u[0] & 0x1fu, min = 0x80;
    } else if (u[0] >= 0xe0 && u[0] <= 0xef) {
        need = 3, value = u[0] & 0x0fu, min = 0x800;
    } else if (u[0] >= 0xf0 && u[0] <= 0xf4) {
        need = 4, value = u[0] & 0x07u, min = 0x10000;
    } else {
        goto bad;
    }
    if (n < need)
        goto bad;
    for (size_t i = 1; i < need; ++i) {
        if ((u[i] & 0xc0) != 0x80)
            goto bad;
        value = (value << 6) | (u[i] & 0x3fu);
    }
    if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        goto bad;
    *cp = value;
    return need;
bad:
    *cp = 0xfffd;
    return 1;
}

typedef struct {
    uint32_t first, last;
} cp_range;

static const cp_range zero_width[] = {
    {0x0300, 0x036f}, {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x05bf, 0x05bf}, {0x05c1, 0x05c2},
    {0x05c4, 0x05c5}, {0x05c7, 0x05c7}, {0x0610, 0x061a}, {0x064b, 0x065f}, {0x0670, 0x0670},
    {0x06d6, 0x06dc}, {0x06df, 0x06e4}, {0x06e7, 0x06e8}, {0x06ea, 0x06ed}, {0x0900, 0x0902},
    {0x093a, 0x093a}, {0x093c, 0x093c}, {0x0941, 0x0948}, {0x094d, 0x094d}, {0x0951, 0x0957},
    {0x0e31, 0x0e31}, {0x0e34, 0x0e3a}, {0x0e47, 0x0e4e}, {0x1160, 0x11ff}, {0x1ab0, 0x1aff},
    {0x1dc0, 0x1dff}, {0x200b, 0x200f}, {0x2028, 0x202e}, {0x2060, 0x2064}, {0x20d0, 0x20ff},
    {0x302a, 0x302d}, {0x3099, 0x309a}, {0xfe00, 0xfe0f}, {0xfe20, 0xfe2f}, {0xfeff, 0xfeff},
    {0x1f3fb, 0x1f3ff}, {0xe0000, 0xe0fff},
};

static const cp_range wide[] = {
    {0x1100, 0x115f}, {0x231a, 0x231b}, {0x2329, 0x232a}, {0x23e9, 0x23ec}, {0x23f0, 0x23f0},
    {0x23f3, 0x23f3}, {0x25fd, 0x25fe}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267f, 0x267f},
    {0x2693, 0x2693}, {0x26a1, 0x26a1}, {0x26aa, 0x26ab}, {0x26bd, 0x26be}, {0x26c4, 0x26c5},
    {0x26ce, 0x26ce}, {0x26d4, 0x26d4}, {0x26ea, 0x26ea}, {0x26f2, 0x26f3}, {0x26f5, 0x26f5},
    {0x26fa, 0x26fa}, {0x26fd, 0x26fd}, {0x2705, 0x2705}, {0x270a, 0x270b}, {0x2728, 0x2728},
    {0x274c, 0x274c}, {0x274e, 0x274e}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27b0, 0x27b0}, {0x27bf, 0x27bf}, {0x2b1b, 0x2b1c}, {0x2b50, 0x2b50}, {0x2b55, 0x2b55},
    {0x2e80, 0x303e}, {0x3041, 0x33ff}, {0x3400, 0x4dbf}, {0x4e00, 0x9fff}, {0xa000, 0xa4cf},
    {0xa960, 0xa97f}, {0xac00, 0xd7a3}, {0xf900, 0xfaff}, {0xfe10, 0xfe19}, {0xfe30, 0xfe6f},
    {0xff00, 0xff60}, {0xffe0, 0xffe6}, {0x16fe0, 0x18cff}, {0x1b000, 0x1b2ff}, {0x1f004, 0x1f004},
    {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a}, {0x1f200, 0x1f2ff},
    {0x1f300, 0x1f320}, {0x1f32d, 0x1f335}, {0x1f337, 0x1f37c}, {0x1f37e, 0x1f393},
    {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3}, {0x1f3e0, 0x1f3f0}, {0x1f3f4, 0x1f3f4},
    {0x1f3f8, 0x1f43e}, {0x1f440, 0x1f440}, {0x1f442, 0x1f4fc}, {0x1f4ff, 0x1f53d},
    {0x1f54b, 0x1f54e}, {0x1f550, 0x1f567}, {0x1f57a, 0x1f57a}, {0x1f595, 0x1f596},
    {0x1f5a4, 0x1f5a4}, {0x1f5fb, 0x1f64f}, {0x1f680, 0x1f6c5}, {0x1f6cc, 0x1f6cc},
    {0x1f6d0, 0x1f6d2}, {0x1f6d5, 0x1f6d7}, {0x1f6dc, 0x1f6df}, {0x1f6eb, 0x1f6ec},
    {0x1f6f4, 0x1f6fc}, {0x1f7e0, 0x1f7eb}, {0x1f7f0, 0x1f7f0}, {0x1f90c, 0x1f93a},
    {0x1f93c, 0x1f945}, {0x1f947, 0x1f9ff}, {0x1fa70, 0x1faff}, {0x20000, 0x2fffd},
    {0x30000, 0x3fffd},
};

static bool in_ranges(uint32_t cp, const cp_range *r, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < r[mid].first)
            hi = mid;
        else if (cp > r[mid].last)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

bool view_is_control(uint32_t cp) {
    return cp < 32 || (cp >= 0x7f && cp < 0xa0);
}

static unsigned cp_width(uint32_t cp) {
    if (cp == '\t')
        return TAB_WIDTH;
    if (view_is_control(cp))
        return 1;
    if (in_ranges(cp, zero_width, sizeof(zero_width) / sizeof(*zero_width)))
        return 0;
    return in_ranges(cp, wide, sizeof(wide) / sizeof(*wide)) ? 2 : 1;
}

size_t view_next_cluster(const char *s, size_t len, size_t pos, unsigned *width, uint32_t *base) {
    uint32_t cp;
    size_t n = view_decode(s + pos, len - pos, &cp);
    *base = cp;
    *width = cp_width(cp);
    pos += n;
    if (cp == '\n')
        return pos;
    bool join = cp == 0x200d;
    while (pos < len) {
        n = view_decode(s + pos, len - pos, &cp);
        if (cp == '\n' || view_is_control(cp))
            break;
        if (!join && cp_width(cp) != 0)
            break;
        join = cp == 0x200d;
        pos += n;
    }
    return pos;
}

size_t view_line_start(const char *s, size_t pos) {
    while (pos && s[pos - 1] != '\n')
        --pos;
    return pos;
}

size_t view_prev_cluster(const char *s, size_t len, size_t pos) {
    if (!pos)
        return 0;
    if (s[pos - 1] == '\n')
        return pos - 1;
    size_t at = view_line_start(s, pos), last = at;
    while (at < pos) {
        unsigned w;
        uint32_t cp;
        last = at;
        at = view_next_cluster(s, len, at, &w, &cp);
    }
    return last;
}

static bool emit_cp(text_buf *out, const char *s, size_t n, uint32_t cp) {
    if (cp == '\t')
        return view_buf_put(out, "    ", TAB_WIDTH);
    if (cp == 0xfffd && n == 1)
        return view_buf_put(out, "\xef\xbf\xbd", 3);
    if (view_is_control(cp))
        return view_buf_put(out, " ", 1);
    return view_buf_put(out, s, n);
}

unsigned view_span_width(const char *s, size_t n) {
    unsigned width = 0;
    for (size_t pos = 0; pos < n;) {
        uint32_t cp;
        size_t step = view_decode(s + pos, n - pos, &cp);
        width += cp == '\n' ? 1 : cp_width(cp);
        pos += step;
    }
    return width;
}

bool view_emit_line(text_buf *out, const char *s, size_t n, unsigned max, unsigned *used) {
    unsigned total = view_span_width(s, n), width = 0;
    bool cut = total > max;
    unsigned limit = cut ? (max > 1 ? max - 1 : 0) : max;
    for (size_t pos = 0; pos < n;) {
        unsigned w;
        uint32_t cp;
        size_t next = view_next_cluster(s, n, pos, &w, &cp);
        if (cp == '\n')
            w = 1;
        if (width + w > limit)
            break;
        for (size_t at = pos; at < next;) {
            uint32_t c;
            size_t step = view_decode(s + at, next - at, &c);
            if (!emit_cp(out, s + at, step, c == '\n' ? ' ' : c))
                return false;
            at += step;
        }
        width += w;
        pos = next;
    }
    if (cut && max >= 1) {
        if (!view_buf_puts(out, "\xe2\x80\xa6")) // …
            return false;
        width += 1;
    }
    if (used)
        *used = width;
    return true;
}

bool view_emit_padded(text_buf *out, const char *s, size_t n, unsigned max) {
    unsigned used = 0;
    if (!view_emit_line(out, s, n, max, &used))
        return false;
    return used < max ? view_buf_repeat(out, " ", max - used) : true;
}

/* ---------- layout ---------- */

static bool push_row(text_row **rows, size_t *count, size_t *cap, size_t start, size_t end,
                     bool soft) {
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2 : 16;
        text_row *grown = realloc(*rows, next * sizeof(**rows));
        if (!grown)
            return false;
        *rows = grown;
        *cap = next;
    }
    (*rows)[(*count)++] = (text_row){start, end, soft};
    return true;
}

bool view_wrap(const char *s, size_t len, unsigned width, text_row **rows, size_t *count,
               size_t *cap) {
    *count = 0;
    if (width < 2)
        width = 2;
    size_t ls = 0;
    for (;;) {
        size_t le = ls;
        while (le < len && s[le] != '\n')
            ++le;
        size_t start = ls, pos = ls, brk = 0;
        unsigned col = 0;
        while (pos < le) {
            unsigned w;
            uint32_t cp;
            size_t next = view_next_cluster(s, le, pos, &w, &cp);
            if (col + w > width && pos > start) {
                size_t at = brk > start ? brk : pos;
                if (!push_row(rows, count, cap, start, at, true))
                    return false;
                start = at;
                brk = 0;
                col = view_span_width(s + start, pos - start);
                continue;
            }
            col += w;
            pos = next;
            if (cp == ' ')
                brk = pos;
        }
        if (!push_row(rows, count, cap, start, le, false))
            return false;
        if (le >= len)
            return true;
        ls = le + 1;
    }
}

unsigned view_column_of(const char *s, size_t from, size_t to) {
    unsigned col = 0;
    while (from < to) {
        unsigned w;
        uint32_t cp;
        from = view_next_cluster(s, to, from, &w, &cp);
        col += w;
    }
    return col;
}

/* ---------- rendering helpers ---------- */

unsigned view_frame_width(unsigned cols) {
    return cols > 1 ? cols - 1 : 1;
}

bool view_newline(text_buf *out, unsigned *lines) {
    if (*lines && !view_buf_put(out, "\n", 1))
        return false;
    ++*lines;
    return true;
}

const char *view_status_color(const char *st, bool attention, bool working) {
    if (attention)
        return S_AMBER;
    if (working)
        return S_ACCENT;
    if (!st)
        return S_DIM;
    if (!strcmp(st, "ready") || !strcmp(st, "completed"))
        return S_GREEN;
    if (!strcmp(st, "failed") || !strcmp(st, "error"))
        return S_RED;
    if (!strcmp(st, "running"))
        return S_ACCENT;
    return S_DIM;
}

bool view_render_detail_line(text_buf *out, const char *s, size_t n, unsigned max) {
    // Highlight "Key: Value" lines with muted key and crisp value
    const char *colon = memchr(s, ':', n);
    if (colon && colon > s && (size_t)(colon - s) <= 18 &&
        (size_t)(colon - s + 1) < n && colon[1] == ' ') {
        size_t klen = (size_t)(colon - s);
        const char *val = colon + 2;
        size_t vlen = n - (size_t)(val - s);
        unsigned used = 0;
        if (!view_buf_puts(out, S_DIM) || !view_emit_line(out, s, klen, max, &used))
            return false;
        if (used + 2 < max) {
            if (!view_buf_puts(out, S_FAINT ": " S_TEXT))
                return false;
            used += 2;
            unsigned vused = 0;
            if (!view_emit_line(out, val, vlen, max - used, &vused))
                return false;
            used += vused;
        }
        return view_buf_puts(out, S_RST) && (used < max ? view_buf_repeat(out, " ", max - used) : true);
    }
    if (!view_buf_puts(out, S_TEXT))
        return false;
    return view_emit_padded(out, s, n, max) && view_buf_puts(out, S_RST);
}
