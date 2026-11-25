// Inline chat frame: stream preview, command palette, and composer.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SPIN_MS = 90, PALETTE_ROWS = 6, PREVIEW_ROWS = 6 };

static const char *const spinner[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f",
};
#define SPIN_FRAMES (sizeof(spinner) / sizeof(*spinner))

static void format_elapsed(char *out, size_t size, uint64_t ms) {
    uint64_t s = ms / 1000;
    if (s < 60)
        snprintf(out, size, "%us", (unsigned)s);
    else if (s < 3600)
        snprintf(out, size, "%um %02us", (unsigned)(s / 60), (unsigned)(s % 60));
    else
        snprintf(out, size, "%uh %02um", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
}

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
        meta_w += view_span_width(model, strlen(model));
        has_meta = true;
    }
    if (state) {
        meta_w += (has_meta ? 3 : 0) + view_span_width(state, strlen(state));
        has_meta = true;
    }
    if (turn_buf[0]) {
        meta_w += (has_meta ? 3 : 0) + view_span_width(turn_buf, strlen(turn_buf));
        has_meta = true;
    }
    if (status->has_pending) {
        meta_w += (has_meta ? 3 : 0) + 9; // "! pending"
        has_meta = true;
    }

    if (!view_buf_puts(out, bcolor) || !view_buf_puts(out, "\xe2\x95\xb0")) // ╰
        return false;
    if (has_meta && width >= meta_w + 6) {
        unsigned fill = width - 2 - (meta_w + 2) - 1;
        if (!view_buf_repeat(out, "\xe2\x94\x80", fill) || !view_buf_put(out, " ", 1))
            return false;
        bool sep = false;
        if (model) {
            if (!view_buf_puts(out, S_GOLD) || !view_buf_puts(out, model))
                return false;
            sep = true;
        }
        if (state) {
            if (sep && !view_buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!view_buf_puts(out, view_status_color(state, status->has_pending, status->busy)) ||
                !view_buf_puts(out, state))
                return false;
            sep = true;
        }
        if (turn_buf[0]) {
            if (sep && !view_buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!view_buf_puts(out, S_DIM) || !view_buf_puts(out, turn_buf))
                return false;
            sep = true;
        }
        if (status->has_pending) {
            if (sep && !view_buf_puts(out, S_FAINT " \xc2\xb7 "))
                return false;
            if (!view_buf_puts(out, S_AMBER "! pending"))
                return false;
        }
        if (!view_buf_puts(out, " ") || !view_buf_puts(out, bcolor) ||
            !view_buf_puts(out, "\xe2\x94\x80\xe2\x95\xaf" S_RST)) // ─╯
            return false;
    } else {
        if (!view_buf_repeat(out, "\xe2\x94\x80", width - 2) || !view_buf_puts(out, "\xe2\x95\xaf" S_RST))
            return false;
    }
    return true;
}

bool tui_view_render_chat(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                          uint64_t now_ms, tui_frame *frame) {
    unsigned width = view_frame_width(cols);
    unsigned budget = rows > 1 ? rows - 1 : 1;
    bool boxed = view_boxed_mode(cols, rows);
    if (status->busy && !v->was_busy)
        v->busy_since = now_ms;
    v->was_busy = status->busy;

    if (!view_layout_draft(v, cols, boxed))
        return false;
    view_refresh_matches(v, status);

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
    if (plen && !view_wrap(preview, plen, prev_w, &prow, &pcount, &pcap)) {
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
    if (!view_buf_reserve(out, 1024))
        goto fail;
    out->data[0] = 0;
    unsigned lines = 0, cursor_row = 0, cursor_col = 0;

    // 1. Streaming preview rail
    for (size_t i = pcount - show_prev; i < pcount; ++i) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, S_FAINT "\xe2\x94\x82 " S_TEXT) ||
            !view_emit_line(out, preview + prow[i].start, prow[i].end - prow[i].start, prev_w, NULL) ||
            !view_buf_puts(out, S_RST))
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
        if (!view_newline(out, &lines) || !view_buf_puts(out, " ") || !view_buf_puts(out, S_ACCENT) ||
            !view_buf_puts(out, spin) || !view_buf_puts(out, " " S_TEXT) || !view_buf_puts(out, verb) ||
            !view_buf_puts(out, S_DIM "  ") || !view_buf_puts(out, elapsed) || !view_buf_puts(out, S_RST))
            goto fail;
        used = 1 + 1 + 1 + (unsigned)strlen(verb) + 2 + (unsigned)strlen(elapsed);
        unsigned hlen = (unsigned)strlen(hint);
        if (used + hlen + 4 <= width) {
            if (!view_buf_repeat(out, " ", width - used - hlen - 1) ||
                !view_buf_puts(out, S_FAINT) || !view_buf_puts(out, hint) || !view_buf_puts(out, S_RST))
                goto fail;
        }
    }

    // 3. Notice strip
    if (show_notice) {
        const char *bar = status->notice_error ? S_RED : status->has_pending ? S_AMBER : S_ACCENT;
        if (!view_newline(out, &lines) || !view_buf_puts(out, " ") || !view_buf_puts(out, bar) ||
            !view_buf_puts(out, "\xe2\x96\x8e " S_TEXT) ||
            !view_emit_line(out, status->notice, strlen(status->notice),
                       width > 4 ? width - 4 : width, NULL) ||
            !view_buf_puts(out, S_RST))
            goto fail;
    }

    // 4. Floating Slash Command Palette ABOVE the composer box
    if (pal_items) {
        size_t top = v->selected >= pal_items ? v->selected + 1 - pal_items : 0;
        unsigned name_w = 0;
        for (size_t i = top; i < top + pal_items; ++i) {
            unsigned w = view_span_width(status->commands[v->matches[i]].name,
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
            if (!view_newline(out, &lines) || !view_buf_puts(out, S_BORDER "\xe2\x95\xad\xe2\x94\x80"))
                goto fail;
            if (width >= hlen + clen + 5) {
                if (!view_buf_puts(out, S_DIM) || !view_buf_puts(out, hdr) || !view_buf_puts(out, S_BORDER) ||
                    !view_buf_repeat(out, "\xe2\x94\x80", width - 3 - hlen - clen - 1) ||
                    !view_buf_puts(out, S_FAINT) || !view_buf_puts(out, count_tag) ||
                    !view_buf_puts(out, S_BORDER "\xe2\x94\x80\xe2\x95\xae" S_RST))
                    goto fail;
            } else {
                if (!view_buf_repeat(out, "\xe2\x94\x80", width - 3) ||
                    !view_buf_puts(out, "\xe2\x95\xae" S_RST))
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
            if (!view_newline(out, &lines))
                goto fail;
            if (boxed && !view_buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST))
                goto fail;
            unsigned row_used = 0;
            if (sel) {
                if (!view_buf_puts(out, S_SEL_BOLD " \xe2\x80\xba /"))
                    goto fail;
            } else {
                if (!view_buf_puts(out, S_TEXT "   /"))
                    goto fail;
            }
            row_used += 4;
            unsigned nw = 0;
            if (!view_emit_line(out, c->name, strlen(c->name),
                           inner > row_used ? inner - row_used : 0, &nw))
                goto fail;
            row_used += nw;
            if (c->description && *c->description && row_used + 2 < inner) {
                unsigned target = 4 + name_w + 3;
                if (target > inner)
                    target = row_used + 2;
                unsigned gap = target > row_used ? target - row_used : 1;
                if (!view_buf_repeat(out, " ", gap))
                    goto fail;
                row_used += gap;
                if (!view_buf_puts(out, sel ? S_SEL_DESC : S_DIM))
                    goto fail;
                unsigned dw = 0;
                if (!view_emit_line(out, c->description, strlen(c->description),
                               inner > row_used + 1 ? inner - row_used - 1 : 0, &dw))
                    goto fail;
                row_used += dw;
            }
            if (row_used < inner && !view_buf_repeat(out, " ", inner - row_used))
                goto fail;
            if (!view_buf_puts(out, S_RST))
                goto fail;
            if (boxed) {
                size_t r = i - top;
                bool on = scrollable && r >= thumb_pos && r < thumb_pos + thumb_len;
                if (!view_buf_puts(out, on ? S_ACCENT "\xe2\x94\x83" S_RST
                                      : S_BORDER "\xe2\x94\x82" S_RST))
                    goto fail;
            }
        }
        if (boxed) {
            if (!view_newline(out, &lines) || !view_buf_puts(out, S_BORDER "\xe2\x95\xb0") ||
                !view_buf_repeat(out, "\xe2\x94\x80", width - 2) ||
                !view_buf_puts(out, "\xe2\x95\xaf" S_RST))
                goto fail;
        }
    }

    // 5. Thin-Border Composer Box
    const char *bcolor = status->busy ? S_BORDER_HI : S_BORDER;
    if (boxed) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, bcolor) || !view_buf_puts(out, "\xe2\x95\xad") ||
            !view_buf_repeat(out, "\xe2\x94\x80", width - 2) || !view_buf_puts(out, "\xe2\x95\xae" S_RST))
            goto fail;
    }

    size_t crow = view_row_of(v, v->cursor);
    if (v->scroll > crow)
        v->scroll = crow;
    if (crow >= v->scroll + ed)
        v->scroll = crow + 1 - ed;
    if (v->scroll + ed > v->row_count)
        v->scroll = v->row_count - ed;
    unsigned ew = view_editor_width(cols, boxed);
    bool empty_draft = v->draft.len == 0;

    for (size_t i = v->scroll; i < v->scroll + ed; ++i) {
        const text_row *r = &v->rows[i];
        if (!view_newline(out, &lines))
            goto fail;
        if (boxed && !view_buf_puts(out, bcolor) )
            goto fail;
        if (boxed && !view_buf_puts(out, "\xe2\x94\x82 " S_RST))
            goto fail;
        if (i == 0) {
            if (!view_buf_puts(out, S_ACCENT "\xe2\x80\xba " S_RST))
                goto fail;
        } else {
            if (!view_buf_puts(out, "  "))
                goto fail;
        }
        unsigned used = 0;
        if (empty_draft && i == 0) {
            const char *ph = status->busy ? "Queue steering message while running..."
                                          : "Ask ah anything, or type / for commands";
            if (!view_buf_puts(out, S_FAINT) || !view_emit_line(out, ph, strlen(ph), ew, &used) ||
                !view_buf_puts(out, S_RST))
                goto fail;
        } else {
            if (!view_buf_puts(out, S_TEXT) ||
                !view_emit_line(out, v->draft.data + r->start, r->end - r->start, ew, &used) ||
                !view_buf_puts(out, S_RST))
                goto fail;
        }
        if (boxed) {
            if (used < ew && !view_buf_repeat(out, " ", ew - used))
                goto fail;
            if (!view_buf_puts(out, " ") || !view_buf_puts(out, bcolor) ||
                !view_buf_puts(out, "\xe2\x94\x82" S_RST))
                goto fail;
        }
        if (i == crow) {
            cursor_row = lines - 1;
            unsigned prefix_w = boxed ? 4 : 2;
            unsigned col = prefix_w + view_column_of(v->draft.data, r->start, v->cursor);
            unsigned max_col = boxed ? width - 2 : width;
            cursor_col = col < max_col ? col : max_col;
        }
    }

    if (boxed) {
        if (!view_newline(out, &lines) || !append_meta_border(out, status, width, bcolor))
            goto fail;
    }

    // 6. Bottom shortcut & workspace context strip
    if (show_footer) {
        if (!view_newline(out, &lines))
            goto fail;
        const char *left_plain = "enter send  ·  / commands  ·  ^j newline  ·  ^c quit";
        unsigned lw = view_span_width(left_plain, strlen(left_plain)) + 1;
        if (!view_buf_puts(out, " " S_KEY "enter" S_DIM " send  \xc2\xb7  " S_KEY "/" S_DIM
                           " commands  \xc2\xb7  " S_KEY "^j" S_DIM " newline  \xc2\xb7  " S_KEY
                           "^c" S_DIM " quit" S_RST))
            goto fail;
        const char *cwd = status->cwd && *status->cwd ? status->cwd : NULL;
        const char *branch = status->branch && *status->branch ? status->branch : NULL;
        if (cwd || branch) {
            unsigned rw = (cwd ? view_span_width(cwd, strlen(cwd)) : 0) +
                          (cwd && branch ? 3 : 0) +
                          (branch ? view_span_width(branch, strlen(branch)) : 0);
            if (lw + rw + 4 <= width) {
                if (!view_buf_repeat(out, " ", width - lw - rw - 1))
                    goto fail;
                if (cwd && (!view_buf_puts(out, S_DIM) || !view_buf_puts(out, cwd)))
                    goto fail;
                if (cwd && branch && !view_buf_puts(out, S_FAINT " \xc2\xb7 "))
                    goto fail;
                if (branch && (!view_buf_puts(out, S_LAVENDER) || !view_buf_puts(out, branch)))
                    goto fail;
                if (!view_buf_puts(out, S_RST))
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
