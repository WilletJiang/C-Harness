#include "tui_app.h"
#include "ah/agents.h"
#include "ah/content.h"
#include "ah/context.h"
#include "ah/workspace.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const tui_command commands[] = {
    {"login", "Configure API key, endpoint, and default model"},
    {"config", "Open API & account configuration page"},
    {"auto-approve", "Toggle automatic approval for ordinary workspace edits"},
    {"model", "Choose model and reasoning effort"},
    {"sessions", "Open Command Center to switch or create sessions"},
    {"agents", "Open interactive agent tree inspector"},
    {"pending", "Review and approve or deny pending operation"},
    {"approve", "Allow the pending operation"},
    {"deny", "Deny the pending operation"},
    {"cancel", "Stop the active run"},
    {"resume", "Resume a stopped or ready session"},
    {"history", "Load an earlier history page"},
    {"fork", "Fork the current session"},
    {"inspect", "Inspect raw session JSON"},
    {"context", "Inspect context budget and checkpoints"},
    {"artifacts", "List saved attachments"},
    {"workspace", "Inspect workspace ownership"},
    {"quit", "Exit ah"},
};

const tui_command *tui_commands(size_t *count) {
    *count = sizeof(commands) / sizeof(*commands);
    return commands;
}

static char *dup_str(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, s, n);
    return copy;
}

static const char *jstr(yyjson_val *obj, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(obj, key));
    return s ? s : "";
}

void tui_picker_close(tui_app *app) {
    for (size_t i = 0; i < app->pick_count; ++i) {
        free(app->pick_strings[i].id);
        free(app->pick_strings[i].title);
        free(app->pick_strings[i].status);
        free(app->pick_strings[i].meta);
        free(app->pick_strings[i].detail);
    }
    free(app->pick_strings);
    free(app->pick_items);
    app->pick_strings = NULL;
    app->pick_items = NULL;
    app->pick_count = 0;
    app->pick_kind = TUI_PICK_NONE;
    memset(&app->picker, 0, sizeof(app->picker));
}

bool tui_picker_alloc(tui_app *app, size_t count) {
    tui_picker_close(app);
    if (!count)
        return true;
    app->pick_items = calloc(count, sizeof(*app->pick_items));
    app->pick_strings = calloc(count, sizeof(*app->pick_strings));
    if (!app->pick_items || !app->pick_strings) {
        tui_picker_close(app);
        return false;
    }
    app->pick_count = count;
    return true;
}

void tui_picker_set(tui_app *app, size_t i, const char *id, const char *title,
                       const char *status, const char *meta, const char *detail, unsigned indent,
                       bool active, bool attention, bool working) {
    tui_pick_strings *s = &app->pick_strings[i];
    s->id = dup_str(id);
    s->title = dup_str(title);
    s->status = dup_str(status);
    s->meta = dup_str(meta);
    s->detail = dup_str(detail);
    app->pick_items[i] = (tui_picker_item){
        .id = s->id,
        .title = s->title,
        .status = s->status,
        .meta = s->meta,
        .detail = s->detail,
        .indent = indent,
        .active = active,
        .attention = attention,
        .working = working,
    };
}

static bool state_needs_attention(const char *st) {
    return !strcmp(st, "waiting_approval") || !strcmp(st, "waiting_input") ||
           !strcmp(st, "outcome_unknown") || !strcmp(st, "approval") || !strcmp(st, "input") ||
           !strcmp(st, "unknown");
}

static ah_code open_sessions_picker(tui_app *app, ah_error *error) {
    char *json = NULL;
    ah_code code = ah_sessions_json(app->cli->engine, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    size_t count = yyjson_is_arr(root) ? yyjson_arr_size(root) : 0;
    if (!tui_picker_alloc(app, count)) {
        yyjson_doc_free(doc);
        return AH_IO;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(root, idx, max, item) {
        const char *id = jstr(item, "id");
        const char *mdl = jstr(item, "model");
        const char *st = jstr(item, "state");
        uint64_t turn = yyjson_get_uint(yyjson_obj_get(item, "turn"));
        uint64_t mcalls = yyjson_get_uint(yyjson_obj_get(item, "model_calls"));
        uint64_t tcalls = yyjson_get_uint(yyjson_obj_get(item, "tool_calls"));
        bool active = !strcmp(id, app->session);
        char meta[64], detail[512];
        snprintf(meta, sizeof(meta), "turn %llu", (unsigned long long)turn);
        snprintf(detail, sizeof(detail),
                 "Session: %s\nStatus: %s%s\nModel: %s\nTurn: %llu\nModel calls: %llu\nTool calls: %llu",
                 id, st, active ? " (current)" : "", *mdl ? mdl : "-",
                 (unsigned long long)turn, (unsigned long long)mcalls, (unsigned long long)tcalls);
        tui_picker_set(app, idx, id, id, st, meta, detail, 0, active, state_needs_attention(st),
                   !strcmp(st, "running"));
    }
    yyjson_doc_free(doc);
    app->pick_kind = TUI_PICK_SESSIONS;
    app->picker = (tui_picker){
        .open = true,
        .title = "Command center",
        .subtitle = "Sessions",
        .can_open = true,
        .can_new = true,
        .items = app->pick_items,
        .count = app->pick_count,
    };
    return AH_OK;
}

static ah_code open_agents_picker(tui_app *app, ah_error *error) {
    char *json = NULL;
    ah_code code = ah_agents_json(app->cli->engine, app->session, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *arr = yyjson_obj_get(root, "agents");
    size_t count = yyjson_is_arr(arr) ? yyjson_arr_size(arr) : 0;
    if (!tui_picker_alloc(app, count)) {
        yyjson_doc_free(doc);
        return AH_IO;
    }
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        const char *id = jstr(item, "id");
        const char *name = jstr(item, "name");
        const char *st = jstr(item, "state");
        const char *parent = jstr(item, "parent");
        unsigned depth = (unsigned)yyjson_get_uint(yyjson_obj_get(item, "depth"));
        uint64_t turn = yyjson_get_uint(yyjson_obj_get(item, "turn"));
        yyjson_val *usage = yyjson_obj_get(item, "usage");
        uint64_t in_tok = yyjson_get_uint(yyjson_obj_get(usage, "input_tokens"));
        uint64_t out_tok = yyjson_get_uint(yyjson_obj_get(usage, "output_tokens"));
        bool active = !strcmp(id, app->session);
        char meta[64], detail[640];
        snprintf(meta, sizeof(meta), "turn %llu", (unsigned long long)turn);
        snprintf(detail, sizeof(detail),
                 "Agent: %s\nSession: %s\nParent: %s\nDepth: %u\nStatus: %s%s\nTurn: %llu\nTokens: %llu in / %llu out",
                 *name ? name : "root", id, *parent ? parent : "-", depth, st,
                 active ? " (current)" : "", (unsigned long long)turn,
                 (unsigned long long)in_tok, (unsigned long long)out_tok);
        tui_picker_set(app, idx, id, *name ? name : id, st, meta, detail, depth, active,
                   state_needs_attention(st), !strcmp(st, "running"));
    }
    yyjson_doc_free(doc);
    app->pick_kind = TUI_PICK_AGENTS;
    app->picker = (tui_picker){
        .open = true,
        .title = "Command center",
        .subtitle = "Agent tree",
        .can_open = true,
        .items = app->pick_items,
        .count = app->pick_count,
    };
    return AH_OK;
}

static ah_code open_pending_picker(tui_app *app, ah_error *error) {
    const char *session = app->pending_session[0] ? app->pending_session : app->session;
    char *json = NULL;
    ah_code code = ah_reader_session_json(app->reader, session, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *pending = yyjson_obj_get(root, "pending");
    const char *st = jstr(pending, "status");
    if (!*st) {
        yyjson_doc_free(doc);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "No pending operation in this session");
        return AH_OK;
    }
    const char *op_id = jstr(pending, "operation_id");
    const char *name = jstr(pending, "name");
    if (!strcmp(st, "unknown")) {
        yyjson_doc_free(doc);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Tool result unconfirmed. Send a new message.");
        return AH_OK;
    }
    bool approval = !strcmp(st, "approval");
    const char *subtitle = approval ? "Press a to approve or d to deny"
                           : !strcmp(st, "input") ? "Awaiting input" : "Review required";
    yyjson_val *args = yyjson_obj_get(pending, "arguments");
    char *args_pretty = args ? yyjson_val_write(args, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL) : NULL;
    if (!tui_picker_alloc(app, 1)) {
        free(args_pretty);
        yyjson_doc_free(doc);
        return AH_IO;
    }
    char detail[1024];
    snprintf(detail, sizeof(detail),
             "Type: %s\nTool: %s\nOperation: %s\nSession: %s\n\nArguments:\n%.700s",
             st, *name ? name : "-", *op_id ? op_id : "-", session,
             args_pretty ? args_pretty : "{}");
    free(args_pretty);
    tui_picker_set(app, 0, op_id, *name ? name : st, st, session, detail, 0, true, true, false);
    yyjson_doc_free(doc);
    app->pick_kind = TUI_PICK_PENDING;
    app->picker = (tui_picker){
        .open = true,
        .title = "Pending operation",
        .subtitle = subtitle,
        .can_approve = approval,
        .items = app->pick_items,
        .count = app->pick_count,
    };
    return AH_OK;
}

ah_code tui_switch_session(tui_app *app, const char *target, ah_error *error) {
    if (!target || !*target || strlen(target) >= sizeof(app->session)) {
        snprintf(error->message, sizeof(error->message), "Invalid session ID");
        return AH_INVALID;
    }
    if (!strcmp(target, app->session)) {
        tui_picker_close(app);
        return AH_OK;
    }
    strcpy(app->session, target);
    app->pending_session[0] = app->operation[0] = app->pending_status[0] = 0;
    app->notice[0] = 0;
    app->status.notice_error = false;
    ah_code code = tui_snapshot(app, app->session, true, error);
    if (code != AH_OK)
        return code;
    tui_models_sync(app);
    tui_picker_close(app);
    tui_print_banner(app);
    (void)tui_history_page(app, app->cursor + 1, error);
    return AH_OK;
}

static void command_id(char id[80]) {
    static uint64_t serial;
    struct timespec time = {0};
    clock_gettime(CLOCK_REALTIME, &time);
    snprintf(id, 80, "tui-%lld-%ld-%ld-%llu", (long long)time.tv_sec, time.tv_nsec,
             (long)getpid(), (unsigned long long)++serial);
}

static ah_code submit(tui_app *app, const char *text, ah_error *error) {
    if (app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Loading model catalog");
        return AH_BUSY;
    }
    char id[80];
    command_id(id);
    ah_engine *engine = app->cli->engine;
    if (app->running) {
        ah_code code = ah_steer_turn(engine, app->session, app->turn, id, text, NULL, error);
        if (code == AH_OK) {
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Steering queued");
        }
        return code;
    }
    ah_code code;
    if (!strcmp(app->pending_status, "input")) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *content = doc ? yyjson_mut_str(doc, text) : NULL;
        if (content)
            yyjson_mut_doc_set_root(doc, content);
        char *json = content ? yyjson_mut_write(doc, 0, NULL) : NULL;
        code = json ? ah_reply(engine, app->pending_session, app->operation, json, error) : AH_IO;
        free(json);
        yyjson_mut_doc_free(doc);
        if (code == AH_OK) {
            app->pending_status[0] = app->operation[0] = app->pending_session[0] = 0;
            tui_line(app, "> ", text);
        }
    } else {
        code = ah_submit_after_interruption(engine, app->session, id, text, error);
        if (code == AH_OK) {
            ah_error ignored = {0};
            (void)tui_snapshot(app, app->session, false, &ignored);
        }
    }
    if (code == AH_OK && !tui_start_run(app, error))
        tui_show_error(app, AH_IO, error);
    return code;
}

static ah_code decide_pending(tui_app *app, bool approve, ah_error *error) {
    if (app->running || app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Available when execution pauses");
        return AH_BUSY;
    }
    if (strcmp(app->pending_status, "approval")) {
        snprintf(error->message, sizeof(error->message), "No pending approval");
        return AH_NOT_FOUND;
    }
    ah_code code =
        ah_decide(app->cli->engine, app->pending_session, app->operation, approve, error);
    if (code == AH_OK) {
        app->pending_status[0] = app->operation[0] = app->pending_session[0] = 0;
        tui_picker_close(app);
        tui_line(app, "", approve ? "Approved" : "Denied");
        if (!tui_start_run(app, error))
            tui_show_error(app, AH_IO, error);
    }
    return code;
}

static ah_code command(tui_app *app, const char *name, ah_error *error) {
    if (!strcmp(name, "quit")) {
        if (app->running) {
            snprintf(error->message, sizeof(error->message), "Use /cancel before exiting");
            return AH_BUSY;
        }
        app->quit = true;
        return AH_OK;
    }
    if (!strcmp(name, "cancel")) {
        if (app->running) {
            ah_cancel(app->cli->engine);
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Stopping");
        }
        return AH_OK;
    }
    if (!strcmp(name, "history"))
        return app->history ? tui_history_page(app, app->history, error) : AH_OK;
    if (!strcmp(name, "pending"))
        return open_pending_picker(app, error);
    if (!strcmp(name, "inspect")) {
        char *json = NULL;
        ah_code code = ah_reader_session_json(app->reader, app->session, &json, error);
        if (code == AH_OK)
            tui_show_json(app, json);
        ah_free(json);
        return code;
    }
    if (app->running || app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Available when execution pauses");
        return AH_BUSY;
    }
    ah_engine *engine = app->cli->engine;
    ah_code code = AH_OK;
    char *json = NULL;
    if (!strcmp(name, "sessions"))
        return open_sessions_picker(app, error);
    if (!strcmp(name, "model"))
        return tui_models_open(app, error);
    if (!strcmp(name, "auto-approve")) {
        ah_local_tools *tools = app->cli->local_tools;
        if (!tools) {
            snprintf(error->message, sizeof(error->message), "Start with --workspace to enable auto-approve");
            return AH_INVALID;
        }
        bool enabled = !ah_local_tools_auto_approve(tools);
        ah_local_tools_set_auto_approve(tools, enabled);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Auto-approve %s", enabled ? "on" : "off");
        return AH_OK;
    }
    if (!strcmp(name, "agents"))
        return open_agents_picker(app, error);
    if (!strcmp(name, "login") || !strcmp(name, "config")) {
        tui_auth_open(app, false);
        return AH_OK;
    }
    if (!strcmp(name, "context"))
        code = ah_context_json(engine, app->session, &json, error);
    else if (!strcmp(name, "artifacts"))
        code = ah_artifacts_json(engine, app->session, &json, error);
    else if (!strcmp(name, "workspace"))
        code = ah_workspace_json(engine, &json, error);
    else if (!strcmp(name, "fork")) {
        char id[AH_ID_CAP];
        code = ah_fork(engine, app->session, id, error);
        if (code == AH_OK)
            tui_line(app, "Forked  ", id);
    } else if (!strcmp(name, "resume")) {
        if (!strcmp(app->state, "stopped") || !strcmp(app->state, "failed"))
            code = ah_resume(engine, app->session, error);
        if (code == AH_OK && !tui_start_run(app, error))
            code = AH_IO;
    } else if (!strcmp(name, "approve") || !strcmp(name, "deny")) {
        return decide_pending(app, !strcmp(name, "approve"), error);
    } else {
        code = AH_NOT_FOUND;
    }
    if (json) {
        tui_show_json(app, json);
        ah_free(json);
    }
    return code;
}

void tui_handle_key(void *context, tui_key key, const char *text, size_t count) {
    tui_app *app = context;
    if (app->quit)
        return;
    tui_restore_history(app);
    if (key == TUI_PASTE && !text) {
        app->status.notice_error = true;
        snprintf(app->notice, sizeof(app->notice), "Paste exceeded input limit");
        return;
    }
    tui_action action = tui_view_key(app->view, &app->status, key, text, count);
    ah_error error = {0};
    ah_code code = AH_OK;
    switch (action.kind) {
    case TUI_SUBMIT: code = submit(app, action.text, &error); break;
    case TUI_COMMAND: code = command(app, action.text, &error); break;
    case TUI_CANCEL: code = command(app, "cancel", &error); break;
    case TUI_QUIT: code = command(app, "quit", &error); break;
    case TUI_PICK_CLOSE:
        if (app->pick_kind == TUI_PICK_EFFORT) {
            code = tui_models_back(app, &error);
            break;
        }
        tui_picker_close(app);
        return;
    case TUI_PICK_OPEN:
        code = app->pick_kind == TUI_PICK_MODELS || app->pick_kind == TUI_PICK_EFFORT
                   ? tui_models_choose(app, action.text, &error)
                   : tui_switch_session(app, action.text, &error);
        break;
    case TUI_PICK_APPROVE:
    case TUI_PICK_DENY:
        if (!action.text || strcmp(action.text, app->operation)) {
            snprintf(error.message, sizeof(error.message), "Pending operation changed");
            code = AH_BUSY;
        } else {
            code = decide_pending(app, action.kind == TUI_PICK_APPROVE, &error);
        }
        break;
    case TUI_PICK_NEW: {
        char id[AH_ID_CAP];
        code = ah_session_create(app->cli->engine, app->default_model,
                                 app->instructions, id, &error);
        if (code == AH_OK)
            code = tui_switch_session(app, id, &error);
        break;
    }
    case TUI_AUTH_CANCEL:
        tui_oauth_cancel(app);
        if (app->auth.onboarding)
            app->quit = true;
        else
            tui_auth_close(app);
        return;
    case TUI_AUTH_SAVE: {
        const char *ep = NULL, *tok = NULL, *mdl = NULL;
        bool chatgpt = false;
        if (tui_view_auth_values(app->view, &ep, &tok, &mdl, &chatgpt)) {
            code = chatgpt ? tui_oauth_start(app, mdl, true, &error)
                           : tui_config_apply(app, ep, tok, mdl, &error);
            if (code != AH_OK) {
                snprintf(app->auth_error, sizeof(app->auth_error), "%s",
                         error.message[0] ? error.message : ah_code_name(code));
                return;
            }
        }
        return;
    }
    case TUI_NO_ACTION: return;
    }
    if (code == AH_OK)
        tui_view_accept(app->view, action.kind);
    else
        tui_show_error(app, code, &error);
}
