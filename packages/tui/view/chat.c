// Inline chat frame: stream preview, palette, pending card, and composer.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BUSY_PULSE_MS = 250, PALETTE_ROWS = 6, PREVIEW_ROWS = 5 };

static bool state_is_quiet(const char *s) {
    return !s || !*s || !strcmp(s, "ready") || !strcmp(s, "completed") || !strcmp(s, "running") ||
           !strcmp(s, "idle");
}

bool tui_view_render_chat(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                          uint64_t now_ms, tui_frame *frame) {
    unsigned width = view_frame_width(cols);
    unsigned budget = rows > 1 ? rows - 1 : 1;
    bool boxed = view_boxed_mode(cols, rows);
    if (!view_layout_draft(v, cols, boxed))
        return false;
    view_refresh_matches(v, status);

    size_t pal_items = v->match_count < PALETTE_ROWS ? v->match_count : PALETTE_ROWS;
    // Seamless unified container: palette shares its divider with the composer top border
    size_t pal_rows = pal_items ? (boxed ? pal_items + 1 : pal_items) : 0;
    bool has_pending_card = !pal_items && boxed && status->pending && status->pending->active;
    text_row *pend_rows = NULL;
    size_t pend_count = 0, pend_cap = 0;
    const char *pend_text = has_pending_card && status->pending->summary ? status->pending->summary : "";
    unsigned pend_w = width > 6 ? width - 6 : 2;
    if (has_pending_card && *pend_text) {
        if (!view_wrap(pend_text, strlen(pend_text), pend_w, &pend_rows, &pend_count, &pend_cap)) {
            free(pend_rows);
            return false;
        }
        if (pend_count > 3)
            pend_count = 3;
    }
    size_t pend_card_rows = has_pending_card ? (pend_count ? pend_count + 1 : 2) : 0;
    bool has_notice = status->notice && *status->notice;
    bool has_image = status->image_name && *status->image_name;
    size_t busy_rows = status->busy ? 1 : 0;
    size_t notice_rows = has_notice ? 1 : 0;
    size_t image_rows = has_image ? 1 : 0;
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
        free(pend_rows);
        free(prow);
        return false;
    }
    while (pcount && prow[pcount - 1].start == prow[pcount - 1].end)
        --pcount;
    preview_need = pcount < PREVIEW_ROWS ? pcount : PREVIEW_ROWS;

    // Row budget allocation: composer box first, then palette/pending card, status, footer, preview
    size_t ed = ed_need;
    size_t max_ed = budget > box_chrome ? budget - box_chrome : 1;
    if (ed > max_ed)
        ed = max_ed;
    if (ed + box_chrome + pal_rows + pend_card_rows + busy_rows + notice_rows + image_rows + footer_rows >
        budget) {
        size_t keep = box_chrome + pal_rows + pend_card_rows + busy_rows + notice_rows + image_rows + footer_rows;
        ed = budget > keep ? budget - keep : 1;
        if (ed > ed_need)
            ed = ed_need;
        if (ed + box_chrome > budget)
            boxed = false, box_chrome = 0, footer_rows = 0, pal_rows = pal_items, pend_card_rows = 0;
    }
    size_t left = budget > ed + box_chrome ? budget - ed - box_chrome : 0;
    if (pal_rows > left) {
        if (boxed && left >= 2) {
            pal_items = left - 1;
            pal_rows = left;
        } else if (!boxed && left >= 1) {
            pal_items = left;
            pal_rows = left;
        } else {
            pal_items = pal_rows = 0;
        }
    }
    left -= pal_rows;
    size_t show_pend = pend_card_rows <= left ? pend_card_rows : 0;
    left -= show_pend;
    size_t show_busy = busy_rows <= left ? busy_rows : 0;
    left -= show_busy;
    size_t show_notice = notice_rows <= left ? notice_rows : 0;
    left -= show_notice;
    size_t show_image = image_rows <= left ? image_rows : 0;
    left -= show_image;
    size_t show_footer = footer_rows <= left ? footer_rows : 0;
    left -= show_footer;
    size_t show_gap = (boxed && left > 0) ? 1 : 0;
    left -= show_gap;
    size_t show_prev = preview_need < left ? preview_need : left;

    text_buf *out = &v->out;
    out->len = 0;
    if (!view_buf_reserve(out, 1024))
        goto fail;
    out->data[0] = 0;
    unsigned lines = 0, cursor_row = 0, cursor_col = 0;

    // 1. Live streaming preview (aligned at column 2 to match committed assistant prose)
    for (size_t i = pcount - show_prev; i < pcount; ++i) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  " S_TEXT) ||
            !view_emit_line(out, preview + prow[i].start, prow[i].end - prow[i].start, prev_w, NULL) ||
            !view_buf_puts(out, S_RST))
            goto fail;
    }

    // 2. Busy strip: one pulsing glyph, stable label, and cancel shortcut.
    if (show_busy) {
        const char *hint = "esc to cancel";
        unsigned hlen = (unsigned)strlen(hint);
        const char *pulse = now_ms / BUSY_PULSE_MS % 4 < 2 ? S_ACCENT : S_DIM;
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  ") ||
            !view_buf_puts(out, pulse) || !view_buf_puts(out, "\xe2\x97\x8c " S_RST S_TEXT))
            goto fail;
        unsigned used = 4;
        if (status->activity && *status->activity) {
            const char *act = status->activity;
            size_t vlen = 0;
            while (act[vlen] && act[vlen] != ' ')
                ++vlen;
            if (!view_buf_put(out, act, vlen) || !view_buf_puts(out, S_RST))
                goto fail;
            used += (unsigned)vlen;
            const char *rest = act + vlen;
            if (*rest) {
                char extra[24] = "";
                if (status->active_tools > 1)
                    snprintf(extra, sizeof(extra), " (+%u)", status->active_tools - 1);
                unsigned max_rest = width > used + hlen + 8 ? width - used - hlen - 8
                                    : width > used + 2      ? width - used - 2
                                                            : 4;
                if (!view_buf_puts(out, S_TEXT))
                    goto fail;
                unsigned ru = 0;
                if (!view_emit_line(out, rest, strlen(rest), max_rest, &ru) ||
                    !view_buf_puts(out, S_RST))
                    goto fail;
                used += ru;
                if (extra[0] && used + strlen(extra) + 3 <= width) {
                    if (!view_buf_puts(out, S_DIM) || !view_buf_puts(out, extra) ||
                        !view_buf_puts(out, S_RST))
                        goto fail;
                    used += (unsigned)strlen(extra);
                }
            }
        } else {
            const char *verb = plen ? "Streaming" : "Thinking";
            size_t vlen = strlen(verb);
            if (!view_buf_put(out, verb, vlen) || !view_buf_puts(out, S_RST))
                goto fail;
            used += (unsigned)vlen;
        }
        if (used + hlen + 4 <= width) {
            if (!view_buf_repeat(out, " ", width - used - hlen - 1) ||
                !view_buf_puts(out, S_FAINT) || !view_buf_puts(out, hint) || !view_buf_puts(out, S_RST))
                goto fail;
        }
    }

    // 3. Notice strip
    if (show_notice) {
        const char *bar = status->notice_error ? S_RED : status->has_pending ? S_AMBER : S_ACCENT;
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  ") || !view_buf_puts(out, bar) ||
            !view_buf_puts(out, "\xe2\x96\x8e " S_RST S_TEXT) ||
            !view_emit_line(out, status->notice, strlen(status->notice),
                       width > 5 ? width - 5 : width, NULL) ||
            !view_buf_puts(out, S_RST))
            goto fail;
    }
    if (show_image) {
        const char *label = "  Image  ";
        unsigned lead = (unsigned)strlen(label);
        if (!view_newline(out, &lines) || !view_buf_puts(out, S_DIM) ||
            !view_emit_line(out, label, lead, width, NULL) || !view_buf_puts(out, S_TEXT) ||
            (width > lead && !view_emit_line(out, status->image_name, strlen(status->image_name),
                                             width - lead, NULL)) || !view_buf_puts(out, S_RST))
            goto fail;
    }

    // 3a. Breathing-room blank line separating conversation/live activity from composer box
    if (show_gap && !view_newline(out, &lines))
        goto fail;

    // 3b. Inline Pending Approval / Question Card above the composer
    if (show_pend) {
        const tui_pending_card *pc = status->pending;
        bool is_appr = pc->kind && !strcmp(pc->kind, "approval");
        bool is_inp = pc->kind && !strcmp(pc->kind, "input");
        const char *pcol = is_inp ? S_ACCENT : S_AMBER;
        const char *hdr = is_appr ? " ! Approval required "
                          : is_inp ? " ? Agent question "
                                   : " ! Unknown tool outcome ";
        unsigned hw = view_span_width(hdr, strlen(hdr));
        const char *tname = pc->tool && *pc->tool ? pc->tool : "";
        unsigned tw = *tname ? view_span_width(tname, strlen(tname)) + 2 : 0;
        if (!view_newline(out, &lines) || !view_buf_puts(out, pcol) || !view_buf_puts(out, "\xe2\x95\xad\xe2\x94\x80"))
            goto fail;
        if (width >= hw + tw + 5) {
            if (!view_buf_puts(out, S_BOLD) || !view_buf_puts(out, hdr) || !view_buf_puts(out, S_RST) ||
                !view_buf_puts(out, pcol) ||
                !view_buf_repeat(out, "\xe2\x94\x80", width - 3 - hw - tw - 1))
                goto fail;
            if (*tname) {
                if (!view_buf_puts(out, S_GOLD " ") || !view_buf_puts(out, tname) || !view_buf_puts(out, " ") ||
                    !view_buf_puts(out, pcol))
                    goto fail;
            }
            if (!view_buf_puts(out, "\xe2\x94\x80\xe2\x95\xae" S_RST))
                goto fail;
        } else {
            if (!view_buf_repeat(out, "\xe2\x94\x80", width - 3) || !view_buf_puts(out, "\xe2\x95\xae" S_RST))
                goto fail;
        }
        size_t body_lines = pend_count ? pend_count : 1;
        for (size_t i = 0; i < body_lines; ++i) {
            if (!view_newline(out, &lines) || !view_buf_puts(out, pcol) || !view_buf_puts(out, "\xe2\x94\x82  " S_RST))
                goto fail;
            if (pend_count) {
                const text_row *pr = &pend_rows[i];
                if (!view_render_detail_line(out, pend_text + pr->start, pr->end - pr->start, pend_w))
                    goto fail;
            } else {
                const char *fallback = is_appr ? "Review tool call before executing"
                                               : "Waiting for user response";
                if (!view_buf_puts(out, S_DIM) || !view_emit_padded(out, fallback, strlen(fallback), pend_w) ||
                    !view_buf_puts(out, S_RST))
                    goto fail;
            }
            if (!view_buf_puts(out, "  ") || !view_buf_puts(out, pcol) || !view_buf_puts(out, "\xe2\x94\x82" S_RST))
                goto fail;
        }
        if (!view_newline(out, &lines) || !view_buf_puts(out, pcol) || !view_buf_puts(out, "\xe2\x94\x9c\xe2\x94\x80"))
            goto fail;
        if (is_appr && width >= 58) {
            const char *act_plain = " [y] Approve   [a] Always   [n] Deny   [i] Inspect ";
            unsigned aw = (unsigned)strlen(act_plain);
            if (!view_buf_puts(out, " " S_BOLD S_TEXT "[y]" S_RST S_TEXT " Approve   " S_ACCENT "[a]"
                               S_RST S_TEXT " Always   " S_KEY "[n]" S_DIM " Deny   " S_KEY "[i]"
                               S_DIM " Inspect ") ||
                !view_buf_puts(out, pcol) || !view_buf_repeat(out, "\xe2\x94\x80", width - 3 - aw) ||
                !view_buf_puts(out, "\xe2\x94\xa4" S_RST))
                goto fail;
        } else if (is_appr && width >= 42) {
            const char *act_plain = " [y] Approve   [a] Always   [n] Deny ";
            unsigned aw = (unsigned)strlen(act_plain);
            if (!view_buf_puts(out, " " S_BOLD S_TEXT "[y]" S_RST S_TEXT " Approve   " S_ACCENT "[a]"
                               S_RST S_TEXT " Always   " S_KEY "[n]" S_DIM " Deny ") ||
                !view_buf_puts(out, pcol) || !view_buf_repeat(out, "\xe2\x94\x80", width - 3 - aw) ||
                !view_buf_puts(out, "\xe2\x94\xa4" S_RST))
                goto fail;
        } else {
            if (!view_buf_repeat(out, "\xe2\x94\x80", width - 3) || !view_buf_puts(out, "\xe2\x94\xa4" S_RST))
                goto fail;
        }
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
                if (!view_buf_puts(out, S_SEL_CHEV " \xe2\x80\xba " S_SEL_BOLD "/"))
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
    }

    // 5. Thin-Border Composer Box (calm neutral border; seamlessly joined with ├...┤ when palette is open)
    const char *bcolor = S_BORDER;
    if (boxed) {
        if (pal_items) {
            if (!view_newline(out, &lines) || !view_buf_puts(out, bcolor) || !view_buf_puts(out, "\xe2\x94\x9c") ||
                !view_buf_repeat(out, "\xe2\x94\x80", width - 2) || !view_buf_puts(out, "\xe2\x94\xa4" S_RST))
                goto fail;
        } else if (!show_pend) {
            if (!view_newline(out, &lines) || !view_buf_puts(out, bcolor) || !view_buf_puts(out, "\xe2\x95\xad") ||
                !view_buf_repeat(out, "\xe2\x94\x80", width - 2) || !view_buf_puts(out, "\xe2\x95\xae" S_RST))
                goto fail;
        }
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
            const char *ph = status->replying
                                 ? "Type your answer, or press Esc to return to steering"
                                 : status->busy
                                 ? "Queue steering message while running..."
                                 : (status->pending && status->pending->active &&
                                    status->pending->kind && !strcmp(status->pending->kind, "input"))
                                       ? "Type your reply to the agent and press enter..."
                                       : (status->pending && status->pending->active &&
                                          status->pending->kind &&
                                          !strcmp(status->pending->kind, "approval"))
                                             ? "Press y to approve, a for always, n to deny, or i to inspect"
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
        if (!view_newline(out, &lines) || !view_buf_puts(out, bcolor) ||
            !view_buf_puts(out, "\xe2\x95\xb0") || !view_buf_repeat(out, "\xe2\x94\x80", width - 2) ||
            !view_buf_puts(out, "\xe2\x95\xaf" S_RST))
            goto fail;
    }

    // 6. Single quiet status line below the pure border: model/mode on left, workspace on right
    if (show_footer) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  "))
            goto fail;
        unsigned lw = 2;
        const char *model = status->model && *status->model ? status->model : NULL;
        const char *state =
            !status->busy && !state_is_quiet(status->state) ? status->state : NULL;
        if (model) {
            unsigned mw = 0;
            if (!view_buf_puts(out, S_DIM) ||
                !view_emit_line(out, model, strlen(model), width > lw ? width - lw : 0, &mw) ||
                !view_buf_puts(out, S_RST))
                goto fail;
            lw += mw;
        }
        if (state && lw + 4 < width) {
            if (lw > 2 && !view_buf_puts(out, "   "))
                goto fail;
            if (lw > 2)
                lw += 3;
            unsigned sw = 0;
            if (!view_buf_puts(out, view_status_color(state, status->has_pending, false)) ||
                !view_emit_line(out, state, strlen(state), width > lw ? width - lw : 0, &sw) ||
                !view_buf_puts(out, S_RST))
                goto fail;
            lw += sw;
        }
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
                if (cwd && branch) {
                    if (!view_buf_puts(out, S_FAINT " (" S_RST S_DIM) || !view_buf_puts(out, branch) ||
                        !view_buf_puts(out, S_FAINT ")"))
                        goto fail;
                } else if (branch) {
                    if (!view_buf_puts(out, S_DIM) || !view_buf_puts(out, branch))
                        goto fail;
                }
                if (!view_buf_puts(out, S_RST))
                    goto fail;
            }
        }
    }

    free(pend_rows);
    free(prow);
    frame->text = out->data;
    frame->lines = lines ? lines : 1;
    frame->cursor_row = cursor_row;
    frame->cursor_col = cursor_col;
    frame->hide_cursor = false;
    return true;
fail:
    free(pend_rows);
    free(prow);
    return false;
}

int tui_view_timeout(const tui_view_status *status, uint64_t now_ms) {
    if (!status || !status->busy)
        return -1;
    return (int)(BUSY_PULSE_MS - now_ms % BUSY_PULSE_MS);
}
