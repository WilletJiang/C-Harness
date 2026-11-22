// Pure C17 view: thin-border composer, floating slash palette, Braille busy indicator,
// and interactive two-pane Command Center. No terminal syscalls or JSON dependencies.
#include "tui_view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SPIN_MS = 90,
    PALETTE_ROWS = 6,
    PREVIEW_ROWS = 6,
    HISTORY_MAX = 200,
    TAB_WIDTH = 4,
    FILTER_COUNT = 5,
};

// Restrained dark-terminal 256-color palette
#define S_RST       "\033[0m"
#define S_BOLD      "\033[1m"
#define S_TEXT      "\033[38;5;252m"
#define S_DIM       "\033[38;5;244m"
#define S_FAINT     "\033[38;5;240m"
#define S_BORDER    "\033[38;5;239m"
#define S_BORDER_HI "\033[38;5;75m"
#define S_ACCENT    "\033[38;5;75m"
#define S_GOLD      "\033[38;5;180m"
#define S_LAVENDER  "\033[38;5;146m"
#define S_GREEN     "\033[38;5;114m"
#define S_AMBER     "\033[38;5;215m"
#define S_RED       "\033[38;5;203m"
#define S_KEY       "\033[38;5;250m"
#define S_SEL_BG    "\033[48;5;24m\033[38;5;231m"
#define S_SEL_BOLD  "\033[48;5;24m\033[1;38;5;231m"
#define S_SEL_DESC  "\033[48;5;24m\033[38;5;153m"
#define S_PILL_ON   "\033[1;48;5;240m\033[38;5;231m"

static const char *const spinner[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f",
};
#define SPIN_FRAMES (sizeof(spinner) / sizeof(*spinner))

typedef struct {
    char *data;
    size_t len, cap;
} text_buf;

typedef struct {
    size_t start, end;
    bool soft;
} text_row;

struct tui_view {
    text_buf draft, out, action, stash;
    size_t cursor, scroll;
    unsigned cols;
    int goal;
    bool dismissed, was_busy, stashed, was_picker;
    uint64_t busy_since;
    size_t selected;
    size_t *matches;
    size_t match_count, match_cap;
    text_row *rows;
    size_t row_count, row_cap;
    char **history;
    size_t history_count, history_pos;
    // Command Center / picker state
    unsigned pick_filter;
    size_t pick_selected, pick_scroll;
    size_t *pick_idx;
    size_t pick_count, pick_cap;
    // Login & API Configuration form state
    bool was_auth, auth_reveal;
    unsigned auth_focus;  // 0=preset, 1=endpoint, 2=token, 3=model, 4=save
    unsigned auth_preset; // 0=OpenAI, 1=OpenRouter, 2=Custom
    text_buf auth_endpoint, auth_token, auth_model;
    size_t auth_cursor[3]; // cursor for endpoint(0), token(1), model(2)
};

/* ---------- buffers ---------- */

static bool buf_reserve(text_buf *b, size_t extra) {
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

static bool buf_insert(text_buf *b, size_t at, const char *s, size_t n) {
    if (!buf_reserve(b, n))
        return false;
    memmove(b->data + at + n, b->data + at, b->len - at);
    memcpy(b->data + at, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}

static bool buf_put(text_buf *b, const char *s, size_t n) {
    return buf_insert(b, b->len, s, n);
}

static bool buf_puts(text_buf *b, const char *s) {
    return buf_put(b, s, strlen(s));
}

static bool buf_repeat(text_buf *b, const char *s, size_t n) {
    size_t slen = strlen(s);
    if (!buf_reserve(b, slen * n))
        return false;
    for (size_t i = 0; i < n; ++i) {
        memcpy(b->data + b->len, s, slen);
        b->len += slen;
    }
    b->data[b->len] = 0;
    return true;
}

static void buf_erase(text_buf *b, size_t at, size_t n) {
    memmove(b->data + at, b->data + at + n, b->len - at - n);
    b->len -= n;
    b->data[b->len] = 0;
}

static bool buf_set(text_buf *b, const char *s, size_t n) {
    b->len = 0;
    if (!buf_reserve(b, n))
        return false;
    memcpy(b->data, s, n);
    b->len = n;
    b->data[n] = 0;
    return true;
}

/* ---------- UTF-8 and display width ---------- */

static size_t decode(const char *s, size_t n, uint32_t *cp) {
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

static bool is_control(uint32_t cp) {
    return cp < 32 || (cp >= 0x7f && cp < 0xa0);
}

static unsigned cp_width(uint32_t cp) {
    if (cp == '\t')
        return TAB_WIDTH;
    if (is_control(cp))
        return 1;
    if (in_ranges(cp, zero_width, sizeof(zero_width) / sizeof(*zero_width)))
        return 0;
    return in_ranges(cp, wide, sizeof(wide) / sizeof(*wide)) ? 2 : 1;
}

static size_t next_cluster(const char *s, size_t len, size_t pos, unsigned *width, uint32_t *base) {
    uint32_t cp;
    size_t n = decode(s + pos, len - pos, &cp);
    *base = cp;
    *width = cp_width(cp);
    pos += n;
    if (cp == '\n')
        return pos;
    bool join = cp == 0x200d;
    while (pos < len) {
        n = decode(s + pos, len - pos, &cp);
        if (cp == '\n' || is_control(cp))
            break;
        if (!join && cp_width(cp) != 0)
            break;
        join = cp == 0x200d;
        pos += n;
    }
    return pos;
}

static size_t line_start(const char *s, size_t pos) {
    while (pos && s[pos - 1] != '\n')
        --pos;
    return pos;
}

static size_t prev_cluster(const char *s, size_t len, size_t pos) {
    if (!pos)
        return 0;
    if (s[pos - 1] == '\n')
        return pos - 1;
    size_t at = line_start(s, pos), last = at;
    while (at < pos) {
        unsigned w;
        uint32_t cp;
        last = at;
        at = next_cluster(s, len, at, &w, &cp);
    }
    return last;
}

static bool emit_cp(text_buf *out, const char *s, size_t n, uint32_t cp) {
    if (cp == '\t')
        return buf_put(out, "    ", TAB_WIDTH);
    if (cp == 0xfffd && n == 1)
        return buf_put(out, "\xef\xbf\xbd", 3);
    if (is_control(cp))
        return buf_put(out, " ", 1);
    return buf_put(out, s, n);
}

static unsigned span_width(const char *s, size_t n) {
    unsigned width = 0;
    for (size_t pos = 0; pos < n;) {
        uint32_t cp;
        size_t step = decode(s + pos, n - pos, &cp);
        width += cp == '\n' ? 1 : cp_width(cp);
        pos += step;
    }
    return width;
}

static bool emit_line(text_buf *out, const char *s, size_t n, unsigned max, unsigned *used) {
    unsigned total = span_width(s, n), width = 0;
    bool cut = total > max;
    unsigned limit = cut ? (max > 1 ? max - 1 : 0) : max;
    for (size_t pos = 0; pos < n;) {
        unsigned w;
        uint32_t cp;
        size_t next = next_cluster(s, n, pos, &w, &cp);
        if (cp == '\n')
            w = 1;
        if (width + w > limit)
            break;
        for (size_t at = pos; at < next;) {
            uint32_t c;
            size_t step = decode(s + at, next - at, &c);
            if (!emit_cp(out, s + at, step, c == '\n' ? ' ' : c))
                return false;
            at += step;
        }
        width += w;
        pos = next;
    }
    if (cut && max >= 1) {
        if (!buf_puts(out, "\xe2\x80\xa6")) // …
            return false;
        width += 1;
    }
    if (used)
        *used = width;
    return true;
}

static bool emit_padded(text_buf *out, const char *s, size_t n, unsigned max) {
    unsigned used = 0;
    if (!emit_line(out, s, n, max, &used))
        return false;
    return used < max ? buf_repeat(out, " ", max - used) : true;
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

static bool wrap(const char *s, size_t len, unsigned width, text_row **rows, size_t *count,
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
            size_t next = next_cluster(s, le, pos, &w, &cp);
            if (col + w > width && pos > start) {
                size_t at = brk > start ? brk : pos;
                if (!push_row(rows, count, cap, start, at, true))
                    return false;
                start = at;
                brk = 0;
                col = span_width(s + start, pos - start);
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

static unsigned column_of(const char *s, size_t from, size_t to) {
    unsigned col = 0;
    while (from < to) {
        unsigned w;
        uint32_t cp;
        from = next_cluster(s, to, from, &w, &cp);
        col += w;
    }
    return col;
}

static size_t row_of(const tui_view *v, size_t pos) {
    size_t row = 0;
    for (size_t i = 0; i < v->row_count && v->rows[i].start <= pos; ++i)
        row = i;
    return row;
}

static size_t offset_at(const tui_view *v, size_t row, unsigned goal) {
    const text_row *r = &v->rows[row];
    size_t pos = r->start;
    unsigned col = 0;
    while (pos < r->end) {
        unsigned w;
        uint32_t cp;
        size_t next = next_cluster(v->draft.data, r->end, pos, &w, &cp);
        if (col + w > goal || (r->soft && next >= r->end))
            break;
        col += w;
        pos = next;
    }
    return pos;
}

static unsigned frame_width(unsigned cols) {
    return cols > 1 ? cols - 1 : 1;
}

static bool boxed_mode(unsigned cols, unsigned rows) {
    return frame_width(cols) >= 16 && rows >= 5;
}

static unsigned editor_width(unsigned cols, bool boxed) {
    unsigned w = frame_width(cols);
    if (boxed)
        return w > 6 ? w - 6 : 2;
    return w > 4 ? w - 2 : 2;
}

static bool layout_draft(tui_view *v, unsigned cols, bool boxed) {
    if (!buf_reserve(&v->draft, 0))
        return false;
    v->draft.data[v->draft.len] = 0;
    return wrap(v->draft.data, v->draft.len, editor_width(cols, boxed), &v->rows, &v->row_count,
                &v->row_cap);
}

/* ---------- palette ---------- */

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool has_space(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t')
            return true;
    return false;
}

static bool palette_open(const tui_view *v) {
    return !v->dismissed && v->draft.len && v->draft.data[0] == '/' &&
           !(v->draft.len > 1 && v->draft.data[1] == '/') && !has_space(v->draft.data, v->draft.len);
}

static int rank(const char *name, const char *q, size_t qn) {
    size_t i = 0;
    while (i < qn && name[i] && lower(name[i]) == lower(q[i]))
        ++i;
    if (i == qn)
        return name[i] ? 2 : 3;
    const char *p = name;
    for (i = 0; i < qn; ++i) {
        while (*p && lower(*p) != lower(q[i]))
            ++p;
        if (!*p)
            return 0;
        ++p;
    }
    return 1;
}

static void refresh_matches(tui_view *v, const tui_view_status *status) {
    v->match_count = 0;
    if (!palette_open(v) || !status || !status->commands)
        return;
    size_t n = status->command_count;
    if (n > v->match_cap) {
        size_t *grown = realloc(v->matches, n * sizeof(*grown));
        if (!grown)
            return;
        v->matches = grown;
        v->match_cap = n;
    }
    const char *q = v->draft.data + 1;
    size_t qn = v->draft.len - 1;
    for (int level = 3; level >= 1; --level)
        for (size_t i = 0; i < n; ++i)
            if (status->commands[i].name && rank(status->commands[i].name, q, qn) == level)
                v->matches[v->match_count++] = i;
    if (v->selected >= v->match_count)
        v->selected = v->match_count ? v->match_count - 1 : 0;
}

/* ---------- picker / command center ---------- */

static bool picker_active(const tui_view_status *status) {
    return status && status->picker && status->picker->open;
}

static bool item_matches_filter(const tui_picker_item *it, unsigned filter) {
    switch (filter) {
    case 0: return true; // All
    case 1: return it->attention;
    case 2: return it->working;
    case 3: return !it->attention && !it->working && it->status && !strcmp(it->status, "ready");
    default: return !it->attention && !it->working && (!it->status || strcmp(it->status, "ready"));
    }
}

static void refresh_picker(tui_view *v, const tui_picker *p) {
    v->pick_count = 0;
    if (!p || !p->open || !p->items || !p->count) {
        v->pick_selected = v->pick_scroll = 0;
        return;
    }
    if (p->count > v->pick_cap) {
        size_t *grown = realloc(v->pick_idx, p->count * sizeof(*grown));
        if (!grown)
            return;
        v->pick_idx = grown;
        v->pick_cap = p->count;
    }
    for (size_t i = 0; i < p->count; ++i)
        if (item_matches_filter(&p->items[i], v->pick_filter))
            v->pick_idx[v->pick_count++] = i;
    if (v->pick_selected >= v->pick_count)
        v->pick_selected = v->pick_count ? v->pick_count - 1 : 0;
}

static const char *status_color(const char *st, bool attention, bool working) {
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

static const char *status_dot(const char *st, bool attention, bool working) {
    if (attention)
        return "\xe2\x96\xb2"; // ▲
    if (working || (st && !strcmp(st, "running")))
        return "\xe2\x97\x90"; // ◐
    if (st && !strcmp(st, "ready"))
        return "\xe2\x97\x8f"; // ●
    if (st && !strcmp(st, "failed"))
        return "\xe2\x9c\x95"; // ✕
    return "\xe2\x97\x8b";     // ○
}

/* ---------- editing ---------- */

static void edited(tui_view *v) {
    v->goal = -1;
    v->dismissed = false;
    v->selected = 0;
    v->history_pos = v->history_count;
    v->stashed = false;
}

static bool insert_text(tui_view *v, const char *s, size_t n) {
    text_buf clean = {0};
    bool ok = buf_reserve(&clean, n);
    for (size_t i = 0; ok && i < n;) {
        uint32_t cp;
        size_t step = decode(s + i, n - i, &cp);
        if (cp == '\r') {
            ok = buf_put(&clean, "\n", 1);
            if (i + 1 < n && s[i + 1] == '\n')
                ++step;
        } else if (cp == '\n' || cp == '\t') {
            ok = buf_put(&clean, s + i, 1);
        } else if (cp == 0xfffd && step == 1) {
            ok = buf_put(&clean, "\xef\xbf\xbd", 3);
        } else if (!is_control(cp)) {
            ok = buf_put(&clean, s + i, step);
        }
        i += step;
    }
    if (ok && clean.len) {
        ok = buf_insert(&v->draft, v->cursor, clean.data, clean.len);
        if (ok)
            v->cursor += clean.len;
    }
    free(clean.data);
    edited(v);
    return ok;
}

static void set_draft(tui_view *v, const char *s, size_t n) {
    if (buf_set(&v->draft, s, n))
        v->cursor = n;
    v->goal = -1;
    v->dismissed = false;
    v->selected = 0;
}

static void history_move(tui_view *v, bool older) {
    if (older) {
        if (!v->history_pos)
            return;
        if (v->history_pos == v->history_count && !v->stashed) {
            if (!buf_set(&v->stash, v->draft.data ? v->draft.data : "", v->draft.len))
                return;
            v->stashed = true;
        }
        --v->history_pos;
    } else {
        if (v->history_pos >= v->history_count)
            return;
        ++v->history_pos;
    }
    if (v->history_pos == v->history_count) {
        set_draft(v, v->stash.data ? v->stash.data : "", v->stash.len);
        v->stashed = false;
    } else {
        const char *h = v->history[v->history_pos];
        set_draft(v, h, strlen(h));
    }
}

static void vertical(tui_view *v, unsigned cols, bool up) {
    if (!layout_draft(v, cols, boxed_mode(cols, 24)))
        return;
    size_t row = row_of(v, v->cursor);
    if (up ? row == 0 : row + 1 >= v->row_count) {
        history_move(v, up);
        return;
    }
    if (v->goal < 0)
        v->goal = (int)column_of(v->draft.data, v->rows[row].start, v->cursor);
    v->cursor = offset_at(v, up ? row - 1 : row + 1, (unsigned)v->goal);
}

static tui_action make(tui_view *v, tui_action_kind kind, const char *s, size_t n) {
    if (!s)
        return (tui_action){kind, NULL};
    if (!buf_set(&v->action, s, n))
        return (tui_action){TUI_NO_ACTION, NULL};
    return (tui_action){kind, v->action.data};
}

static bool is_blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (s[i] != ' ' && s[i] != '\n' && s[i] != '\t')
            return false;
    return true;
}

static tui_action enter(tui_view *v, const tui_view_status *status) {
    const char *s = v->draft.data;
    size_t n = v->draft.len;
    if (!n || is_blank(s, n))
        return (tui_action){TUI_NO_ACTION, NULL};
    if (v->cursor == n && s[n - 1] == '\\') {
        v->draft.data[n - 1] = '\n';
        edited(v);
        return (tui_action){TUI_NO_ACTION, NULL};
    }
    if (palette_open(v) && v->match_count && status && status->commands) {
        const char *name = status->commands[v->matches[v->selected]].name;
        return make(v, TUI_COMMAND, name, strlen(name));
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t'))
        --n;
    if (s[0] == '/' && n > 1 && s[1] == '/')
        return make(v, TUI_SUBMIT, s + 1, n - 1);
    if (s[0] == '/' && n > 1) {
        size_t token = 1;
        while (token < n && s[token] != ' ' && s[token] != '\n' && s[token] != '\t')
            ++token;
        bool known = token == n;
        for (size_t i = 0; !known && status && status->commands && i < status->command_count; ++i) {
            const char *name = status->commands[i].name;
            known = name && strlen(name) == token - 1 && !memcmp(name, s + 1, token - 1);
        }
        if (known)
            return make(v, TUI_COMMAND, s + 1, n - 1);
    }
    return make(v, TUI_SUBMIT, s, n);
}

static tui_action picker_key(tui_view *v, const tui_picker *p, tui_key key, const char *text,
                             size_t count) {
    tui_action none = {TUI_NO_ACTION, NULL};
    refresh_picker(v, p);
    const tui_picker_item *cur =
        v->pick_count ? &p->items[v->pick_idx[v->pick_selected]] : NULL;
    switch (key) {
    case TUI_ESCAPE:
    case TUI_INTERRUPT:
        return (tui_action){TUI_PICK_CLOSE, NULL};
    case TUI_UP:
        if (v->pick_count)
            v->pick_selected = (v->pick_selected + v->pick_count - 1) % v->pick_count;
        break;
    case TUI_DOWN:
        if (v->pick_count)
            v->pick_selected = (v->pick_selected + 1) % v->pick_count;
        break;
    case TUI_HOME:
        v->pick_selected = 0;
        break;
    case TUI_END:
        if (v->pick_count)
            v->pick_selected = v->pick_count - 1;
        break;
    case TUI_TAB:
        v->pick_filter = (v->pick_filter + 1) % FILTER_COUNT;
        v->pick_selected = v->pick_scroll = 0;
        break;
    case TUI_SHIFT_TAB:
        v->pick_filter = (v->pick_filter + FILTER_COUNT - 1) % FILTER_COUNT;
        v->pick_selected = v->pick_scroll = 0;
        break;
    case TUI_ENTER:
        if (p->can_open && cur && cur->id)
            return make(v, TUI_PICK_OPEN, cur->id, strlen(cur->id));
        break;
    case TUI_TEXT:
        if (text && count == 1) {
            char c = lower(text[0]);
            if (c == 'q')
                return (tui_action){TUI_PICK_CLOSE, NULL};
            if (c == 'j' && v->pick_count)
                v->pick_selected = (v->pick_selected + 1) % v->pick_count;
            else if (c == 'k' && v->pick_count)
                v->pick_selected = (v->pick_selected + v->pick_count - 1) % v->pick_count;
            else if (c == 'a' && p->can_approve && cur && cur->id)
                return make(v, TUI_PICK_APPROVE, cur->id, strlen(cur->id));
            else if (c == 'd' && p->can_approve && cur && cur->id)
                return make(v, TUI_PICK_DENY, cur->id, strlen(cur->id));
            else if (c == 'n' && p->can_new)
                return (tui_action){TUI_PICK_NEW, NULL};
        }
        break;
    default:
        break;
    }
    return none;
}

/* ---------- Login & API Configuration ---------- */

static const char *const preset_names[] = {"ChatGPT", "OpenRouter", "Custom"};
static const char *const preset_urls[] = {
    "https://api.openai.com/v1/responses",
    "https://openrouter.ai/api/v1/responses",
    "",
};
#define PRESET_COUNT 3

static bool auth_active(const tui_view_status *status) {
    return status && status->auth && status->auth->open;
}

static void wipe_buf(text_buf *b) {
    if (b->data && b->cap) {
        volatile unsigned char *p = (volatile unsigned char *)b->data;
        for (size_t i = 0; i < b->cap; ++i)
            p[i] = 0;
    }
    b->len = 0;
}

static unsigned detect_preset(const char *url) {
    return url && !strcmp(url, preset_urls[1]) ? 1 : 2;
}

static void auth_init(tui_view *v, const tui_auth_state *a) {
    const char *ep = a->endpoint && *a->endpoint ? a->endpoint : preset_urls[0];
    const char *md = a->model && *a->model ? a->model : "gpt-5";
    (void)buf_set(&v->auth_endpoint, ep, strlen(ep));
    wipe_buf(&v->auth_token);
    (void)buf_set(&v->auth_token, "", 0);
    (void)buf_set(&v->auth_model, md, strlen(md));
    v->auth_cursor[0] = v->auth_endpoint.len;
    v->auth_cursor[1] = 0;
    v->auth_cursor[2] = v->auth_model.len;
    v->auth_preset = a->chatgpt ? 0 : detect_preset(ep);
    v->auth_reveal = false;
    v->auth_focus = v->auth_preset == 0 ? 4 : a->has_token ? 1 : 2;
    v->was_auth = true;
}

static text_buf *auth_field_buf(tui_view *v, unsigned focus, size_t **cur) {
    if (focus == 1) {
        *cur = &v->auth_cursor[0];
        return &v->auth_endpoint;
    }
    if (focus == 2) {
        *cur = &v->auth_cursor[1];
        return &v->auth_token;
    }
    if (focus == 3) {
        *cur = &v->auth_cursor[2];
        return &v->auth_model;
    }
    return NULL;
}

static void auth_insert(text_buf *b, size_t *cur, const char *s, size_t n) {
    text_buf clean = {0};
    bool ok = buf_reserve(&clean, n);
    for (size_t i = 0; ok && i < n;) {
        uint32_t cp;
        size_t step = decode(s + i, n - i, &cp);
        // Strip newlines/tabs/control chars from single-line API fields
        if (cp >= 32 && !(cp >= 0x7f && cp < 0xa0))
            ok = buf_put(&clean, s + i, step);
        i += step;
    }
    if (ok && clean.len && buf_insert(b, *cur, clean.data, clean.len))
        *cur += clean.len;
    wipe_buf(&clean);
    free(clean.data);
}

static void move_auth_focus(tui_view *v, unsigned step) {
    do {
        v->auth_focus = (v->auth_focus + step) % 5;
    } while (v->auth_preset == 0 && (v->auth_focus == 1 || v->auth_focus == 2));
}

static tui_action auth_key(tui_view *v, const tui_auth_state *a, tui_key key, const char *text,
                           size_t count) {
    tui_action none = {TUI_NO_ACTION, NULL};
    if (!v->was_auth)
        auth_init(v, a);
    if (a->waiting)
        return key == TUI_ESCAPE || key == TUI_INTERRUPT
                   ? (tui_action){TUI_AUTH_CANCEL, NULL} : none;

    size_t *cur = NULL;
    text_buf *fb = auth_field_buf(v, v->auth_focus, &cur);

    switch (key) {
    case TUI_ESCAPE:
        return (tui_action){TUI_AUTH_CANCEL, NULL};
    case TUI_INTERRUPT:
        if (fb && fb->len) {
            wipe_buf(fb);
            *cur = 0;
            if (v->auth_focus == 1)
                v->auth_preset = 2;
            break;
        }
        return (tui_action){TUI_AUTH_CANCEL, NULL};
    case TUI_SAVE:
        return (tui_action){TUI_AUTH_SAVE, NULL};
    case TUI_REVEAL:
        if (v->auth_reveal)
            wipe_buf(&v->out);
        v->auth_reveal = !v->auth_reveal;
        break;
    case TUI_UP:
    case TUI_SHIFT_TAB:
        move_auth_focus(v, 4);
        break;
    case TUI_DOWN:
    case TUI_TAB:
        move_auth_focus(v, 1);
        break;
    case TUI_ENTER:
        if (v->auth_focus == 4)
            return (tui_action){TUI_AUTH_SAVE, NULL};
        if (v->auth_focus == 3)
            return (tui_action){TUI_AUTH_SAVE, NULL};
        move_auth_focus(v, 1);
        break;
    case TUI_LEFT:
        if (v->auth_focus == 0) {
            v->auth_preset = (v->auth_preset + PRESET_COUNT - 1) % PRESET_COUNT;
            if (v->auth_preset < 2) {
                const char *u = preset_urls[v->auth_preset];
                (void)buf_set(&v->auth_endpoint, u, strlen(u));
                v->auth_cursor[0] = v->auth_endpoint.len;
            }
        } else if (fb && *cur) {
            *cur = prev_cluster(fb->data, fb->len, *cur);
        }
        break;
    case TUI_RIGHT:
        if (v->auth_focus == 0) {
            v->auth_preset = (v->auth_preset + 1) % PRESET_COUNT;
            if (v->auth_preset < 2) {
                const char *u = preset_urls[v->auth_preset];
                (void)buf_set(&v->auth_endpoint, u, strlen(u));
                v->auth_cursor[0] = v->auth_endpoint.len;
            }
        } else if (fb && *cur < fb->len) {
            unsigned w;
            uint32_t cp;
            *cur = next_cluster(fb->data, fb->len, *cur, &w, &cp);
        }
        break;
    case TUI_HOME:
        if (fb)
            *cur = 0;
        break;
    case TUI_END:
        if (fb)
            *cur = fb->len;
        break;
    case TUI_BACKSPACE:
        if (fb && *cur) {
            size_t at = prev_cluster(fb->data, fb->len, *cur);
            buf_erase(fb, at, *cur - at);
            *cur = at;
            if (v->auth_focus == 1)
                v->auth_preset = detect_preset(fb->data);
        }
        break;
    case TUI_DELETE:
        if (fb && *cur < fb->len) {
            unsigned w;
            uint32_t cp;
            size_t end = next_cluster(fb->data, fb->len, *cur, &w, &cp);
            buf_erase(fb, *cur, end - *cur);
            if (v->auth_focus == 1)
                v->auth_preset = detect_preset(fb->data);
        }
        break;
    case TUI_TEXT:
    case TUI_PASTE:
        if (v->auth_focus == 0 && text && count == 1 && text[0] == ' ') {
            v->auth_preset = (v->auth_preset + 1) % PRESET_COUNT;
            if (v->auth_preset < 2) {
                const char *u = preset_urls[v->auth_preset];
                (void)buf_set(&v->auth_endpoint, u, strlen(u));
                v->auth_cursor[0] = v->auth_endpoint.len;
            }
        } else if (fb && text && count) {
            auth_insert(fb, cur, text, count);
            if (v->auth_focus == 1)
                v->auth_preset = detect_preset(fb->data);
        }
        break;
    default:
        break;
    }
    return none;
}

/* ---------- public API ---------- */

tui_view *tui_view_create(void) {
    tui_view *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->goal = -1;
    v->cols = 80;
    if (!buf_reserve(&v->draft, 0) || !buf_reserve(&v->out, 0)) {
        tui_view_destroy(v);
        return NULL;
    }
    return v;
}

void tui_view_wipe_auth(tui_view *v) {
    if (!v)
        return;
    wipe_buf(&v->auth_token);
    if (v->was_auth)
        wipe_buf(&v->out);
    v->auth_cursor[1] = 0;
    v->auth_reveal = false;
    v->was_auth = false;
}

bool tui_view_auth_values(const tui_view *v, const char **endpoint, const char **token,
                          const char **model, bool *chatgpt) {
    if (!v)
        return false;
    if (endpoint)
        *endpoint = v->auth_endpoint.data ? v->auth_endpoint.data : "";
    if (token)
        *token = v->auth_token.data ? v->auth_token.data : "";
    if (model)
        *model = v->auth_model.data ? v->auth_model.data : "";
    if (chatgpt)
        *chatgpt = v->auth_preset == 0;
    return true;
}

void tui_view_destroy(tui_view *v) {
    if (!v)
        return;
    wipe_buf(&v->auth_token);
    free(v->auth_endpoint.data);
    free(v->auth_token.data);
    free(v->auth_model.data);
    for (size_t i = 0; i < v->history_count; ++i)
        free(v->history[i]);
    free(v->history);
    free(v->draft.data);
    free(v->out.data);
    free(v->action.data);
    free(v->stash.data);
    free(v->matches);
    free(v->rows);
    free(v->pick_idx);
    free(v);
}

tui_action tui_view_key(tui_view *v, const tui_view_status *status, tui_key key, const char *text,
                        size_t count) {
    tui_action none = {TUI_NO_ACTION, NULL};
    if (!v)
        return none;
    if (auth_active(status))
        return auth_key(v, status->auth, key, text, count);
    v->was_auth = false;
    if (picker_active(status)) {
        if (!v->was_picker) {
            v->pick_filter = 0;
            v->pick_selected = v->pick_scroll = 0;
            v->was_picker = true;
        }
        return picker_key(v, status->picker, key, text, count);
    }
    v->was_picker = false;

    refresh_matches(v, status);
    bool palette = palette_open(v) && v->match_count;
    char *s = v->draft.data;
    size_t n = v->draft.len;
    switch (key) {
    case TUI_TEXT:
    case TUI_PASTE:
        if (text && count)
            (void)insert_text(v, text, count);
        break;
    case TUI_NEWLINE:
        (void)insert_text(v, "\n", 1);
        break;
    case TUI_ENTER:
        return enter(v, status);
    case TUI_TAB:
        if (palette) {
            const char *name = status->commands[v->matches[v->selected]].name;
            text_buf full = {0};
            if (buf_put(&full, "/", 1) && buf_puts(&full, name))
                set_draft(v, full.data, full.len);
            free(full.data);
        } else if (!n) {
            (void)insert_text(v, "/", 1);
        }
        break;
    case TUI_SHIFT_TAB:
        if (palette) {
            size_t m = v->match_count;
            v->selected = (v->selected + m - 1) % m;
        }
        break;
    case TUI_ESCAPE:
        if (palette_open(v)) {
            v->dismissed = true;
            break;
        }
        return status && status->busy ? (tui_action){TUI_CANCEL, NULL} : none;
    case TUI_UP:
    case TUI_DOWN:
        if (palette) {
            size_t m = v->match_count;
            v->selected = key == TUI_UP ? (v->selected + m - 1) % m : (v->selected + 1) % m;
        } else {
            vertical(v, v->cols, key == TUI_UP);
        }
        break;
    case TUI_LEFT:
        v->cursor = prev_cluster(s, n, v->cursor);
        v->goal = -1;
        break;
    case TUI_RIGHT:
        if (v->cursor < n) {
            unsigned w;
            uint32_t cp;
            v->cursor = next_cluster(s, n, v->cursor, &w, &cp);
        }
        v->goal = -1;
        break;
    case TUI_HOME:
        v->cursor = line_start(s, v->cursor);
        v->goal = -1;
        break;
    case TUI_END:
        while (v->cursor < n && s[v->cursor] != '\n')
            ++v->cursor;
        v->goal = -1;
        break;
    case TUI_BACKSPACE:
        if (v->cursor) {
            size_t at = prev_cluster(s, n, v->cursor);
            buf_erase(&v->draft, at, v->cursor - at);
            v->cursor = at;
            edited(v);
        }
        break;
    case TUI_DELETE:
    case TUI_EOF:
        if (key == TUI_EOF && !n)
            return status && status->busy ? none : (tui_action){TUI_QUIT, NULL};
        if (v->cursor < n) {
            unsigned w;
            uint32_t cp;
            size_t end = next_cluster(s, n, v->cursor, &w, &cp);
            buf_erase(&v->draft, v->cursor, end - v->cursor);
            edited(v);
        }
        break;
    case TUI_INTERRUPT:
        if (n) {
            v->draft.len = 0;
            v->draft.data[0] = 0;
            v->cursor = 0;
            edited(v);
            break;
        }
        return status && status->busy ? (tui_action){TUI_CANCEL, NULL}
                                      : (tui_action){TUI_QUIT, NULL};
    case TUI_SAVE:
    case TUI_REVEAL:
        break;
    }
    return none;
}

void tui_view_accept(tui_view *v, tui_action_kind kind) {
    if (!v || (kind != TUI_SUBMIT && kind != TUI_COMMAND))
        return;
    bool command = kind == TUI_COMMAND && v->action.data;
    const char *entry = command ? v->action.data : v->draft.data;
    size_t size = command ? v->action.len + 1 : v->draft.len;
    if (size) {
        const char *last = v->history_count ? v->history[v->history_count - 1] : NULL;
        char *copy = malloc(size + 1);
        if (copy) {
            if (command)
                copy[0] = '/';
            memcpy(copy + command, entry, size - command);
            copy[size] = 0;
        }
        if (copy && last && !strcmp(last, copy)) {
            free(copy);
        } else if (copy) {
            if (v->history_count == HISTORY_MAX) {
                free(v->history[0]);
                memmove(v->history, v->history + 1, (HISTORY_MAX - 1) * sizeof(*v->history));
                --v->history_count;
            }
            if (!v->history)
                v->history = malloc(HISTORY_MAX * sizeof(*v->history));
            if (v->history)
                v->history[v->history_count++] = copy;
            else
                free(copy);
        }
    }
    v->draft.len = 0;
    v->draft.data[0] = 0;
    v->cursor = v->scroll = 0;
    edited(v);
}

/* ---------- rendering helpers ---------- */

static void format_elapsed(char *out, size_t size, uint64_t ms) {
    uint64_t s = ms / 1000;
    if (s < 60)
        snprintf(out, size, "%us", (unsigned)s);
    else if (s < 3600)
        snprintf(out, size, "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
    else
        snprintf(out, size, "%uh %02um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
}

static bool newline(text_buf *out, unsigned *lines) {
    if (*lines && !buf_put(out, "\n", 1))
        return false;
    ++*lines;
    return true;
}

/* ---------- Command Center rendering ---------- */

static bool render_detail_line(text_buf *out, const char *s, size_t n, unsigned max) {
    // Highlight "Key: Value" lines with muted key and crisp value
    const char *colon = memchr(s, ':', n);
    if (colon && colon > s && (size_t)(colon - s) <= 18 &&
        (size_t)(colon - s + 1) < n && colon[1] == ' ') {
        size_t klen = (size_t)(colon - s);
        const char *val = colon + 2;
        size_t vlen = n - (size_t)(val - s);
        unsigned used = 0;
        if (!buf_puts(out, S_DIM) || !emit_line(out, s, klen, max, &used))
            return false;
        if (used + 2 < max) {
            if (!buf_puts(out, S_FAINT ": " S_TEXT))
                return false;
            used += 2;
            unsigned vused = 0;
            if (!emit_line(out, val, vlen, max - used, &vused))
                return false;
            used += vused;
        }
        return buf_puts(out, S_RST) && (used < max ? buf_repeat(out, " ", max - used) : true);
    }
    if (!buf_puts(out, S_TEXT))
        return false;
    return emit_padded(out, s, n, max) && buf_puts(out, S_RST);
}

static bool render_picker(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                          tui_frame *frame) {
    const tui_picker *p = status->picker;
    if (!v->was_picker) {
        v->pick_filter = 0;
        v->pick_selected = v->pick_scroll = 0;
        v->was_picker = true;
    }
    refresh_picker(v, p);

    unsigned width = frame_width(cols);
    unsigned budget = rows > 2 ? rows - 2 : 1;
    bool two_pane = width >= 72;
    unsigned left_w = two_pane ? (width * 60) / 100 : width;
    unsigned right_w = two_pane ? width - left_w - 1 : 0;

    text_buf *out = &v->out;
    out->len = 0;
    if (!buf_reserve(out, 2048))
        return false;
    out->data[0] = 0;
    unsigned lines = 0;

    // 1. Header row: Title + subtitle on left, "tab / shift+tab  filter" on right
    if (!newline(out, &lines))
        return false;
    const char *title = p->title && *p->title ? p->title : "Command center";
    const char *sub = p->subtitle && *p->subtitle ? p->subtitle : "";
    const char *rhint = "tab / shift+tab  filter";
    unsigned rlen = span_width(rhint, strlen(rhint));
    unsigned used = 0;
    if (!buf_puts(out, "  " S_BOLD S_TEXT))
        return false;
    used += 2;
    unsigned tw = 0;
    if (!emit_line(out, title, strlen(title), width > used ? width - used : 0, &tw))
        return false;
    used += tw;
    if (*sub && used + 4 < width) {
        if (!buf_puts(out, S_RST S_DIM "  \xc2\xb7  "))
            return false;
        used += 5;
        unsigned sw = 0;
        if (!emit_line(out, sub, strlen(sub), width > used ? width - used : 0, &sw))
            return false;
        used += sw;
    }
    if (!buf_puts(out, S_RST))
        return false;
    if (used + rlen + 4 <= width) {
        if (!buf_repeat(out, " ", width - used - rlen - 2) ||
            !buf_puts(out, S_KEY "tab" S_DIM "/" S_KEY "shift+tab" S_DIM "  filter" S_RST))
            return false;
    }

    // 2. Filter pills row
    if (lines < budget) {
        static const char *const labels[FILTER_COUNT] = {
            "All", "Attention", "Working", "Ready", "Other",
        };
        size_t counts[FILTER_COUNT] = {0};
        for (size_t i = 0; i < p->count; ++i)
            for (unsigned f = 0; f < FILTER_COUNT; ++f)
                if (item_matches_filter(&p->items[i], f))
                    ++counts[f];
        if (!newline(out, &lines) || !buf_puts(out, "  "))
            return false;
        unsigned col = 2;
        for (unsigned f = 0; f < FILTER_COUNT; ++f) {
            char pill[48];
            int plen = snprintf(pill, sizeof(pill), " %s %zu ", labels[f], counts[f]);
            if (plen <= 0)
                continue;
            unsigned pw = (unsigned)plen;
            if (col + pw + 1 > width)
                break;
            if (f == v->pick_filter) {
                if (!buf_puts(out, S_PILL_ON) || !buf_put(out, pill, (size_t)plen) ||
                    !buf_puts(out, S_RST " "))
                    return false;
            } else {
                const char *clr = f == 1 && counts[1] ? S_AMBER
                                  : f == 2 && counts[2] ? S_ACCENT
                                                        : S_DIM;
                if (!buf_puts(out, clr) || !buf_put(out, pill, (size_t)plen) ||
                    !buf_puts(out, S_RST " "))
                    return false;
            }
            col += pw + 1;
        }
    }

    // 3. Top hairline
    if (lines < budget) {
        if (!newline(out, &lines) || !buf_puts(out, S_BORDER))
            return false;
        if (two_pane) {
            if (!buf_repeat(out, "\xe2\x94\x80", left_w) || !buf_puts(out, "\xe2\x94\xac") ||
                !buf_repeat(out, "\xe2\x94\x80", right_w))
                return false;
        } else if (!buf_repeat(out, "\xe2\x94\x80", width)) {
            return false;
        }
        if (!buf_puts(out, S_RST))
            return false;
    }

    // Prepare right-pane wrapped details for the selected item
    const tui_picker_item *sel =
        v->pick_count ? &p->items[v->pick_idx[v->pick_selected]] : NULL;
    text_row *drows = NULL;
    size_t dcount = 0, dcap = 0;
    const char *detail = sel && sel->detail ? sel->detail : "";
    unsigned dwidth = right_w > 4 ? right_w - 4 : 2;
    if (two_pane && *detail) {
        if (!wrap(detail, strlen(detail), dwidth, &drows, &dcount, &dcap)) {
            free(drows);
            return false;
        }
    }

    // Body height: leave 2 rows for bottom hairline + shortcut footer
    unsigned body_max = budget > lines + 2 ? budget - lines - 2 : 1;
    if (body_max > 16)
        body_max = 16;
    unsigned list_rows = body_max > 1 ? body_max - 1 : 1;

    if (v->pick_scroll > v->pick_selected)
        v->pick_scroll = v->pick_selected;
    if (v->pick_selected >= v->pick_scroll + list_rows)
        v->pick_scroll = v->pick_selected + 1 - list_rows;
    if (v->pick_count > list_rows && v->pick_scroll + list_rows > v->pick_count)
        v->pick_scroll = v->pick_count - list_rows;
    if (v->pick_count <= list_rows)
        v->pick_scroll = 0;

    // 4. Column header row
    if (lines + 2 < budget) {
        if (!newline(out, &lines))
            goto fail;
        unsigned status_w = left_w > 40 ? 11 : 0;
        unsigned meta_w = left_w > 54 ? 14 : 0;
        unsigned title_w = left_w > 4 + status_w + meta_w ? left_w - 4 - status_w - meta_w : 4;
        if (!buf_puts(out, "  " S_DIM) || !emit_padded(out, "Item", 4, title_w))
            goto fail;
        if (status_w && !emit_padded(out, "Status", 6, status_w))
            goto fail;
        if (meta_w && !emit_padded(out, "Activity", 8, meta_w))
            goto fail;
        if (!buf_puts(out, "  " S_RST))
            goto fail;
        if (two_pane) {
            if (!buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  " S_BOLD S_TEXT) ||
                !emit_padded(out, "Details", 7, right_w > 2 ? right_w - 2 : 0) ||
                !buf_puts(out, S_RST))
                goto fail;
        }
    }

    // 5. List & Details rows
    unsigned thumb_pos = 0, thumb_len = list_rows;
    bool scrollable = v->pick_count > list_rows;
    if (scrollable) {
        thumb_len = (unsigned)((list_rows * list_rows) / v->pick_count);
        if (thumb_len < 1)
            thumb_len = 1;
        size_t max_scroll = v->pick_count - list_rows;
        thumb_pos = max_scroll ? (unsigned)((v->pick_scroll * (list_rows - thumb_len)) / max_scroll)
                               : 0;
    }

    for (unsigned r = 0; r < list_rows && lines + 2 < budget; ++r) {
        if (!newline(out, &lines))
            goto fail;
        size_t idx = v->pick_scroll + r;
        unsigned inner_l = left_w > 1 ? left_w - 1 : 1; // 1 col reserved for scrollbar/gap
        if (v->pick_count == 0) {
            if (r == 1) {
                if (!buf_puts(out, S_DIM "  ") ||
                    !emit_padded(out, "No matching items", 17, inner_l > 2 ? inner_l - 2 : 0) ||
                    !buf_puts(out, S_RST " "))
                    goto fail;
            } else if (!buf_repeat(out, " ", left_w)) {
                goto fail;
            }
        } else if (idx < v->pick_count) {
            const tui_picker_item *it = &p->items[v->pick_idx[idx]];
            bool is_sel = idx == v->pick_selected;
            unsigned status_w = inner_l > 38 ? 11 : 0;
            unsigned meta_w = inner_l > 52 ? 14 : 0;
            unsigned lead_w = 5 + (it->indent < 6 ? it->indent * 2 : 12);
            unsigned title_w = inner_l > lead_w + status_w + meta_w
                                   ? inner_l - lead_w - status_w - meta_w
                                   : 4;
            const char *st = it->status ? it->status : "";
            const char *meta = it->meta ? it->meta : "";
            const char *ititle = it->title ? it->title : (it->id ? it->id : "");
            if (is_sel) {
                if (!buf_puts(out, S_SEL_BOLD " \xe2\x80\xba ")) // ›
                    goto fail;
                for (unsigned k = 0; k < (it->indent < 6 ? it->indent : 6); ++k)
                    if (!buf_puts(out, "  "))
                        goto fail;
                if (!buf_puts(out, status_dot(st, it->attention, it->working)) ||
                    !buf_puts(out, " ") ||
                    !emit_padded(out, ititle, strlen(ititle), title_w))
                    goto fail;
                if (status_w) {
                    if (!buf_puts(out, S_SEL_DESC) ||
                        !emit_padded(out, st, strlen(st), status_w > 2 ? status_w - 2 : status_w) ||
                        (status_w > 2 && !buf_put(out, "  ", 2)))
                        goto fail;
                }
                if (meta_w) {
                    if (!buf_puts(out, S_SEL_DESC) ||
                        !emit_padded(out, meta, strlen(meta), meta_w))
                        goto fail;
                }
                if (!buf_puts(out, S_RST))
                    goto fail;
            } else {
                if (!buf_puts(out, it->active ? S_ACCENT " \xe2\x80\xa2 " : "   "))
                    goto fail;
                for (unsigned k = 0; k < (it->indent < 6 ? it->indent : 6); ++k)
                    if (!buf_puts(out, "  "))
                        goto fail;
                if (!buf_puts(out, status_color(st, it->attention, it->working)) ||
                    !buf_puts(out, status_dot(st, it->attention, it->working)) ||
                    !buf_puts(out, " ") ||
                    !buf_puts(out, it->active ? S_BOLD S_TEXT : S_TEXT) ||
                    !emit_padded(out, ititle, strlen(ititle), title_w) ||
                    !buf_puts(out, S_RST))
                    goto fail;
                if (status_w) {
                    if (!buf_puts(out, status_color(st, it->attention, it->working)) ||
                        !emit_padded(out, st, strlen(st), status_w > 2 ? status_w - 2 : status_w) ||
                        (status_w > 2 && !buf_put(out, "  ", 2)) || !buf_puts(out, S_RST))
                        goto fail;
                }
                if (meta_w) {
                    if (!buf_puts(out, S_DIM) || !emit_padded(out, meta, strlen(meta), meta_w) ||
                        !buf_puts(out, S_RST))
                        goto fail;
                }
            }
            // Right-edge scrollbar
            if (scrollable) {
                bool on = r >= thumb_pos && r < thumb_pos + thumb_len;
                if (!buf_puts(out, on ? S_DIM "\xe2\x94\x83" S_RST : S_FAINT "\xe2\x94\x82" S_RST))
                    goto fail;
            } else if (!buf_put(out, " ", 1)) {
                goto fail;
            }
        } else if (!buf_repeat(out, " ", left_w)) {
            goto fail;
        }

        // Right pane detail row
        if (two_pane) {
            if (!buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  "))
                goto fail;
            if (sel && r < dcount) {
                const text_row *dr = &drows[r];
                if (!render_detail_line(out, detail + dr->start, dr->end - dr->start, dwidth))
                    goto fail;
            } else if (sel && r == 0 && !dcount) {
                if (!buf_puts(out, S_DIM) || !emit_padded(out, "No details", 10, dwidth) ||
                    !buf_puts(out, S_RST))
                    goto fail;
            }
        }
    }

    // 6. Bottom hairline
    if (lines < budget) {
        if (!newline(out, &lines) || !buf_puts(out, S_BORDER))
            goto fail;
        if (two_pane) {
            if (!buf_repeat(out, "\xe2\x94\x80", left_w) || !buf_puts(out, "\xe2\x94\xb4") ||
                !buf_repeat(out, "\xe2\x94\x80", right_w))
                goto fail;
        } else if (!buf_repeat(out, "\xe2\x94\x80", width)) {
            goto fail;
        }
        if (!buf_puts(out, S_RST))
            goto fail;
    }

    // 7. Footer shortcut bar
    if (lines < budget) {
        if (!newline(out, &lines) || !buf_puts(out, "  " S_KEY "\xe2\x86\x91/\xe2\x86\x93" S_DIM
                                                    " move  \xc2\xb7  " S_KEY "tab" S_DIM
                                                    " filter"))
            goto fail;
        if (p->can_open && !buf_puts(out, "  \xc2\xb7  " S_KEY "enter" S_DIM " open"))
            goto fail;
        if (p->can_approve &&
            !buf_puts(out, "  \xc2\xb7  " S_GREEN "a" S_DIM " approve  \xc2\xb7  " S_RED "d" S_DIM
                           " deny"))
            goto fail;
        if (p->can_new && !buf_puts(out, "  \xc2\xb7  " S_KEY "n" S_DIM " new"))
            goto fail;
        if (!buf_puts(out, "  \xc2\xb7  " S_KEY "esc" S_DIM " back" S_RST))
            goto fail;
    }

    free(drows);
    frame->text = out->data;
    frame->lines = lines ? lines : 1;
    frame->cursor_row = frame->lines - 1;
    frame->cursor_col = 0;
    frame->hide_cursor = true;
    return true;
fail:
    free(drows);
    return false;
}

/* ---------- Inline Chat View rendering ---------- */

static bool append_meta_border(text_buf *out, const tui_view_status *status, unsigned width,
                               const char *bcolor) {
    // Build embedded bottom-right metadata: " model · state · turn N "
    const char *model = status->model && *status->model ? status->model : NULL;
    const char *state = status->state && *status->state ? status->state : NULL;
    char turn_buf[32] = "";
    if (status->turn)
        snprintf(turn_buf, sizeof(turn_buf), "turn %u", status->turn);

    unsigned meta_w = 0;
    bool has_meta = false;
    if (model) {
        meta_w += span_width(model, strlen(model));
        has_meta = true;
    }
    if (state) {
        meta_w += (has_meta ? 3 : 0) + span_width(state, strlen(state));
        has_meta = true;
    }
    if (turn_buf[0]) {
        meta_w += (has_meta ? 3 : 0) + span_width(turn_buf, strlen(turn_buf));
        has_meta = true;
    }
    if (status->has_pending) {
        meta_w += (has_meta ? 3 : 0) + 9; // "! pending"
        has_meta = true;
    }

    if (!buf_puts(out, bcolor) || !buf_puts(out, "\xe2\x95\xb0")) // ╰
        return false;
    if (has_meta && width >= meta_w + 6) {
        unsigned fill = width - 2 - (meta_w + 2) - 1;
        if (!buf_repeat(out, "\xe2\x94\x80", fill) || !buf_put(out, " ", 1))
            return false;
        bool sep = false;
        if (model) {
            if (!buf_puts(out, S_GOLD) || !buf_puts(out, model))
                return false;
            sep = true;
        }
        if (state) {
            if (sep && !buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!buf_puts(out, status_color(state, status->has_pending, status->busy)) ||
                !buf_puts(out, state))
                return false;
            sep = true;
        }
        if (turn_buf[0]) {
            if (sep && !buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!buf_puts(out, S_DIM) || !buf_puts(out, turn_buf))
                return false;
            sep = true;
        }
        if (status->has_pending) {
            if (sep && !buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!buf_puts(out, S_AMBER "! pending"))
                return false;
        }
        if (!buf_puts(out, " ") || !buf_puts(out, bcolor) ||
            !buf_puts(out, "\xe2\x94\x80\xe2\x95\xaf" S_RST)) // ─╯
            return false;
    } else {
        if (!buf_repeat(out, "\xe2\x94\x80", width - 2) || !buf_puts(out, "\xe2\x95\xaf" S_RST))
            return false;
    }
    return true;
}

/* ---------- Login & API Configuration rendering ---------- */

static bool emit_field_box_row(text_buf *out, const char *val, size_t vlen, size_t cur_byte,
                               const char *placeholder, bool masked, bool focused,
                               unsigned box_w, unsigned *out_cursor_offset) {
    unsigned inner = box_w > 4 ? box_w - 4 : 2; // "│ " + inner + " │"
    const char *bcolor = focused ? S_BORDER_HI : S_BORDER;
    if (!buf_puts(out, bcolor) || !buf_puts(out, "\xe2\x94\x82 " S_RST))
        return false;
    if (!vlen) {
        if (focused && out_cursor_offset)
            *out_cursor_offset = 2;
        if (!buf_puts(out, S_FAINT) ||
            !emit_padded(out, placeholder, strlen(placeholder), inner) || !buf_puts(out, S_RST))
            return false;
    } else if (masked) {
        // Count clusters up to cursor and total clusters
        size_t total = 0, cur_idx = 0;
        for (size_t pos = 0; pos < vlen;) {
            if (pos < cur_byte)
                ++cur_idx;
            unsigned w;
            uint32_t cp;
            pos = next_cluster(val, vlen, pos, &w, &cp);
            ++total;
        }
        if (cur_byte >= vlen)
            cur_idx = total;
        size_t skip = cur_idx >= inner ? cur_idx - inner + 1 : 0;
        size_t show = total > skip ? total - skip : 0;
        if (show > inner)
            show = inner;
        if (!buf_puts(out, S_TEXT) || !buf_repeat(out, "\xe2\x80\xa2", show) ||
            !buf_puts(out, S_RST))
            return false;
        if (show < inner && !buf_repeat(out, " ", inner - show))
            return false;
        if (focused && out_cursor_offset)
            *out_cursor_offset = 2 + (unsigned)(cur_idx - skip);
    } else {
        // Horizontal scroll so cursor is always visible inside inner
        unsigned cur_col = column_of(val, 0, cur_byte <= vlen ? cur_byte : vlen);
        unsigned skip_col = cur_col >= inner ? cur_col - inner + 1 : 0;
        size_t start = 0;
        unsigned col = 0;
        while (start < vlen && col < skip_col) {
            unsigned w;
            uint32_t cp;
            start = next_cluster(val, vlen, start, &w, &cp);
            col += w;
        }
        unsigned used = 0;
        if (!buf_puts(out, S_TEXT) ||
            !emit_line(out, val + start, vlen - start, inner, &used) || !buf_puts(out, S_RST))
            return false;
        if (used < inner && !buf_repeat(out, " ", inner - used))
            return false;
        if (focused && out_cursor_offset) {
            unsigned off = cur_col >= col ? cur_col - col : 0;
            *out_cursor_offset = 2 + (off < inner ? off : inner - 1);
        }
    }
    return buf_puts(out, " ") && buf_puts(out, bcolor) && buf_puts(out, "\xe2\x94\x82" S_RST);
}

static bool emit_auth_right_row(text_buf *out, unsigned row, const tui_view *v,
                                const tui_auth_state *a, unsigned right_w) {
    if (!right_w)
        return true;
    unsigned rw = right_w > 3 ? right_w - 3 : 1;
    if (!buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  "))
        return false;
    if (v->auth_preset == 0) {
        const char *label = row == 0 ? "ChatGPT subscription"
                            : row == 2 ? "Sign in through your browser"
                            : row == 4 ? "Storage: macOS Keychain" : "";
        return buf_puts(out, row == 0 ? S_BOLD S_TEXT : S_DIM) &&
               emit_padded(out, label, strlen(label), rw) && buf_puts(out, S_RST);
    }
    const char *ep = v->auth_endpoint.data ? v->auth_endpoint.data : "";
    bool ep_ok = !strncmp(ep, "https://", 8) || !strncmp(ep, "http://127.0.0.1", 16) ||
                 !strncmp(ep, "http://localhost", 16) || !strncmp(ep, "http://[::1]", 12);
    bool can_keep = a->has_token && !a->chatgpt && a->endpoint && !strcmp(ep, a->endpoint);
    bool tok_ok = v->auth_token.len > 0 || can_keep;
    const char *mdl = v->auth_model.data && v->auth_model.len ? v->auth_model.data : "-";
    const char *cfg = a->config_path && *a->config_path ? a->config_path : "~/.config/ah/tui.json";
    char line[256] = "";
    switch (row) {
    case 0:
        return buf_puts(out, S_BOLD S_TEXT) && emit_padded(out, "Connection", 10, rw) &&
               buf_puts(out, S_RST);
    case 2:
        if (v->auth_token.len)
            snprintf(line, sizeof(line), "\xe2\x97\x8f New key ready (%zu chars)", v->auth_token.len);
        else if (can_keep)
            snprintf(line, sizeof(line), "\xe2\x97\x8f Configured (%s)",
                     a->token_source && *a->token_source ? a->token_source : "saved");
        else
            snprintf(line, sizeof(line), "\xe2\x96\xb2 API key required");
        return buf_puts(out, tok_ok ? S_GREEN : S_AMBER) &&
               emit_padded(out, line, strlen(line), rw) && buf_puts(out, S_RST);
    case 3:
        snprintf(line, sizeof(line), "%s %s", ep_ok ? "\xe2\x97\x8f" : "\xe2\x96\xb2",
                 ep_ok ? "Responses URL entered" : "Enter an HTTPS URL");
        return buf_puts(out, ep_ok ? S_GREEN : S_AMBER) &&
               emit_padded(out, line, strlen(line), rw) && buf_puts(out, S_RST);
    case 4:
        snprintf(line, sizeof(line), "Model: %s", mdl);
        return render_detail_line(out, line, strlen(line), rw);
    case 6:
        return buf_puts(out, S_BOLD S_TEXT) && emit_padded(out, "Storage", 7, rw) &&
               buf_puts(out, S_RST);
    case 7:
        return buf_puts(out, S_LAVENDER) && emit_padded(out, cfg, strlen(cfg), rw) &&
               buf_puts(out, S_RST);
    case 8:
        return buf_puts(out, S_DIM) && emit_padded(out, "Mode 0600 (owner-only)", 22, rw) &&
               buf_puts(out, S_RST);
    case 10:
        return buf_puts(out, S_BOLD S_TEXT) && emit_padded(out, "Tips", 4, rw) &&
               buf_puts(out, S_RST);
    case 11:
        return buf_puts(out, S_DIM) && emit_padded(out, "Paste API key directly", 22, rw) &&
               buf_puts(out, S_RST);
    case 12:
        return buf_puts(out, S_DIM) && emit_padded(out, "Press ^r to show/hide key", 25, rw) &&
               buf_puts(out, S_RST);
    case 13:
        return buf_puts(out, S_DIM) && emit_padded(out, "Press ^s to save & apply", 24, rw) &&
               buf_puts(out, S_RST);
    default:
        return buf_repeat(out, " ", rw);
    }
}

static bool render_auth(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                        tui_frame *frame) {
    const tui_auth_state *a = status->auth;
    if (!v->was_auth)
        auth_init(v, a);

    unsigned width = frame_width(cols);
    unsigned budget = rows > 2 ? rows - 2 : 1;
    bool two_pane = width >= 76;
    unsigned left_w = two_pane ? (width * 62) / 100 : width;
    unsigned right_w = two_pane ? width - left_w - 1 : 0;
    unsigned box_w = left_w > 6 ? left_w - 4 : (left_w > 2 ? left_w - 2 : 2);
    bool chatgpt = v->auth_preset == 0;

    text_buf *out = &v->out;
    out->len = 0;
    if (!buf_reserve(out, 2048))
        return false;
    out->data[0] = 0;
    unsigned lines = 0, cursor_row = 0, cursor_col = 0;
    bool hide_cursor = true;

    // 1. Header row
    if (!newline(out, &lines))
        return false;
    const char *title = a->onboarding ? ">_ ah  \xc2\xb7  Sign in & API setup"
                                      : "API & Account  \xc2\xb7  Configuration";
    char rhint[34];
    snprintf(rhint, sizeof(rhint), "^s %s  ·  esc %s",
             chatgpt ? "connect" : "save", a->onboarding ? "quit" : "back");
    unsigned rlen = span_width(rhint, strlen(rhint));
    unsigned used = 2;
    if (!buf_puts(out, "  " S_BOLD S_TEXT))
        return false;
    unsigned tw = 0;
    if (!emit_line(out, title, strlen(title), width > used ? width - used : 0, &tw) ||
        !buf_puts(out, S_RST))
        return false;
    used += tw;
    if (used + rlen + 4 <= width) {
        if (!buf_repeat(out, " ", width - used - rlen - 2) ||
            !buf_puts(out, S_KEY "^s" S_DIM) ||
            !buf_puts(out, chatgpt ? " connect" : " save") ||
            !buf_puts(out, "  \xc2\xb7  " S_KEY "esc" S_DIM) ||
            !buf_puts(out, a->onboarding ? " quit" S_RST : " back" S_RST))
            return false;
    }

    // 2. Top hairline
    if (lines < budget) {
        if (!newline(out, &lines) || !buf_puts(out, S_BORDER))
            return false;
        if (two_pane) {
            if (!buf_repeat(out, "\xe2\x94\x80", left_w) || !buf_puts(out, "\xe2\x94\xac") ||
                !buf_repeat(out, "\xe2\x94\x80", right_w))
                return false;
        } else if (!buf_repeat(out, "\xe2\x94\x80", width)) {
            return false;
        }
        if (!buf_puts(out, S_RST))
            return false;
    }

    unsigned rrow = 0;
    // Macro-free helper pattern: emit left pane of exact width left_w, then right pane row rrow++
    // Row 0: Provider label + preset pills
    if (lines + 2 < budget) {
        if (!newline(out, &lines))
            return false;
        bool f0 = v->auth_focus == 0;
        if (!buf_puts(out, "  ") || !buf_puts(out, f0 ? S_ACCENT "\xe2\x80\xba " S_BOLD S_TEXT
                                                      : "  " S_DIM) ||
            !buf_puts(out, "Provider   " S_RST))
            return false;
        unsigned col = 15;
        for (unsigned p = 0; p < PRESET_COUNT; ++p) {
            unsigned plen = (unsigned)strlen(preset_names[p]) + 2;
            if (col + plen + 1 > left_w)
                break;
            if (p == v->auth_preset) {
                if (!buf_puts(out, f0 ? S_SEL_BOLD : S_PILL_ON) || !buf_put(out, " ", 1) ||
                    !buf_puts(out, preset_names[p]) || !buf_puts(out, " " S_RST " "))
                    return false;
            } else {
                if (!buf_puts(out, S_DIM " ") || !buf_puts(out, preset_names[p]) ||
                    !buf_puts(out, " " S_RST " "))
                    return false;
            }
            col += plen + 1;
        }
        if (col < left_w && !buf_repeat(out, " ", left_w - col))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Blank spacer
    if (lines + 2 < budget) {
        if (!newline(out, &lines) || !buf_repeat(out, " ", left_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Text fields: 1=Endpoint, 2=API Key, 3=Model
    const char *ep = v->auth_endpoint.data ? v->auth_endpoint.data : "";
    bool can_keep = a->has_token && !a->chatgpt && a->endpoint && !strcmp(ep, a->endpoint);
    const char *tok_ph = can_keep ? "Configured — leave blank to keep" : "Paste the API key";

    const char *labels[3] = {"API Endpoint", "API Key / Token", "Default Model"};
    const char *placeholders[3] = {
        "https://api.openai.com/v1/responses",
        tok_ph,
        "gpt-5",
    };
    const text_buf *fbufs[3] = {&v->auth_endpoint, &v->auth_token, &v->auth_model};

    for (unsigned fi = 0; fi < 3; ++fi) {
        if (chatgpt && fi < 2)
            continue;
        unsigned focus_id = fi + 1;
        bool focused = v->auth_focus == focus_id;
        const char *bcolor = focused ? S_BORDER_HI : S_BORDER;
        if (lines + 4 >= budget)
            break;

        // Label row
        if (!newline(out, &lines))
            return false;
        if (!buf_puts(out, "  ") || !buf_puts(out, focused ? S_ACCENT "\xe2\x80\xba " S_BOLD S_TEXT
                                                           : "  " S_DIM))
            return false;
        unsigned lw = 4;
        unsigned lu = 0;
        if (!emit_line(out, labels[fi], strlen(labels[fi]), left_w > lw ? left_w - lw : 0, &lu) ||
            !buf_puts(out, S_RST))
            return false;
        lw += lu;
        if (fi == 1) {
            const char *hint = v->auth_reveal ? "visible (^r hide)" : "masked (^r show)";
            unsigned hw = (unsigned)strlen(hint);
            if (lw + hw + 3 <= left_w) {
                if (!buf_repeat(out, " ", left_w - lw - hw - 2) || !buf_puts(out, S_FAINT) ||
                    !buf_puts(out, hint) || !buf_puts(out, S_RST "  "))
                    return false;
                lw = left_w;
            }
        }
        if (lw < left_w && !buf_repeat(out, " ", left_w - lw))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box top
        if (!newline(out, &lines) || !buf_puts(out, "  ") || !buf_puts(out, bcolor) ||
            !buf_puts(out, "\xe2\x95\xad") || !buf_repeat(out, "\xe2\x94\x80", box_w - 2) ||
            !buf_puts(out, "\xe2\x95\xae" S_RST))
            return false;
        if (2 + box_w < left_w && !buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box content
        if (!newline(out, &lines) || !buf_puts(out, "  "))
            return false;
        unsigned coff = 0;
        if (!emit_field_box_row(out, fbufs[fi]->data ? fbufs[fi]->data : "", fbufs[fi]->len,
                                v->auth_cursor[fi], placeholders[fi],
                                fi == 1 && !v->auth_reveal, focused, box_w, &coff))
            return false;
        if (focused) {
            cursor_row = lines - 1;
            cursor_col = 2 + coff;
            hide_cursor = false;
        }
        if (2 + box_w < left_w && !buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box bottom
        if (!newline(out, &lines) || !buf_puts(out, "  ") || !buf_puts(out, bcolor) ||
            !buf_puts(out, "\xe2\x95\xb0") || !buf_repeat(out, "\xe2\x94\x80", box_w - 2) ||
            !buf_puts(out, "\xe2\x95\xaf" S_RST))
            return false;
        if (2 + box_w < left_w && !buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Inline error banner (if any)
    if (a->error && *a->error && lines + 3 < budget) {
        if (!newline(out, &lines) || !buf_puts(out, "  " S_RED "\xe2\x96\x8e "))
            return false;
        unsigned eu = 0;
        if (!emit_line(out, a->error, strlen(a->error), left_w > 5 ? left_w - 5 : 1, &eu) ||
            !buf_puts(out, S_RST))
            return false;
        if (4 + eu < left_w && !buf_repeat(out, " ", left_w - 4 - eu))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Action button row
    if (lines + 2 < budget) {
        if (!newline(out, &lines))
            return false;
        bool fsave = v->auth_focus == 4;
        const char *btn = a->waiting ? " Connecting… "
                          : chatgpt ? " Sign in with ChatGPT "
                          : a->onboarding ? " Save & Continue " : " Save & Apply ";
        unsigned bw = (unsigned)strlen(btn);
        unsigned col = 4;
        if (!buf_puts(out, fsave ? "  " S_ACCENT "\xe2\x80\xba " S_SEL_BOLD : "    " S_PILL_ON) ||
            !buf_puts(out, btn) || !buf_puts(out, S_RST))
            return false;
        col += bw;
        if (!a->onboarding && col + 12 <= left_w) {
            if (!buf_puts(out, "   " S_DIM "esc cancel" S_RST))
                return false;
            col += 13;
        }
        if (col < left_w && !buf_repeat(out, " ", left_w - col))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Bottom hairline
    if (lines < budget) {
        if (!newline(out, &lines) || !buf_puts(out, S_BORDER))
            return false;
        if (two_pane) {
            if (!buf_repeat(out, "\xe2\x94\x80", left_w) || !buf_puts(out, "\xe2\x94\xb4") ||
                !buf_repeat(out, "\xe2\x94\x80", right_w))
                return false;
        } else if (!buf_repeat(out, "\xe2\x94\x80", width)) {
            return false;
        }
        if (!buf_puts(out, S_RST))
            return false;
    }

    // Footer shortcut bar
    if (lines < budget) {
        if (!newline(out, &lines) ||
            !buf_puts(out, "  " S_KEY "\xe2\x86\x91/\xe2\x86\x93" S_DIM " field  \xc2\xb7  " S_KEY
                           "\xe2\x86\x90/\xe2\x86\x92" S_DIM " preset/cursor  \xc2\xb7  ") ||
            (!chatgpt && !buf_puts(out, S_KEY "^r" S_DIM " reveal  \xc2\xb7  ")) ||
            !buf_puts(out, chatgpt ? S_KEY "^s" S_DIM " connect  \xc2\xb7  "
                                  : S_KEY "^s" S_DIM " save  \xc2\xb7  ") ||
            !buf_puts(out, S_KEY "esc" S_DIM))
            return false;
        if (!buf_puts(out, a->onboarding ? " quit" S_RST : " back" S_RST))
            return false;
    }

    frame->text = out->data;
    frame->lines = lines ? lines : 1;
    frame->cursor_row = hide_cursor ? frame->lines - 1 : cursor_row;
    frame->cursor_col = hide_cursor ? 0 : cursor_col;
    frame->hide_cursor = hide_cursor;
    return true;
}

bool tui_view_frame(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                    uint64_t now_ms, tui_frame *frame) {
    if (!v || !frame)
        return false;
    tui_view_status none = {0};
    if (!status)
        status = &none;
    v->cols = cols;

    if (auth_active(status))
        return render_auth(v, status, cols, rows, frame);
    v->was_auth = false;

    if (picker_active(status))
        return render_picker(v, status, cols, rows, frame);
    v->was_picker = false;

    unsigned width = frame_width(cols);
    unsigned budget = rows > 1 ? rows - 1 : 1;
    bool boxed = boxed_mode(cols, rows);
    if (status->busy && !v->was_busy)
        v->busy_since = now_ms;
    v->was_busy = status->busy;

    if (!layout_draft(v, cols, boxed))
        return false;
    refresh_matches(v, status);

    size_t pal_items = v->match_count < PALETTE_ROWS ? v->match_count : PALETTE_ROWS;
    size_t pal_rows = pal_items ? (boxed ? pal_items + 2 : pal_items) : 0;
    bool has_notice = status->notice && *status->notice;
    size_t busy_rows = status->busy ? 1 : 0;
    size_t notice_rows = has_notice ? 1 : 0;
    size_t footer_rows = boxed ? 1 : 0;
    size_t box_chrome = boxed ? 2 : 0;
    size_t ed_need = v->row_count;

    // Preview tail
    text_row *prow = NULL;
    size_t pcount = 0, pcap = 0, preview_need = 0;
    const char *preview = status->preview;
    size_t plen = preview ? strlen(preview) : 0;
    unsigned prev_w = width > 3 ? width - 2 : width;
    if (plen && !wrap(preview, plen, prev_w, &prow, &pcount, &pcap)) {
        free(prow);
        return false;
    }
    while (pcount && prow[pcount - 1].start == prow[pcount - 1].end)
        --pcount;
    preview_need = pcount < PREVIEW_ROWS ? pcount : PREVIEW_ROWS;

    // Row budget allocation: composer box first, then palette, status, footer, preview
    size_t ed = ed_need;
    size_t max_ed = budget > box_chrome ? budget - box_chrome : 1;
    if (ed > max_ed)
        ed = max_ed;
    if (ed + box_chrome + pal_rows + busy_rows + notice_rows + footer_rows > budget) {
        size_t keep = box_chrome + pal_rows + busy_rows + notice_rows + footer_rows;
        ed = budget > keep ? budget - keep : 1;
        if (ed > ed_need)
            ed = ed_need;
        if (ed + box_chrome > budget)
            boxed = false, box_chrome = 0, footer_rows = 0, pal_rows = pal_items;
    }
    size_t left = budget > ed + box_chrome ? budget - ed - box_chrome : 0;
    if (pal_rows > left) {
        if (boxed && left >= 3) {
            pal_items = left - 2;
            pal_rows = left;
        } else if (!boxed && left >= 1) {
            pal_items = left;
            pal_rows = left;
        } else {
            pal_items = pal_rows = 0;
        }
    }
    left -= pal_rows;
    size_t show_busy = busy_rows <= left ? busy_rows : 0;
    left -= show_busy;
    size_t show_notice = notice_rows <= left ? notice_rows : 0;
    left -= show_notice;
    size_t show_footer = footer_rows <= left ? footer_rows : 0;
    left -= show_footer;
    size_t show_prev = preview_need < left ? preview_need : left;

    text_buf *out = &v->out;
    out->len = 0;
    if (!buf_reserve(out, 1024))
        goto fail;
    out->data[0] = 0;
    unsigned lines = 0, cursor_row = 0, cursor_col = 0;

    // 1. Streaming preview rail
    for (size_t i = pcount - show_prev; i < pcount; ++i) {
        if (!newline(out, &lines) || !buf_puts(out, S_FAINT "\xe2\x94\x82 " S_TEXT) ||
            !emit_line(out, preview + prow[i].start, prow[i].end - prow[i].start, prev_w, NULL) ||
            !buf_puts(out, S_RST))
            goto fail;
    }

    // 2. Busy strip (smooth Braille spinner + elapsed + right-aligned esc hint)
    if (show_busy) {
        char elapsed[32];
        format_elapsed(elapsed, sizeof(elapsed), now_ms - v->busy_since);
        const char *spin = spinner[(now_ms / SPIN_MS) % SPIN_FRAMES];
        const char *verb = plen ? "Streaming" : "Working";
        const char *hint = "esc to cancel";
        unsigned used = 0;
        if (!newline(out, &lines) || !buf_puts(out, " ") || !buf_puts(out, S_ACCENT) ||
            !buf_puts(out, spin) || !buf_puts(out, " " S_TEXT) || !buf_puts(out, verb) ||
            !buf_puts(out, S_DIM "  ") || !buf_puts(out, elapsed) || !buf_puts(out, S_RST))
            goto fail;
        used = 1 + 1 + 1 + (unsigned)strlen(verb) + 2 + (unsigned)strlen(elapsed);
        unsigned hlen = (unsigned)strlen(hint);
        if (used + hlen + 4 <= width) {
            if (!buf_repeat(out, " ", width - used - hlen - 1) ||
                !buf_puts(out, S_FAINT) || !buf_puts(out, hint) || !buf_puts(out, S_RST))
                goto fail;
        }
    }

    // 3. Notice strip
    if (show_notice) {
        const char *bar = status->notice_error ? S_RED : status->has_pending ? S_AMBER : S_ACCENT;
        if (!newline(out, &lines) || !buf_puts(out, " ") || !buf_puts(out, bar) ||
            !buf_puts(out, "\xe2\x96\x8e " S_TEXT) ||
            !emit_line(out, status->notice, strlen(status->notice),
                       width > 4 ? width - 4 : width, NULL) ||
            !buf_puts(out, S_RST))
            goto fail;
    }

    // 4. Floating Slash Command Palette ABOVE the composer box
    if (pal_items) {
        size_t top = v->selected >= pal_items ? v->selected + 1 - pal_items : 0;
        unsigned name_w = 0;
        for (size_t i = top; i < top + pal_items; ++i) {
            unsigned w = span_width(status->commands[v->matches[i]].name,
                                    strlen(status->commands[v->matches[i]].name));
            if (w > name_w)
                name_w = w;
        }
        unsigned inner = boxed ? width - 2 : width;
        if (boxed) {
            char count_tag[32];
            snprintf(count_tag, sizeof(count_tag), " %zu/%zu ", v->match_count,
                     status->command_count);
            unsigned clen = (unsigned)strlen(count_tag);
            const char *hdr = " Commands ";
            unsigned hlen = (unsigned)strlen(hdr);
            if (!newline(out, &lines) || !buf_puts(out, S_BORDER "\xe2\x95\xad\xe2\x94\x80"))
                goto fail;
            if (width >= hlen + clen + 5) {
                if (!buf_puts(out, S_DIM) || !buf_puts(out, hdr) || !buf_puts(out, S_BORDER) ||
                    !buf_repeat(out, "\xe2\x94\x80", width - 3 - hlen - clen - 1) ||
                    !buf_puts(out, S_FAINT) || !buf_puts(out, count_tag) ||
                    !buf_puts(out, S_BORDER "\xe2\x94\x80\xe2\x95\xae" S_RST))
                    goto fail;
            } else {
                if (!buf_repeat(out, "\xe2\x94\x80", width - 3) ||
                    !buf_puts(out, "\xe2\x95\xae" S_RST))
                    goto fail;
            }
        }
        bool scrollable = v->match_count > pal_items;
        unsigned thumb_len = 1, thumb_pos = 0;
        if (scrollable) {
            thumb_len = (unsigned)((pal_items * pal_items) / v->match_count);
            if (thumb_len < 1)
                thumb_len = 1;
            size_t max_top = v->match_count - pal_items;
            thumb_pos = max_top ? (unsigned)((top * (pal_items - thumb_len)) / max_top) : 0;
        }
        for (size_t i = top; i < top + pal_items; ++i) {
            const tui_command *c = &status->commands[v->matches[i]];
            bool sel = i == v->selected;
            if (!newline(out, &lines))
                goto fail;
            if (boxed && !buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST))
                goto fail;
            unsigned row_used = 0;
            if (sel) {
                if (!buf_puts(out, S_SEL_BOLD " \xe2\x80\xba /"))
                    goto fail;
            } else {
                if (!buf_puts(out, S_TEXT "   /"))
                    goto fail;
            }
            row_used += 4;
            unsigned nw = 0;
            if (!emit_line(out, c->name, strlen(c->name),
                           inner > row_used ? inner - row_used : 0, &nw))
                goto fail;
            row_used += nw;
            if (c->description && *c->description && row_used + 2 < inner) {
                unsigned target = 4 + name_w + 3;
                if (target > inner)
                    target = row_used + 2;
                unsigned gap = target > row_used ? target - row_used : 1;
                if (!buf_repeat(out, " ", gap))
                    goto fail;
                row_used += gap;
                if (!buf_puts(out, sel ? S_SEL_DESC : S_DIM))
                    goto fail;
                unsigned dw = 0;
                if (!emit_line(out, c->description, strlen(c->description),
                               inner > row_used + 1 ? inner - row_used - 1 : 0, &dw))
                    goto fail;
                row_used += dw;
            }
            if (row_used < inner && !buf_repeat(out, " ", inner - row_used))
                goto fail;
            if (!buf_puts(out, S_RST))
                goto fail;
            if (boxed) {
                size_t r = i - top;
                bool on = scrollable && r >= thumb_pos && r < thumb_pos + thumb_len;
                if (!buf_puts(out, on ? S_ACCENT "\xe2\x94\x83" S_RST
                                      : S_BORDER "\xe2\x94\x82" S_RST))
                    goto fail;
            }
        }
        if (boxed) {
            if (!newline(out, &lines) || !buf_puts(out, S_BORDER "\xe2\x95\xb0") ||
                !buf_repeat(out, "\xe2\x94\x80", width - 2) ||
                !buf_puts(out, "\xe2\x95\xaf" S_RST))
                goto fail;
        }
    }

    // 5. Thin-Border Composer Box
    const char *bcolor = status->busy ? S_BORDER_HI : S_BORDER;
    if (boxed) {
        if (!newline(out, &lines) || !buf_puts(out, bcolor) || !buf_puts(out, "\xe2\x95\xad") ||
            !buf_repeat(out, "\xe2\x94\x80", width - 2) || !buf_puts(out, "\xe2\x95\xae" S_RST))
            goto fail;
    }

    size_t crow = row_of(v, v->cursor);
    if (v->scroll > crow)
        v->scroll = crow;
    if (crow >= v->scroll + ed)
        v->scroll = crow + 1 - ed;
    if (v->scroll + ed > v->row_count)
        v->scroll = v->row_count - ed;
    unsigned ew = editor_width(cols, boxed);
    bool empty_draft = v->draft.len == 0;

    for (size_t i = v->scroll; i < v->scroll + ed; ++i) {
        const text_row *r = &v->rows[i];
        if (!newline(out, &lines))
            goto fail;
        if (boxed && !buf_puts(out, bcolor) )
            goto fail;
        if (boxed && !buf_puts(out, "\xe2\x94\x82 " S_RST))
            goto fail;
        if (i == 0) {
            if (!buf_puts(out, S_ACCENT "\xe2\x80\xba " S_RST))
                goto fail;
        } else {
            if (!buf_puts(out, "  "))
                goto fail;
        }
        unsigned used = 0;
        if (empty_draft && i == 0) {
            const char *ph = status->busy ? "Queue steering message while running..."
                                          : "Ask ah anything, or type / for commands";
            if (!buf_puts(out, S_FAINT) || !emit_line(out, ph, strlen(ph), ew, &used) ||
                !buf_puts(out, S_RST))
                goto fail;
        } else {
            if (!buf_puts(out, S_TEXT) ||
                !emit_line(out, v->draft.data + r->start, r->end - r->start, ew, &used) ||
                !buf_puts(out, S_RST))
                goto fail;
        }
        if (boxed) {
            if (used < ew && !buf_repeat(out, " ", ew - used))
                goto fail;
            if (!buf_puts(out, " ") || !buf_puts(out, bcolor) ||
                !buf_puts(out, "\xe2\x94\x82" S_RST))
                goto fail;
        }
        if (i == crow) {
            cursor_row = lines - 1;
            unsigned prefix_w = boxed ? 4 : 2;
            unsigned col = prefix_w + column_of(v->draft.data, r->start, v->cursor);
            unsigned max_col = boxed ? width - 2 : width;
            cursor_col = col < max_col ? col : max_col;
        }
    }

    if (boxed) {
        if (!newline(out, &lines) || !append_meta_border(out, status, width, bcolor))
            goto fail;
    }

    // 6. Bottom shortcut & workspace context strip
    if (show_footer) {
        if (!newline(out, &lines))
            goto fail;
        const char *left_plain = "enter send  ·  / commands  ·  ^j newline  ·  ^c quit";
        unsigned lw = span_width(left_plain, strlen(left_plain)) + 1;
        if (!buf_puts(out, " " S_KEY "enter" S_DIM " send  \xc2\xb7  " S_KEY "/" S_DIM
                           " commands  \xc2\xb7  " S_KEY "^j" S_DIM " newline  \xc2\xb7  " S_KEY
                           "^c" S_DIM " quit" S_RST))
            goto fail;
        const char *cwd = status->cwd && *status->cwd ? status->cwd : NULL;
        const char *branch = status->branch && *status->branch ? status->branch : NULL;
        if (cwd || branch) {
            unsigned rw = (cwd ? span_width(cwd, strlen(cwd)) : 0) +
                          (cwd && branch ? 3 : 0) +
                          (branch ? span_width(branch, strlen(branch)) : 0);
            if (lw + rw + 4 <= width) {
                if (!buf_repeat(out, " ", width - lw - rw - 1))
                    goto fail;
                if (cwd && (!buf_puts(out, S_DIM) || !buf_puts(out, cwd)))
                    goto fail;
                if (cwd && branch && !buf_puts(out, S_FAINT " \xc2\xb7 "))
                    goto fail;
                if (branch && (!buf_puts(out, S_LAVENDER) || !buf_puts(out, branch)))
                    goto fail;
                if (!buf_puts(out, S_RST))
                    goto fail;
            }
        }
    }

    free(prow);
    frame->text = out->data;
    frame->lines = lines ? lines : 1;
    frame->cursor_row = cursor_row;
    frame->cursor_col = cursor_col;
    frame->hide_cursor = false;
    return true;
fail:
    free(prow);
    return false;
}

int tui_view_timeout(const tui_view_status *status, uint64_t now_ms) {
    if (!status || !status->busy)
        return -1;
    return (int)(SPIN_MS - now_ms % SPIN_MS);
}
