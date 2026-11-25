#ifndef AH_TUI_VIEW_H
#define AH_TUI_VIEW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure C17 view: no terminal I/O. The caller owns the tty, scrollback and redraw loop.
// Supports three modes:
//   1. Inline chat composer with floating slash command palette above the box.
//   2. Interactive two-pane Command Center (when status->picker->open).
//   3. Interactive Login & API Configuration page (when status->auth->open).
typedef struct tui_view tui_view;

typedef enum {
    TUI_TEXT, TUI_PASTE, TUI_ENTER, TUI_ESCAPE, TUI_TAB, TUI_SHIFT_TAB, TUI_NEWLINE,
    TUI_UP, TUI_DOWN, TUI_LEFT, TUI_RIGHT, TUI_HOME, TUI_END,
    TUI_BACKSPACE, TUI_DELETE, TUI_INTERRUPT, TUI_EOF, TUI_SAVE, TUI_REVEAL
} tui_key;

typedef enum {
    TUI_NO_ACTION,
    TUI_SUBMIT,
    TUI_COMMAND,
    TUI_CANCEL,
    TUI_QUIT,
    TUI_PICK_OPEN,
    TUI_PICK_APPROVE,
    TUI_PICK_APPROVE_ALL,
    TUI_PICK_DENY,
    TUI_PICK_NEW,
    TUI_PICK_CLOSE,
    TUI_AUTH_SAVE,
    TUI_AUTH_CANCEL
} tui_action_kind;

typedef struct {
    tui_action_kind kind;
    // Borrowed until the next view call. No leading slash on command names.
    // For TUI_PICK_*, carries the selected item's id (if any).
    const char *text;
} tui_action;

typedef struct {
    const char *name;
    const char *description;
} tui_command;

typedef struct {
    const char *id;
    const char *title;
    const char *status;
    const char *meta;
    const char *detail;
    unsigned indent;
    bool active;
    bool attention;
    bool working;
} tui_picker_item;

typedef struct {
    bool open;
    const char *title;
    const char *subtitle;
    const char *question; /* Optional prompt for a choice-only picker. */
    bool can_open;
    bool can_approve;
    bool can_new;
    const tui_picker_item *items;
    size_t count;
} tui_picker;

typedef struct {
    bool open;
    bool onboarding;
    bool has_token;
    bool chatgpt;
    bool waiting;
    const char *token_source; // "saved", "env", "file", or "none"
    const char *endpoint;
    const char *model;
    const char *config_path;
    const char *error;
} tui_auth_state;

typedef struct {
    bool active;
    const char *kind;      // "approval", "input", or "unknown"
    const char *operation; // operation id
    const char *tool;      // tool name
    const char *summary;   // multiline formatted arguments or question
} tui_pending_card;

typedef struct {
    bool busy;
    bool steering;
    bool replying;
    const char *activity;  // active tool summary while running (optional)
    unsigned active_tools; // number of tools currently executing
    const char *model;
    const char *session;
    const char *state;
    const char *cwd;
    const char *branch;
    unsigned turn;
    bool has_pending;
    const char *notice;
    bool notice_error;
    const char *preview;
    const char *image_name;
    const tui_command *commands;
    size_t command_count;
    const tui_pending_card *pending;
    const tui_picker *picker;
    const tui_auth_state *auth;
} tui_view_status;

typedef struct {
    // UTF-8 lines joined with LF, no trailing LF. May include safe SGR codes (\033[...m).
    // The frame belongs to the view and is valid until the next view call.
    const char *text;
    unsigned lines, cursor_row, cursor_col;
    bool hide_cursor;
} tui_frame;

tui_view *tui_view_create(void);
void tui_view_destroy(tui_view *);
void tui_view_select_picker(tui_view *, size_t row);
tui_action tui_view_key(tui_view *, const tui_view_status *, tui_key, const char *, size_t);
// Commit a successful submission or command. A failed action preserves the draft.
void tui_view_accept(tui_view *, tui_action_kind);
// Read the current form fields after TUI_AUTH_SAVE. Pointers remain valid until next view call.
bool tui_view_auth_values(const tui_view *, const char **endpoint, const char **token,
                          const char **model, bool *chatgpt);
// Securely wipe any in-memory API key draft held by the view.
void tui_view_wipe_auth(tui_view *);
bool tui_view_frame(tui_view *, const tui_view_status *, unsigned cols, unsigned rows,
                    uint64_t now_ms, tui_frame *);
// Milliseconds until animation needs repaint. -1 disables the timer.
int tui_view_timeout(const tui_view_status *, uint64_t now_ms);
#endif
