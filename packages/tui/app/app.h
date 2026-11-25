#ifndef AH_TUI_APP_H
#define AH_TUI_APP_H
#include "tui/tui.h"
#include "tui/terminal/terminal.h"
#include "ah/content.h"
#include "ah/observe.h"
#include <stdatomic.h>
#include <sys/types.h>

enum { PREVIEW_BYTES = 4096, LIVE_PAGE = 24, HISTORY_PAGE = 48, TOOL_CACHE_MAX = 32 };
#define TUI_CHATGPT_ENDPOINT "https://api.openai.com/v1/responses"

typedef enum {
    TUI_PICK_NONE, TUI_PICK_SESSIONS, TUI_PICK_AGENTS, TUI_PICK_PENDING,
    TUI_PICK_MODELS, TUI_PICK_EFFORT, TUI_PICK_ASK
} tui_pick_kind;

typedef enum {
    TUI_BLOCK_NONE, TUI_BLOCK_USER, TUI_BLOCK_TOOL, TUI_BLOCK_ASSISTANT, TUI_BLOCK_NOTE
} tui_block_kind;

typedef struct {
    char id[AH_ID_CAP];
    char name[96];
    char summary[256];
    char stats[48];
    uint64_t started_ms;
    bool active;
} tui_tool_entry;

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
    char image_id[AH_ARTIFACT_ID_CAP], image_name[256];
    bool pending_ask;
    char *ask_prompt;
    char model[257], default_model[257], endpoint[512], cwd[256], branch[128];
    char chosen_model[257], model_picker_subtitle[320];
    char effort[16], model_label[288];
    char config_path[512], config_display[256], prefs_path[512];
    char auto_approval_attempt[AH_ID_CAP];
    char token_source[16], auth_error[256];
    bool has_token, endpoint_explicit, allow_loopback_http, chatgpt;
    const char *instructions;
    char notice[768], preview[PREVIEW_BYTES];
    char state[48], pending_status[24], pending_tool[96], pending_summary[512];
    char reply_operation[AH_ID_CAP];
    bool pending_background;
    char active_tool[256];
    char network_status[96], network_label[96];
    bool network_permission_denied;
    unsigned active_tools;
    tui_tool_entry tool_cache[TOOL_CACHE_MAX];
    size_t tool_cache_next;
    tui_pending_card pending_card;
    uint64_t cursor, history;
    int wake[2];
    bool quit, running, refresh_events, restore_history, approve_all;
    tui_block_kind last_block;
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
void tui_block_begin(tui_app *, tui_block_kind);
void tui_line(tui_app *, const char *prefix, const char *text);
void tui_print_banner(tui_app *);
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
bool tui_yolo_set(tui_app *, bool enabled);
void tui_resume_yolo(tui_app *);
ah_code tui_config_apply(tui_app *, const char *endpoint, const char *token, const char *model,
                         ah_error *);
ah_code tui_config_chatgpt(tui_app *, const char *token, const char *model, ah_error *);
ah_code tui_oauth_start(tui_app *, const char *model, bool interactive, ah_error *);
void tui_oauth_read(tui_app *);
void tui_oauth_cancel(tui_app *);
ah_code tui_oauth_refresh(ah_responses *, ah_error *);
ah_code tui_oauth_rejected(void *, const char *, uint64_t, ah_cancel_check, void *, char **);
ah_code tui_open_sessions_picker(tui_app *, ah_error *);
ah_code tui_image_command(tui_app *, const char *, ah_error *);
ah_code tui_submit_image(tui_app *, const char *, const char *, ah_error *);
void tui_input_summary(yyjson_val *, char *, size_t);
void tui_handle_key(void *, tui_key, const char *, size_t);
#endif
