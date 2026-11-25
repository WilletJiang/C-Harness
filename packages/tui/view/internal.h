#ifndef AH_TUI_VIEW_INTERNAL_H
#define AH_TUI_VIEW_INTERNAL_H
#include "view.h"

// Theme-adaptive minimalist palette: terminal-default fg + mid-gray border + signal orange accent
#define S_RST       "\033[0m"
#define S_BOLD      "\033[1m"
#define S_TEXT      ""
#define S_DIM       "\033[2m"
#define S_FAINT     "\033[2m"
#define S_BORDER    "\033[38;5;244m"
#define S_BORDER_HI "\033[38;5;208m"
#define S_ACCENT    "\033[38;5;208m"
#define S_GOLD      ""
#define S_LAVENDER  "\033[2m"
#define S_GREEN     "\033[2m"
#define S_AMBER     "\033[38;5;208m"
#define S_RED       "\033[38;5;167m"
#define S_KEY       "\033[1m"
#define S_SEL_BG    ""
#define S_SEL_CHEV  "\033[1;38;5;208m"
#define S_SEL_BOLD  "\033[0;1m"
#define S_SEL_DESC  "\033[0;2m"
#define S_PILL_ON   "\033[1;7m"

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
    bool dismissed, stashed, was_picker;
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

/* Shared buffers, UTF-8 cell layout, and frame drawing. */
bool view_buf_reserve(text_buf *b, size_t extra);
bool view_buf_insert(text_buf *b, size_t at, const char *s, size_t n);
bool view_buf_put(text_buf *b, const char *s, size_t n);
bool view_buf_puts(text_buf *b, const char *s);
bool view_buf_repeat(text_buf *b, const char *s, size_t n);
void view_buf_erase(text_buf *b, size_t at, size_t n);
bool view_buf_set(text_buf *b, const char *s, size_t n);
size_t view_decode(const char *s, size_t n, uint32_t *cp);
bool view_is_control(uint32_t cp);
size_t view_next_cluster(const char *s, size_t len, size_t pos, unsigned *width, uint32_t *base);
size_t view_prev_cluster(const char *s, size_t len, size_t pos);
size_t view_line_start(const char *s, size_t pos);
unsigned view_span_width(const char *s, size_t n);
bool view_emit_line(text_buf *out, const char *s, size_t n, unsigned max, unsigned *used);
bool view_emit_padded(text_buf *out, const char *s, size_t n, unsigned max);
bool view_wrap(const char *s, size_t len, unsigned width, text_row **rows, size_t *count,
               size_t *cap);
unsigned view_column_of(const char *s, size_t from, size_t to);
unsigned view_frame_width(unsigned cols);
bool view_newline(text_buf *out, unsigned *lines);
const char *view_status_color(const char *st, bool attention, bool working);
bool view_render_detail_line(text_buf *out, const char *s, size_t n, unsigned max);

/* Chat editor layout also used by its renderer. */
bool view_boxed_mode(unsigned cols, unsigned rows);
unsigned view_editor_width(unsigned cols, bool boxed);
bool view_layout_draft(tui_view *v, unsigned cols, bool boxed);
size_t view_row_of(const tui_view *v, size_t pos);
void view_refresh_matches(tui_view *v, const tui_view_status *status);

/* Mode handlers and their shared key helpers. */
char view_lower(char c);
tui_action view_action(tui_view *v, tui_action_kind kind, const char *s, size_t n);
tui_action tui_view_picker_key(tui_view *v, const tui_picker *p, tui_key key, const char *text,
                               size_t count);
tui_action tui_view_auth_key(tui_view *v, const tui_auth_state *a, tui_key key, const char *text,
                             size_t count);
bool tui_view_render_picker(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                            tui_frame *frame);
bool tui_view_render_auth(tui_view *v, const tui_view_status *status, unsigned cols, unsigned rows,
                          tui_frame *frame);
bool tui_view_render_chat(tui_view *, const tui_view_status *, unsigned cols, unsigned rows,
                          uint64_t now_ms, tui_frame *);
void tui_view_auth_dispose(tui_view *);
#endif
