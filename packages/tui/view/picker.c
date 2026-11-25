// Command Center and question pickers: navigation, filtering, and rendering.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FILTER_COUNT = 5 };

static bool item_is_ready(const char *st) {
    return st && (!strcmp(st, "ready") || !strcmp(st, "completed") || !strcmp(st, "idle"));
}

static bool item_matches_filter(const tui_picker_item *it, unsigned filter) {
    switch (filter) {
    case 0: return true; // All
    case 1: return it->attention;
    case 2: return it->working;
    case 3: return !it->attention && !it->working && item_is_ready(it->status);
    default: return !it->attention && !it->working && !item_is_ready(it->status);
    }
}

static const char *status_label(const char *st) {
    if (!st || !*st)
        return "";
    if (!strcmp(st, "outcome_unknown"))
        return "unconfirmed";
    if (!strcmp(st, "waiting_approval"))
        return "approval";
    if (!strcmp(st, "waiting_input"))
        return "question";
    if (!strcmp(st, "completed") || !strcmp(st, "idle"))
        return "ready";
    return st;
}

static const char *status_mark(const char *state, bool attention, bool working) {
    if (attention)
        return "!";
    if (working || !strcmp(state, "running"))
        return "\xe2\x97\x8c"; /* ◌ */
    return item_is_ready(state) ? "\xe2\x80\xa2" : "\xc2\xb7";
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

void tui_view_select_picker(tui_view *v, size_t row) {
    v->pick_filter = 0;
    v->pick_selected = row;
    v->pick_scroll = 0;
    v->was_picker = true;
}

tui_action tui_view_picker_key(tui_view *v, const tui_picker *p, tui_key key, const char *text,
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
        if (p->question)
            break;
        v->pick_filter = (v->pick_filter + 1) % FILTER_COUNT;
        v->pick_selected = v->pick_scroll = 0;
        break;
    case TUI_SHIFT_TAB:
        if (p->question)
            break;
        v->pick_filter = (v->pick_filter + FILTER_COUNT - 1) % FILTER_COUNT;
        v->pick_selected = v->pick_scroll = 0;
        break;
    case TUI_ENTER:
        if (p->can_open && cur && cur->id)
            return view_action(v, TUI_PICK_OPEN, cur->id, strlen(cur->id));
        break;
    case TUI_TEXT:
        if (text && count == 1) {
            char c = view_lower(text[0]);
            if (c == 'q')
                return (tui_action){TUI_PICK_CLOSE, NULL};
            if (c == 'j' && v->pick_count)
                v->pick_selected = (v->pick_selected + 1) % v->pick_count;
            else if (c == 'k' && v->pick_count)
                v->pick_selected = (v->pick_selected + v->pick_count - 1) % v->pick_count;
            else if (c == 'a' && p->can_approve && cur && cur->id)
                return view_action(v, text[0] == 'A' ? TUI_PICK_APPROVE_ALL : TUI_PICK_APPROVE,
                                   cur->id, strlen(cur->id));
            else if (c == 'd' && p->can_approve && cur && cur->id)
                return view_action(v, TUI_PICK_DENY, cur->id, strlen(cur->id));
            else if (c == 'n' && p->can_new)
                return (tui_action){TUI_PICK_NEW, NULL};
        }
        break;
    default:
        break;
    }
    return none;
}

/* ---------- Command Center rendering ---------- */

static bool render_ask_picker(tui_view *v, const tui_picker *p, unsigned cols, unsigned rows,
                              tui_frame *frame) {
    unsigned width = view_frame_width(cols), budget = rows > 2 ? rows - 2 : 1;
    unsigned inner = width > 4 ? width - 4 : 1;
    text_buf *out = &v->out;
    out->len = 0;
    if (!view_buf_reserve(out, 256))
        return false;
    out->data[0] = 0;
    unsigned lines = 0;
    if (budget > 1 && (!view_newline(out, &lines) ||
        !view_buf_puts(out, S_ACCENT S_BOLD) ||
        !view_emit_line(out, p->title, strlen(p->title), width, NULL) || !view_buf_puts(out, S_RST)))
        return false;

    text_row *rows_data = NULL;
    size_t count = 0, capacity = 0;
    bool ok = view_wrap(p->question, strlen(p->question), inner, &rows_data, &count, &capacity);
    unsigned question_rows = budget > 4 ? (budget - 3) / 2 : 0;
    for (size_t i = 0; ok && i < count && i < question_rows; i++)
        ok = view_newline(out, &lines) && view_buf_puts(out, "  " S_TEXT) &&
             view_emit_line(out, p->question + rows_data[i].start, rows_data[i].end - rows_data[i].start,
                       inner, NULL) && view_buf_puts(out, S_RST);
    if (!ok)
        goto done;
    if (!v->pick_count)
        goto frame;

    const char *selected = p->items[v->pick_idx[v->pick_selected]].title;
    count = 0;
    if (selected)
        ok = view_wrap(selected, strlen(selected), inner, &rows_data, &count, &capacity);
    if (!ok)
        goto done;
    unsigned available = budget - lines;
    bool footer = available > 1;
    if (footer)
        --available;
    size_t selected_rows = count ? (count < available ? count : available) : 1;
    size_t visible = available - selected_rows + 1;
    if (v->pick_scroll > v->pick_selected)
        v->pick_scroll = v->pick_selected;
    if (v->pick_selected >= v->pick_scroll + visible)
        v->pick_scroll = v->pick_selected + 1 - visible;
    if (v->pick_count - v->pick_scroll < visible)
        v->pick_scroll = v->pick_count > visible ? v->pick_count - visible : 0;
    for (size_t i = v->pick_scroll; ok && i < v->pick_count && i - v->pick_scroll < visible; i++) {
        const char *label = p->items[v->pick_idx[i]].title;
        bool active = i == v->pick_selected;
        size_t steps = active ? selected_rows : 1;
        for (size_t j = 0; ok && j < steps; j++) {
            size_t start = active && count ? rows_data[j].start : 0;
            size_t length = active && count ? rows_data[j].end - start : label ? strlen(label) : 0;
            ok = view_newline(out, &lines) && view_buf_puts(out, active && !j ? S_ACCENT "  › " : "    ") &&
                 (!length || view_emit_line(out, label + start, length, inner, NULL)) && view_buf_puts(out, S_RST);
        }
    }
    if (ok && footer && lines < budget) {
        static const char help[] = "↑↓ choose   Enter reply   Esc type";
        ok = view_newline(out, &lines) && view_buf_puts(out, "  " S_DIM) &&
             view_emit_line(out, help, sizeof(help) - 1, inner, NULL) && view_buf_puts(out, S_RST);
    }
frame:
    if (ok)
        *frame = (tui_frame){.text = out->data, .lines = lines ? lines : 1,
                            .cursor_row = lines ? lines - 1 : 0, .hide_cursor = true};
done:
    free(rows_data);
    return ok;
}

bool tui_view_render_picker(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                            tui_frame *frame) {
    const tui_picker *p = status->picker;
    if (!v->was_picker)
        tui_view_select_picker(v, 0);
    refresh_picker(v, p);
    if (p->question)
        return render_ask_picker(v, p, cols, rows, frame);

    unsigned width = view_frame_width(cols);
    unsigned budget = rows > 2 ? rows - 2 : 1;
    bool two_pane = width >= 72;
    unsigned left_w = two_pane ? (width * 60) / 100 : width;
    unsigned right_w = two_pane ? width - left_w - 1 : 0;

    text_buf *out = &v->out;
    out->len = 0;
    if (!view_buf_reserve(out, 2048))
        return false;
    out->data[0] = 0;
    unsigned lines = 0;

    // 1. Header row: Title + subtitle on left, "tab / shift+tab  filter" on right
    if (!view_newline(out, &lines))
        return false;
    const char *title = p->title && *p->title ? p->title : "Command center";
    const char *sub = p->subtitle && *p->subtitle ? p->subtitle : "";
    const char *rhint = "tab / shift+tab  filter";
    unsigned rlen = view_span_width(rhint, strlen(rhint));
    unsigned used = 0;
    if (!view_buf_puts(out, "  " S_BOLD S_TEXT))
        return false;
    used += 2;
    unsigned tw = 0;
    if (!view_emit_line(out, title, strlen(title), width > used ? width - used : 0, &tw))
        return false;
    used += tw;
    if (*sub && used + 4 < width) {
        if (!view_buf_puts(out, S_RST S_DIM "   "))
            return false;
        used += 3;
        unsigned sw = 0;
        if (!view_emit_line(out, sub, strlen(sub), width > used ? width - used : 0, &sw))
            return false;
        used += sw;
    }
    if (!view_buf_puts(out, S_RST))
        return false;
    if (used + rlen + 4 <= width) {
        if (!view_buf_repeat(out, " ", width - used - rlen - 2) ||
            !view_buf_puts(out, S_KEY "tab" S_DIM "/" S_KEY "shift+tab" S_DIM "  filter" S_RST))
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
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  "))
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
                if (!view_buf_puts(out, S_PILL_ON) || !view_buf_put(out, pill, (size_t)plen) ||
                    !view_buf_puts(out, S_RST " "))
                    return false;
            } else {
                const char *clr = f == 1 && counts[1] ? S_AMBER
                                  : f == 2 && counts[2] ? S_ACCENT
                                                        : S_DIM;
                if (!view_buf_puts(out, clr) || !view_buf_put(out, pill, (size_t)plen) ||
                    !view_buf_puts(out, S_RST " "))
                    return false;
            }
            col += pw + 1;
        }
    }

    // 3. Top hairline
    if (lines < budget) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, S_BORDER))
            return false;
        if (two_pane) {
            if (!view_buf_repeat(out, "\xe2\x94\x80", left_w) || !view_buf_puts(out, "\xe2\x94\xac") ||
                !view_buf_repeat(out, "\xe2\x94\x80", right_w))
                return false;
        } else if (!view_buf_repeat(out, "\xe2\x94\x80", width)) {
            return false;
        }
        if (!view_buf_puts(out, S_RST))
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
        if (!view_wrap(detail, strlen(detail), dwidth, &drows, &dcount, &dcap)) {
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
        if (!view_newline(out, &lines))
            goto fail;
        unsigned status_w = left_w > 40 ? 11 : 0;
        unsigned meta_w = left_w > 54 ? 14 : 0;
        unsigned title_w = left_w > 4 + status_w + meta_w ? left_w - 4 - status_w - meta_w : 4;
        if (!view_buf_puts(out, "  " S_DIM) || !view_emit_padded(out, "Item", 4, title_w))
            goto fail;
        if (status_w && !view_emit_padded(out, "Status", 6, status_w))
            goto fail;
        if (meta_w && !view_emit_padded(out, "Activity", 8, meta_w))
            goto fail;
        if (!view_buf_puts(out, "  " S_RST))
            goto fail;
        if (two_pane) {
            if (!view_buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  " S_BOLD S_TEXT) ||
                !view_emit_padded(out, "Details", 7, right_w > 2 ? right_w - 2 : 0) ||
                !view_buf_puts(out, S_RST))
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
        if (!view_newline(out, &lines))
            goto fail;
        size_t idx = v->pick_scroll + r;
        unsigned inner_l = left_w > 1 ? left_w - 1 : 1; // 1 col reserved for scrollbar/gap
        if (v->pick_count == 0) {
            if (r == 1) {
                if (!view_buf_puts(out, S_DIM "  ") ||
                    !view_emit_padded(out, "No matching items", 17, inner_l > 2 ? inner_l - 2 : 0) ||
                    !view_buf_puts(out, S_RST " "))
                    goto fail;
            } else if (!view_buf_repeat(out, " ", left_w)) {
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
            const char *label = status_label(st);
            const char *meta = it->meta ? it->meta : "";
            const char *ititle = it->title ? it->title : (it->id ? it->id : "");
            if (is_sel) {
                if (!view_buf_puts(out, S_SEL_CHEV " \xe2\x80\xba " S_SEL_BOLD)) // ›
                    goto fail;
                for (unsigned k = 0; k < (it->indent < 6 ? it->indent : 6); ++k)
                    if (!view_buf_puts(out, "  "))
                        goto fail;
                if (!view_buf_puts(out, status_mark(st, it->attention, it->working)) ||
                    !view_buf_puts(out, " ") ||
                    !view_emit_padded(out, ititle, strlen(ititle), title_w))
                    goto fail;
                if (status_w) {
                    if (!view_buf_puts(out, S_SEL_DESC) ||
                        !view_emit_padded(out, label, strlen(label), status_w > 2 ? status_w - 2 : status_w) ||
                        (status_w > 2 && !view_buf_put(out, "  ", 2)))
                        goto fail;
                }
                if (meta_w) {
                    if (!view_buf_puts(out, S_SEL_DESC) ||
                        !view_emit_padded(out, meta, strlen(meta), meta_w))
                        goto fail;
                }
                if (!view_buf_puts(out, S_RST))
                    goto fail;
            } else {
                if (!view_buf_puts(out, it->active ? S_ACCENT " \xe2\x80\xa2 " : "   "))
                    goto fail;
                for (unsigned k = 0; k < (it->indent < 6 ? it->indent : 6); ++k)
                    if (!view_buf_puts(out, "  "))
                        goto fail;
                if (!view_buf_puts(out, view_status_color(st, it->attention, it->working)) ||
                    !view_buf_puts(out, status_mark(st, it->attention, it->working)) ||
                    !view_buf_puts(out, " ") ||
                    !view_buf_puts(out, it->active ? S_BOLD S_TEXT : S_TEXT) ||
                    !view_emit_padded(out, ititle, strlen(ititle), title_w) ||
                    !view_buf_puts(out, S_RST))
                    goto fail;
                if (status_w) {
                    if (!view_buf_puts(out, view_status_color(st, it->attention, it->working)) ||
                        !view_emit_padded(out, label, strlen(label), status_w > 2 ? status_w - 2 : status_w) ||
                        (status_w > 2 && !view_buf_put(out, "  ", 2)) || !view_buf_puts(out, S_RST))
                        goto fail;
                }
                if (meta_w) {
                    if (!view_buf_puts(out, S_DIM) || !view_emit_padded(out, meta, strlen(meta), meta_w) ||
                        !view_buf_puts(out, S_RST))
                        goto fail;
                }
            }
            // Right-edge scrollbar
            if (scrollable) {
                bool on = r >= thumb_pos && r < thumb_pos + thumb_len;
                if (!view_buf_puts(out, on ? S_DIM "\xe2\x94\x83" S_RST : S_FAINT "\xe2\x94\x82" S_RST))
                    goto fail;
            } else if (!view_buf_put(out, " ", 1)) {
                goto fail;
            }
        } else if (!view_buf_repeat(out, " ", left_w)) {
            goto fail;
        }

        // Right pane detail row
        if (two_pane) {
            if (!view_buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  "))
                goto fail;
            if (sel && r < dcount) {
                const text_row *dr = &drows[r];
                if (!view_render_detail_line(out, detail + dr->start, dr->end - dr->start, dwidth))
                    goto fail;
            } else if (sel && r == 0 && !dcount) {
                if (!view_buf_puts(out, S_DIM) || !view_emit_padded(out, "No details", 10, dwidth) ||
                    !view_buf_puts(out, S_RST))
                    goto fail;
            }
        }
    }

    // 6. Bottom hairline
    if (lines < budget) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, S_BORDER))
            goto fail;
        if (two_pane) {
            if (!view_buf_repeat(out, "\xe2\x94\x80", left_w) || !view_buf_puts(out, "\xe2\x94\xb4") ||
                !view_buf_repeat(out, "\xe2\x94\x80", right_w))
                goto fail;
        } else if (!view_buf_repeat(out, "\xe2\x94\x80", width)) {
            goto fail;
        }
        if (!view_buf_puts(out, S_RST))
            goto fail;
    }

    // 7. Footer shortcut bar
    if (lines < budget) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  " S_KEY "\xe2\x86\x91/\xe2\x86\x93" S_DIM
                                                    " move   " S_KEY "tab" S_DIM
                                                    " filter"))
            goto fail;
        if (p->can_open && !view_buf_puts(out, "   " S_KEY "enter" S_DIM " open"))
            goto fail;
        if (p->can_approve &&
            !view_buf_puts(out, "   " S_KEY "a" S_DIM " once   " S_KEY "A" S_DIM " all   " S_RED "d" S_DIM
                           " deny"))
            goto fail;
        if (p->can_new && !view_buf_puts(out, "   " S_KEY "n" S_DIM " new"))
            goto fail;
        if (!view_buf_puts(out, "   " S_KEY "esc" S_DIM " back" S_RST))
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
