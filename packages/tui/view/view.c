// View lifecycle, mode dispatch, and chat editor input.
#include "internal.h"
#include <stdlib.h>
#include <string.h>

enum { HISTORY_MAX = 200 };

size_t view_row_of(const tui_view *v, size_t pos) {
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
        size_t next = view_next_cluster(v->draft.data, r->end, pos, &w, &cp);
        if (col + w > goal || (r->soft && next >= r->end))
            break;
        col += w;
        pos = next;
    }
    return pos;
}

bool view_boxed_mode(unsigned cols, unsigned rows) {
    return view_frame_width(cols) >= 16 && rows >= 5;
}

unsigned view_editor_width(unsigned cols, bool boxed) {
    unsigned w = view_frame_width(cols);
    if (boxed)
        return w > 6 ? w - 6 : 2;
    return w > 4 ? w - 2 : 2;
}

bool view_layout_draft(tui_view *v, unsigned cols, bool boxed) {
    if (!view_buf_reserve(&v->draft, 0))
        return false;
    v->draft.data[v->draft.len] = 0;
    return view_wrap(v->draft.data, v->draft.len, view_editor_width(cols, boxed), &v->rows, &v->row_count,
                &v->row_cap);
}

/* ---------- palette ---------- */

char view_lower(char c) {
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
    while (i < qn && name[i] && view_lower(name[i]) == view_lower(q[i]))
        ++i;
    if (i == qn)
        return name[i] ? 2 : 3;
    const char *p = name;
    for (i = 0; i < qn; ++i) {
        while (*p && view_lower(*p) != view_lower(q[i]))
            ++p;
        if (!*p)
            return 0;
        ++p;
    }
    return 1;
}

void view_refresh_matches(tui_view *v, const tui_view_status *status) {
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
    bool ok = view_buf_reserve(&clean, n);
    for (size_t i = 0; ok && i < n;) {
        uint32_t cp;
        size_t step = view_decode(s + i, n - i, &cp);
        if (cp == '\r') {
            ok = view_buf_put(&clean, "\n", 1);
            if (i + 1 < n && s[i + 1] == '\n')
                ++step;
        } else if (cp == '\n' || cp == '\t') {
            ok = view_buf_put(&clean, s + i, 1);
        } else if (cp == 0xfffd && step == 1) {
            ok = view_buf_put(&clean, "\xef\xbf\xbd", 3);
        } else if (!view_is_control(cp)) {
            ok = view_buf_put(&clean, s + i, step);
        }
        i += step;
    }
    if (ok && clean.len) {
        ok = view_buf_insert(&v->draft, v->cursor, clean.data, clean.len);
        if (ok)
            v->cursor += clean.len;
    }
    free(clean.data);
    edited(v);
    return ok;
}

static void set_draft(tui_view *v, const char *s, size_t n) {
    if (view_buf_set(&v->draft, s, n))
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
            if (!view_buf_set(&v->stash, v->draft.data ? v->draft.data : "", v->draft.len))
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
    if (!view_layout_draft(v, cols, view_boxed_mode(cols, 24)))
        return;
    size_t row = view_row_of(v, v->cursor);
    if (up ? row == 0 : row + 1 >= v->row_count) {
        history_move(v, up);
        return;
    }
    if (v->goal < 0)
        v->goal = (int)view_column_of(v->draft.data, v->rows[row].start, v->cursor);
    v->cursor = offset_at(v, up ? row - 1 : row + 1, (unsigned)v->goal);
}

tui_action view_action(tui_view *v, tui_action_kind kind, const char *s, size_t n) {
    if (!s)
        return (tui_action){kind, NULL};
    if (!view_buf_set(&v->action, s, n))
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
    if (!n || is_blank(s, n)) {
        if (status && status->image_name && *status->image_name && !status->busy && !status->has_pending)
            return view_action(v, TUI_SUBMIT, "", 0);
        return (tui_action){TUI_NO_ACTION, NULL};
    }
    if (v->cursor == n && s[n - 1] == '\\') {
        v->draft.data[n - 1] = '\n';
        edited(v);
        return (tui_action){TUI_NO_ACTION, NULL};
    }
    if (palette_open(v) && v->match_count && status && status->commands) {
        const char *name = status->commands[v->matches[v->selected]].name;
        return view_action(v, TUI_COMMAND, name, strlen(name));
    }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t'))
        --n;
    if (s[0] == '/' && n > 1 && s[1] == '/')
        return view_action(v, TUI_SUBMIT, s + 1, n - 1);
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
            return view_action(v, TUI_COMMAND, s + 1, n - 1);
    }
    return view_action(v, TUI_SUBMIT, s, n);
}

/* ---------- mode dispatch and lifecycle ---------- */

static bool auth_active(const tui_view_status *status) {
    return status && status->auth && status->auth->open;
}

static bool picker_active(const tui_view_status *status) {
    return status && status->picker && status->picker->open;
}

tui_view *tui_view_create(void) {
    tui_view *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->goal = -1;
    v->cols = 80;
    if (!view_buf_reserve(&v->draft, 0) || !view_buf_reserve(&v->out, 0)) {
        tui_view_destroy(v);
        return NULL;
    }
    return v;
}

void tui_view_destroy(tui_view *v) {
    if (!v)
        return;
    tui_view_auth_dispose(v);
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
        return tui_view_auth_key(v, status->auth, key, text, count);
    v->was_auth = false;
    if (picker_active(status)) {
        if (!v->was_picker)
            tui_view_select_picker(v, 0);
        return tui_view_picker_key(v, status->picker, key, text, count);
    }
    v->was_picker = false;

    view_refresh_matches(v, status);
    bool palette = palette_open(v) && v->match_count;
    char *s = v->draft.data;
    size_t n = v->draft.len;
    switch (key) {
    case TUI_TEXT:
        if (!n && text && count == 1 && status && status->pending && status->pending->active &&
            status->pending->kind && !strcmp(status->pending->kind, "approval")) {
            char c = view_lower(text[0]);
            if (c == 'y' && status->pending->operation)
                return view_action(v, TUI_PICK_APPROVE, status->pending->operation,
                            strlen(status->pending->operation));
            if (c == 'a' && status->pending->operation)
                return view_action(v, TUI_PICK_APPROVE_ALL, status->pending->operation,
                            strlen(status->pending->operation));
            if (c == 'n' && status->pending->operation)
                return view_action(v, TUI_PICK_DENY, status->pending->operation,
                            strlen(status->pending->operation));
            if (c == 'i')
                return view_action(v, TUI_COMMAND, "pending", 7);
        }
        if (text && count)
            (void)insert_text(v, text, count);
        break;
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
            if (view_buf_put(&full, "/", 1) && view_buf_puts(&full, name))
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
        v->cursor = view_prev_cluster(s, n, v->cursor);
        v->goal = -1;
        break;
    case TUI_RIGHT:
        if (v->cursor < n) {
            unsigned w;
            uint32_t cp;
            v->cursor = view_next_cluster(s, n, v->cursor, &w, &cp);
        }
        v->goal = -1;
        break;
    case TUI_HOME:
        v->cursor = view_line_start(s, v->cursor);
        v->goal = -1;
        break;
    case TUI_END:
        while (v->cursor < n && s[v->cursor] != '\n')
            ++v->cursor;
        v->goal = -1;
        break;
    case TUI_BACKSPACE:
        if (v->cursor) {
            size_t at = view_prev_cluster(s, n, v->cursor);
            view_buf_erase(&v->draft, at, v->cursor - at);
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
            size_t end = view_next_cluster(s, n, v->cursor, &w, &cp);
            view_buf_erase(&v->draft, v->cursor, end - v->cursor);
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

bool tui_view_frame(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                    uint64_t now_ms, tui_frame *frame) {
    if (!v || !frame)
        return false;
    tui_view_status none = {0};
    if (!status)
        status = &none;
    v->cols = cols;

    if (auth_active(status))
        return tui_view_render_auth(v, status, cols, rows, frame);
    v->was_auth = false;

    if (picker_active(status))
        return tui_view_render_picker(v, status, cols, rows, frame);
    v->was_picker = false;

    return tui_view_render_chat(v, status, cols, rows, now_ms, frame);
}
