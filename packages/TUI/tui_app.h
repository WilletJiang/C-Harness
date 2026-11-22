#ifndef AH_TUI_APP_H
#define AH_TUI_APP_H
#include "tui_internal.h"
#include "ah/observe.h"
#include <stdatomic.h>
#include <sys/types.h>

enum { PREVIEW_BYTES = 4096, LIVE_PAGE = 24, HISTORY_PAGE = 48 };
#define TUI_CHATGPT_ENDPOINT "https://api.openai.com/v1/responses"

typedef enum {
    TUI_PICK_NONE, TUI_PICK_SESSIONS, TUI_PICK_AGENTS, TUI_PICK_PENDING,
    TUI_PICK_MODELS, TUI_PICK_EFFORT, TUI_PICK_ASK
} tui_pick_kind;

typedef struct {
    char *id, *title, *status, *meta, *detail;
} tui_pick_strings;

typedef struct tui_stream tui_stream;

typedef struct {
    cli_context *cli;
    ah_responses *responses;
    ah_reader *reader;
    tui_view *view;
    tui_terminal tty;
    tui_graphics graphics;
    tui_input input;
    tui_view_status status;
    unsigned cols, rows, turn;
    char session[AH_ID_CAP], pending_session[AH_ID_CAP], operation[AH_ID_CAP];
    char ask_seen[AH_ID_CAP];
    bool pending_ask;
    char *ask_prompt;
    char model[257], default_model[257], endpoint[512], cwd[256], branch[128];
    char chosen_model[257], model_picker_subtitle[320];
    char effort[16], model_label[288];
    char config_path[512], config_display[256];
    char token_source[16], auth_error[256];
    bool has_token, endpoint_explicit, allow_loopback_http, chatgpt;
    const char *instructions;
    char notice[768], preview[PREVIEW_BYTES];
    char state[48], pending_status[24];
    uint64_t cursor, history, run_started_ms;
    int wake[2];
    bool quit, running, refresh_events, restore_history;
    pthread_t worker;
    pthread_mutex_t preview_lock;
    atomic_bool worker_done, journal_dirty;
    ah_error run_error;
    ah_code run_code;
    bool catalog_loading;
    yyjson_doc *catalog;
    tui_stream *streams;
    uint64_t stream_attempt, stream_active;
    size_t stream_bytes;
    // Interactive Command Center / picker backing store
    tui_pick_kind pick_kind;
    tui_picker picker;
    tui_picker_item *pick_items;
    tui_pick_strings *pick_strings;
    size_t pick_count;
    // Interactive Login & API Configuration page
    tui_auth_state auth;
    pid_t oauth_pid;
    int oauth_fd;
    size_t oauth_used;
    char *oauth_reply;
    char oauth_model[257];
} tui_app;

const tui_command *tui_commands(size_t *count);
void tui_notify(int fd);
void tui_observe(void *, const ah_event *);
void tui_stream_observe(tui_app *, const ah_event *);
void tui_stream_render(tui_app *);
void tui_stream_commit(tui_app *, yyjson_val *output, bool live);
void tui_stream_abort(tui_app *, const char *reason);
void tui_stream_free(tui_app *);
void tui_show_error(tui_app *, ah_code, const ah_error *);
void tui_line(tui_app *, const char *prefix, const char *text);
void tui_print_banner(tui_app *);
void tui_print_worked(tui_app *, uint64_t elapsed_ms);
void tui_show_json(tui_app *, const char *json);
ah_code tui_snapshot(tui_app *, const char *session, bool set_cursor, ah_error *);
ah_code tui_history_page(tui_app *, uint64_t boundary, ah_error *);
void tui_restore_history(tui_app *);
void tui_refresh(tui_app *);
bool tui_start_run(tui_app *, ah_error *);
void tui_picker_close(tui_app *);
bool tui_picker_alloc(tui_app *, size_t count);
void tui_picker_set(tui_app *, size_t, const char *, const char *, const char *, const char *,
                    const char *, unsigned, bool, bool, bool);
ah_code tui_switch_session(tui_app *, const char *, ah_error *);
ah_code tui_models_open(tui_app *, ah_error *);
void tui_models_ready(tui_app *);
void tui_models_sync(tui_app *);
ah_code tui_models_choose(tui_app *, const char *, ah_error *);
ah_code tui_models_back(tui_app *, ah_error *);
bool tui_ask_has_options(yyjson_val *arguments);
ah_code tui_ask_picker(tui_app *, const char *operation, yyjson_val *arguments, ah_error *);
void tui_ask_maybe_open(tui_app *);
ah_code tui_open_pending_picker(tui_app *, ah_error *);
void tui_auth_open(tui_app *, bool onboarding);
void tui_auth_close(tui_app *);
void tui_config_init_paths(tui_app *);
void tui_config_load(tui_app *);
bool tui_config_save_model(tui_app *, const char *);
ah_code tui_config_apply(tui_app *, const char *endpoint, const char *token, const char *model,
                         ah_error *);
ah_code tui_config_chatgpt(tui_app *, const char *token, const char *model, ah_error *);
ah_code tui_oauth_start(tui_app *, const char *model, bool interactive, ah_error *);
void tui_oauth_read(tui_app *);
void tui_oauth_cancel(tui_app *);
ah_code tui_oauth_refresh(ah_responses *, ah_error *);
void tui_handle_key(void *, tui_key, const char *, size_t);
#endif
