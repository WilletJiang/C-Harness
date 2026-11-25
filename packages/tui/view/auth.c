// Login and API configuration: form state, keyboard input, and rendering.
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const preset_names[] = {"ChatGPT", "OpenRouter", "Custom"};
static const char *const preset_urls[] = {
    "https://api.openai.com/v1/responses",
    "https://openrouter.ai/api/v1/responses",
    "",
};
#define PRESET_COUNT 3

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
    (void)view_buf_set(&v->auth_endpoint, ep, strlen(ep));
    wipe_buf(&v->auth_token);
    (void)view_buf_set(&v->auth_token, "", 0);
    (void)view_buf_set(&v->auth_model, md, strlen(md));
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
    bool ok = view_buf_reserve(&clean, n);
    for (size_t i = 0; ok && i < n;) {
        uint32_t cp;
        size_t step = view_decode(s + i, n - i, &cp);
        // Strip newlines/tabs/control chars from single-line API fields
        if (cp >= 32 && !(cp >= 0x7f && cp < 0xa0))
            ok = view_buf_put(&clean, s + i, step);
        i += step;
    }
    if (ok && clean.len && view_buf_insert(b, *cur, clean.data, clean.len))
        *cur += clean.len;
    wipe_buf(&clean);
    free(clean.data);
}

static void move_auth_focus(tui_view *v, unsigned step) {
    do {
        v->auth_focus = (v->auth_focus + step) % 5;
    } while (v->auth_preset == 0 && (v->auth_focus == 1 || v->auth_focus == 2));
}

tui_action tui_view_auth_key(tui_view *v, const tui_auth_state *a, tui_key key, const char *text,
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
                (void)view_buf_set(&v->auth_endpoint, u, strlen(u));
                v->auth_cursor[0] = v->auth_endpoint.len;
            }
        } else if (fb && *cur) {
            *cur = view_prev_cluster(fb->data, fb->len, *cur);
        }
        break;
    case TUI_RIGHT:
        if (v->auth_focus == 0) {
            v->auth_preset = (v->auth_preset + 1) % PRESET_COUNT;
            if (v->auth_preset < 2) {
                const char *u = preset_urls[v->auth_preset];
                (void)view_buf_set(&v->auth_endpoint, u, strlen(u));
                v->auth_cursor[0] = v->auth_endpoint.len;
            }
        } else if (fb && *cur < fb->len) {
            unsigned w;
            uint32_t cp;
            *cur = view_next_cluster(fb->data, fb->len, *cur, &w, &cp);
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
            size_t at = view_prev_cluster(fb->data, fb->len, *cur);
            view_buf_erase(fb, at, *cur - at);
            *cur = at;
            if (v->auth_focus == 1)
                v->auth_preset = detect_preset(fb->data);
        }
        break;
    case TUI_DELETE:
        if (fb && *cur < fb->len) {
            unsigned w;
            uint32_t cp;
            size_t end = view_next_cluster(fb->data, fb->len, *cur, &w, &cp);
            view_buf_erase(fb, *cur, end - *cur);
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
                (void)view_buf_set(&v->auth_endpoint, u, strlen(u));
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

void tui_view_auth_dispose(tui_view *v) {
    wipe_buf(&v->auth_token);
    free(v->auth_endpoint.data);
    free(v->auth_token.data);
    free(v->auth_model.data);
}

/* ---------- Login & API Configuration rendering ---------- */

static bool emit_field_box_row(text_buf *out, const char *val, size_t vlen, size_t cur_byte,
                               const char *placeholder, bool masked, bool focused,
                               unsigned box_w, unsigned *out_cursor_offset) {
    unsigned inner = box_w > 4 ? box_w - 4 : 2; // "│ " + inner + " │"
    const char *bcolor = focused ? S_BORDER_HI : S_BORDER;
    if (!view_buf_puts(out, bcolor) || !view_buf_puts(out, "\xe2\x94\x82 " S_RST))
        return false;
    if (!vlen) {
        if (focused && out_cursor_offset)
            *out_cursor_offset = 2;
        if (!view_buf_puts(out, S_FAINT) ||
            !view_emit_padded(out, placeholder, strlen(placeholder), inner) || !view_buf_puts(out, S_RST))
            return false;
    } else if (masked) {
        // Count clusters up to cursor and total clusters
        size_t total = 0, cur_idx = 0;
        for (size_t pos = 0; pos < vlen;) {
            if (pos < cur_byte)
                ++cur_idx;
            unsigned w;
            uint32_t cp;
            pos = view_next_cluster(val, vlen, pos, &w, &cp);
            ++total;
        }
        if (cur_byte >= vlen)
            cur_idx = total;
        size_t skip = cur_idx >= inner ? cur_idx - inner + 1 : 0;
        size_t show = total > skip ? total - skip : 0;
        if (show > inner)
            show = inner;
        if (!view_buf_puts(out, S_TEXT) || !view_buf_repeat(out, "\xe2\x80\xa2", show) ||
            !view_buf_puts(out, S_RST))
            return false;
        if (show < inner && !view_buf_repeat(out, " ", inner - show))
            return false;
        if (focused && out_cursor_offset)
            *out_cursor_offset = 2 + (unsigned)(cur_idx - skip);
    } else {
        // Horizontal scroll so cursor is always visible inside inner
        unsigned cur_col = view_column_of(val, 0, cur_byte <= vlen ? cur_byte : vlen);
        unsigned skip_col = cur_col >= inner ? cur_col - inner + 1 : 0;
        size_t start = 0;
        unsigned col = 0;
        while (start < vlen && col < skip_col) {
            unsigned w;
            uint32_t cp;
            start = view_next_cluster(val, vlen, start, &w, &cp);
            col += w;
        }
        unsigned used = 0;
        if (!view_buf_puts(out, S_TEXT) ||
            !view_emit_line(out, val + start, vlen - start, inner, &used) || !view_buf_puts(out, S_RST))
            return false;
        if (used < inner && !view_buf_repeat(out, " ", inner - used))
            return false;
        if (focused && out_cursor_offset) {
            unsigned off = cur_col >= col ? cur_col - col : 0;
            *out_cursor_offset = 2 + (off < inner ? off : inner - 1);
        }
    }
    return view_buf_puts(out, " ") && view_buf_puts(out, bcolor) && view_buf_puts(out, "\xe2\x94\x82" S_RST);
}

static bool emit_auth_right_row(text_buf *out, unsigned row, const tui_view *v,
                                const tui_auth_state *a, unsigned right_w) {
    if (!right_w)
        return true;
    unsigned rw = right_w > 3 ? right_w - 3 : 1;
    if (!view_buf_puts(out, S_BORDER "\xe2\x94\x82" S_RST "  "))
        return false;
    if (v->auth_preset == 0) {
        const char *label = row == 0 ? "ChatGPT subscription"
                            : row == 2 ? "Sign in through your browser"
                            : row == 4 ? "Storage: macOS Keychain" : "";
        return view_buf_puts(out, row == 0 ? S_BOLD S_TEXT : S_DIM) &&
               view_emit_padded(out, label, strlen(label), rw) && view_buf_puts(out, S_RST);
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
        return view_buf_puts(out, S_BOLD S_TEXT) && view_emit_padded(out, "Connection", 10, rw) &&
               view_buf_puts(out, S_RST);
    case 2:
        if (v->auth_token.len)
            snprintf(line, sizeof(line), "\xe2\x80\xa2 New key ready (%zu chars)", v->auth_token.len);
        else if (can_keep)
            snprintf(line, sizeof(line), "\xe2\x80\xa2 Configured (%s)",
                     a->token_source && *a->token_source ? a->token_source : "saved");
        else
            snprintf(line, sizeof(line), "! API key required");
        return view_buf_puts(out, tok_ok ? S_DIM : S_AMBER) &&
               view_emit_padded(out, line, strlen(line), rw) && view_buf_puts(out, S_RST);
    case 3:
        snprintf(line, sizeof(line), "%s %s", ep_ok ? "\xe2\x80\xa2" : "!",
                 ep_ok ? "Responses URL entered" : "Enter an HTTPS URL");
        return view_buf_puts(out, ep_ok ? S_DIM : S_AMBER) &&
               view_emit_padded(out, line, strlen(line), rw) && view_buf_puts(out, S_RST);
    case 4:
        snprintf(line, sizeof(line), "Model: %s", mdl);
        return view_render_detail_line(out, line, strlen(line), rw);
    case 6:
        return view_buf_puts(out, S_BOLD S_TEXT) && view_emit_padded(out, "Storage", 7, rw) &&
               view_buf_puts(out, S_RST);
    case 7:
        return view_buf_puts(out, S_LAVENDER) && view_emit_padded(out, cfg, strlen(cfg), rw) &&
               view_buf_puts(out, S_RST);
    case 8:
        return view_buf_puts(out, S_DIM) && view_emit_padded(out, "Mode 0600 (owner-only)", 22, rw) &&
               view_buf_puts(out, S_RST);
    case 10:
        return view_buf_puts(out, S_BOLD S_TEXT) && view_emit_padded(out, "Tips", 4, rw) &&
               view_buf_puts(out, S_RST);
    case 11:
        return view_buf_puts(out, S_DIM) && view_emit_padded(out, "Paste API key directly", 22, rw) &&
               view_buf_puts(out, S_RST);
    case 12:
        return view_buf_puts(out, S_DIM) && view_emit_padded(out, "Press ^r to show/hide key", 25, rw) &&
               view_buf_puts(out, S_RST);
    case 13:
        return view_buf_puts(out, S_DIM) && view_emit_padded(out, "Press ^s to save & apply", 24, rw) &&
               view_buf_puts(out, S_RST);
    default:
        return view_buf_repeat(out, " ", rw);
    }
}

bool tui_view_render_auth(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                          tui_frame *frame) {
    const tui_auth_state *a = status->auth;
    if (!v->was_auth)
        auth_init(v, a);

    unsigned width = view_frame_width(cols);
    unsigned budget = rows > 2 ? rows - 2 : 1;
    bool two_pane = width >= 76;
    unsigned left_w = two_pane ? (width * 62) / 100 : width;
    unsigned right_w = two_pane ? width - left_w - 1 : 0;
    unsigned box_w = left_w > 6 ? left_w - 4 : (left_w > 2 ? left_w - 2 : 2);
    bool chatgpt = v->auth_preset == 0;

    text_buf *out = &v->out;
    out->len = 0;
    if (!view_buf_reserve(out, 2048))
        return false;
    out->data[0] = 0;
    unsigned lines = 0, cursor_row = 0, cursor_col = 0;
    bool hide_cursor = true;

    // 1. Header row
    if (!view_newline(out, &lines))
        return false;
    const char *title = a->onboarding ? ">_ ah  \xc2\xb7  Sign in & API setup"
                                      : "API & Account  \xc2\xb7  Configuration";
    char rhint[34];
    snprintf(rhint, sizeof(rhint), "^s %s  ·  esc %s",
             chatgpt ? "connect" : "save", a->onboarding ? "quit" : "back");
    unsigned rlen = view_span_width(rhint, strlen(rhint));
    unsigned used = 2;
    if (!view_buf_puts(out, "  " S_BOLD S_TEXT))
        return false;
    unsigned tw = 0;
    if (!view_emit_line(out, title, strlen(title), width > used ? width - used : 0, &tw) ||
        !view_buf_puts(out, S_RST))
        return false;
    used += tw;
    if (used + rlen + 4 <= width) {
        if (!view_buf_repeat(out, " ", width - used - rlen - 2) ||
            !view_buf_puts(out, S_KEY "^s" S_DIM) ||
            !view_buf_puts(out, chatgpt ? " connect" : " save") ||
            !view_buf_puts(out, "  \xc2\xb7  " S_KEY "esc" S_DIM) ||
            !view_buf_puts(out, a->onboarding ? " quit" S_RST : " back" S_RST))
            return false;
    }

    // 2. Top hairline
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

    unsigned rrow = 0;
    // Macro-free helper pattern: emit left pane of exact width left_w, then right pane row rrow++
    // Row 0: Provider label + preset pills
    if (lines + 2 < budget) {
        if (!view_newline(out, &lines))
            return false;
        bool f0 = v->auth_focus == 0;
        if (!view_buf_puts(out, "  ") || !view_buf_puts(out, f0 ? S_ACCENT "\xe2\x80\xba " S_BOLD S_TEXT
                                                      : "  " S_DIM) ||
            !view_buf_puts(out, "Provider   " S_RST))
            return false;
        unsigned col = 15;
        for (unsigned p = 0; p < PRESET_COUNT; ++p) {
            unsigned plen = (unsigned)strlen(preset_names[p]) + 2;
            if (col + plen + 1 > left_w)
                break;
            if (p == v->auth_preset) {
                if (!view_buf_puts(out, f0 ? S_SEL_BOLD : S_PILL_ON) || !view_buf_put(out, " ", 1) ||
                    !view_buf_puts(out, preset_names[p]) || !view_buf_puts(out, " " S_RST " "))
                    return false;
            } else {
                if (!view_buf_puts(out, S_DIM " ") || !view_buf_puts(out, preset_names[p]) ||
                    !view_buf_puts(out, " " S_RST " "))
                    return false;
            }
            col += plen + 1;
        }
        if (col < left_w && !view_buf_repeat(out, " ", left_w - col))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Blank spacer
    if (lines + 2 < budget) {
        if (!view_newline(out, &lines) || !view_buf_repeat(out, " ", left_w))
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
        if (!view_newline(out, &lines))
            return false;
        if (!view_buf_puts(out, "  ") || !view_buf_puts(out, focused ? S_ACCENT "\xe2\x80\xba " S_BOLD S_TEXT
                                                           : "  " S_DIM))
            return false;
        unsigned lw = 4;
        unsigned lu = 0;
        if (!view_emit_line(out, labels[fi], strlen(labels[fi]), left_w > lw ? left_w - lw : 0, &lu) ||
            !view_buf_puts(out, S_RST))
            return false;
        lw += lu;
        if (fi == 1) {
            const char *hint = v->auth_reveal ? "visible (^r hide)" : "masked (^r show)";
            unsigned hw = (unsigned)strlen(hint);
            if (lw + hw + 3 <= left_w) {
                if (!view_buf_repeat(out, " ", left_w - lw - hw - 2) || !view_buf_puts(out, S_FAINT) ||
                    !view_buf_puts(out, hint) || !view_buf_puts(out, S_RST "  "))
                    return false;
                lw = left_w;
            }
        }
        if (lw < left_w && !view_buf_repeat(out, " ", left_w - lw))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box top
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  ") || !view_buf_puts(out, bcolor) ||
            !view_buf_puts(out, "\xe2\x95\xad") || !view_buf_repeat(out, "\xe2\x94\x80", box_w - 2) ||
            !view_buf_puts(out, "\xe2\x95\xae" S_RST))
            return false;
        if (2 + box_w < left_w && !view_buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box content
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  "))
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
        if (2 + box_w < left_w && !view_buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;

        // Box bottom
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  ") || !view_buf_puts(out, bcolor) ||
            !view_buf_puts(out, "\xe2\x95\xb0") || !view_buf_repeat(out, "\xe2\x94\x80", box_w - 2) ||
            !view_buf_puts(out, "\xe2\x95\xaf" S_RST))
            return false;
        if (2 + box_w < left_w && !view_buf_repeat(out, " ", left_w - 2 - box_w))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Inline error banner (if any)
    if (a->error && *a->error && lines + 3 < budget) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, "  " S_RED "\xe2\x96\x8e "))
            return false;
        unsigned eu = 0;
        if (!view_emit_line(out, a->error, strlen(a->error), left_w > 5 ? left_w - 5 : 1, &eu) ||
            !view_buf_puts(out, S_RST))
            return false;
        if (4 + eu < left_w && !view_buf_repeat(out, " ", left_w - 4 - eu))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Action button row
    if (lines + 2 < budget) {
        if (!view_newline(out, &lines))
            return false;
        bool fsave = v->auth_focus == 4;
        const char *btn = a->waiting ? " Connecting… "
                          : chatgpt ? " Sign in with ChatGPT "
                          : a->onboarding ? " Save & Continue " : " Save & Apply ";
        unsigned bw = (unsigned)strlen(btn);
        unsigned col = 4;
        if (!view_buf_puts(out, fsave ? "  " S_ACCENT "\xe2\x80\xba " S_SEL_BOLD : "    " S_PILL_ON) ||
            !view_buf_puts(out, btn) || !view_buf_puts(out, S_RST))
            return false;
        col += bw;
        if (!a->onboarding && col + 12 <= left_w) {
            if (!view_buf_puts(out, "   " S_DIM "esc cancel" S_RST))
                return false;
            col += 13;
        }
        if (col < left_w && !view_buf_repeat(out, " ", left_w - col))
            return false;
        if (two_pane && !emit_auth_right_row(out, rrow++, v, a, right_w))
            return false;
    }

    // Bottom hairline
    if (lines < budget) {
        if (!view_newline(out, &lines) || !view_buf_puts(out, S_BORDER))
            return false;
        if (two_pane) {
            if (!view_buf_repeat(out, "\xe2\x94\x80", left_w) || !view_buf_puts(out, "\xe2\x94\xb4") ||
                !view_buf_repeat(out, "\xe2\x94\x80", right_w))
                return false;
        } else if (!view_buf_repeat(out, "\xe2\x94\x80", width)) {
            return false;
        }
        if (!view_buf_puts(out, S_RST))
            return false;
    }

    // Footer shortcut bar
    if (lines < budget) {
        if (!view_newline(out, &lines) ||
            !view_buf_puts(out, "  " S_KEY "\xe2\x86\x91/\xe2\x86\x93" S_DIM " field   " S_KEY
                           "\xe2\x86\x90/\xe2\x86\x92" S_DIM " preset/cursor   ") ||
            (!chatgpt && !view_buf_puts(out, S_KEY "^r" S_DIM " reveal   ")) ||
            !view_buf_puts(out, chatgpt ? S_KEY "^s" S_DIM " connect   "
                                  : S_KEY "^s" S_DIM " save   ") ||
            !view_buf_puts(out, S_KEY "esc" S_DIM))
            return false;
        if (!view_buf_puts(out, a->onboarding ? " quit" S_RST : " back" S_RST))
            return false;
    }

    frame->text = out->data;
    frame->lines = lines ? lines : 1;
    frame->cursor_row = hide_cursor ? frame->lines - 1 : cursor_row;
    frame->cursor_col = hide_cursor ? 0 : cursor_col;
    frame->hide_cursor = hide_cursor;
    return true;
}
