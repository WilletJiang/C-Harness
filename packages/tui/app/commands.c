#include "app.h"
#include "ah/agents.h"
#include "ah/content.h"
#include "ah/context.h"
#include "ah/workspace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const tui_command commands[] = {
    {"login", "Configure API key, endpoint, and default model"},
    {"config", "Open API & account configuration page"},
    {"auto-approve", "Toggle automatic approval for ordinary workspace edits"},
    {"approve-all", "Alias for /yolo"},
    {"yolo", "Toggle all tool approvals (or /yolo on|off)"},
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
    {"note", "Queue text for the next model request: /note TEXT"},
    {"image", "Attach an image: /image PATH or /image clear"},
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
    free(app->ask_prompt);
    app->ask_prompt = NULL;
    for (size_t i = 0; i < app->pick_count; ++i) {
        free(app->pick_strings[i].id);
        free(app->pick_strings[i].title);
        free(app->pick_strings[i].status);
        free(app->pick_strings[i].meta);
        free(app->pick_strings[i].detail);
        free(app->pick_strings[i].database);
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
    return tui_picker_extend(app, count);
}

bool tui_picker_extend(tui_app *app, size_t count) {
    if (!count)
        return true;
    if (count > SIZE_MAX - app->pick_count)
        return false;
    size_t total = app->pick_count + count;
    if (total > SIZE_MAX / sizeof(*app->pick_items) ||
        total > SIZE_MAX / sizeof(*app->pick_strings))
        return false;
    tui_pick_strings *strings = realloc(app->pick_strings, total * sizeof(*strings));
    if (!strings)
        return false;
    app->pick_strings = strings;
    tui_picker_item *items = realloc(app->pick_items, total * sizeof(*items));
    if (!items)
        return false;
    app->pick_items = items;
    memset(strings + app->pick_count, 0, count * sizeof(*strings));
    memset(items + app->pick_count, 0, count * sizeof(*items));
    app->pick_count = total;
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

ah_code tui_open_pending_picker(tui_app *app, ah_error *error) {
    if (!app->session[0])
        return ah_fail(error, AH_NOT_FOUND, "No pending operation");
    const char *session = !app->running && app->pending_session[0] ? app->pending_session
                                                                 : app->session;
    char *json = NULL;
    ah_code code = ah_reader_session_json(app->reader, session, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *question = yyjson_obj_get(root, "question");
    yyjson_val *pending = yyjson_is_obj(question) ? question : yyjson_obj_get(root, "pending");
    const char *st = jstr(pending, "status");
    if (!*st) {
        yyjson_doc_free(doc);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "No pending operation in this session");
        return AH_OK;
    }
    const char *op_id = jstr(pending, "operation_id");
    const char *name = jstr(pending, "name");
    bool background = yyjson_is_true(yyjson_obj_get(pending, "background"));
    if (!strcmp(st, "unknown")) {
        yyjson_doc_free(doc);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Tool result unconfirmed. Send a new message.");
        return AH_OK;
    }
    bool approval = !strcmp(st, "approval");
    const char *subtitle = approval ? "a approve once   A approve all   d deny"
                           : !strcmp(st, "input") ? "Awaiting input" : "Review required";
    yyjson_val *args = yyjson_obj_get(pending, "arguments");
    if (!strcmp(st, "input") && tui_ask_has_options(args)) {
        code = tui_ask_picker(app, op_id, args, error);
        yyjson_doc_free(doc);
        return code;
    }
    if (background) {
        snprintf(app->reply_operation, sizeof(app->reply_operation), "%s", op_id);
        snprintf(app->notice, sizeof(app->notice), "%s", jstr(args, "question"));
        app->status.notice_error = false;
        yyjson_doc_free(doc);
        return AH_OK;
    }
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

static void command_id(char id[80]) {
    static uint64_t serial;
    struct timespec time = {0};
    clock_gettime(CLOCK_REALTIME, &time);
    snprintf(id, 80, "tui-%lld-%ld-%ld-%llu", (long long)time.tv_sec, time.tv_nsec,
             (long)getpid(), (unsigned long long)++serial);
}

static ah_code answer_question(tui_app *app, const char *session, const char *operation,
                               const char *text, ah_error *error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *content = doc ? yyjson_mut_str(doc, text) : NULL;
    if (content)
        yyjson_mut_doc_set_root(doc, content);
    char *json = content ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json)
        return AH_IO;
    ah_engine *engine = app->cli->engine;
    ah_code code = app->running ? ah_queue_reply(engine, session, app->turn, operation, json, error)
                                : ah_reply(engine, session, operation, json, error);
    free(json);
    return code;
}

static ah_code submit(tui_app *app, const char *text, ah_error *error) {
    if (app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Loading model catalog");
        return AH_BUSY;
    }
    char id[80];
    command_id(id);
    ah_engine *engine = app->cli->engine;
    if (app->reply_operation[0]) {
        ah_code code = answer_question(app, app->session, app->reply_operation, text, error);
        if (code == AH_OK) {
            app->reply_operation[0] = 0;
            app->notice[0] = 0;
            if (!app->running && !tui_start_run(app, error))
                tui_show_error(app, AH_IO, error);
        }
        return code;
    }
    if (app->running) {
        if (app->image_id[0]) {
            snprintf(error->message, sizeof(error->message), "Wait for the current run to send the attached image");
            return AH_BUSY;
        }
        ah_code code = ah_steer_turn(engine, app->session, app->turn, id, text, NULL, error);
        if (code == AH_OK) {
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Steering queued");
        }
        return code;
    }
    ah_code code;
    if (!strcmp(app->pending_status, "input")) {
        code = answer_question(app, app->pending_session, app->operation, text, error);
        if (code == AH_OK) {
            app->pending_status[0] = app->operation[0] = app->pending_session[0] = 0;
            app->pending_ask = false;
            app->pending_tool[0] = app->pending_summary[0] = 0;
            if (!app->pending_background)
                tui_line(app, "> ", text);
            app->pending_background = false;
        }
    } else {
        code = tui_submit_input(app, id, text, error);
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
        app->pending_tool[0] = app->pending_summary[0] = 0;
        tui_picker_close(app);
        tui_line(app, approve ? "" : "! ", approve ? "Approved" : "Denied");
        if (!tui_start_run(app, error))
            tui_show_error(app, AH_IO, error);
    }
    return code;
}

void tui_resume_yolo(tui_app *app) {
    if (app->quit || app->running || app->catalog_loading || !app->has_token || app->auth.open ||
        !app->approve_all || strcmp(app->pending_status, "approval") || !app->operation[0] ||
        strcmp(app->pending_session, app->session) ||
        !strcmp(app->auto_approval_attempt, app->operation))
        return;
    strcpy(app->auto_approval_attempt, app->operation);
    ah_error error = {0};
    ah_code code = decide_pending(app, true, &error);
    if (code != AH_OK)
        tui_show_error(app, code, &error);
}

static void set_yolo(tui_app *app, bool enabled) {
    bool saved = tui_yolo_set(app, enabled);
    app->status.notice_error = !saved;
    snprintf(app->notice, sizeof(app->notice), "YOLO mode %s%s", enabled ? "on" : "off",
             saved ? "" : " (could not save preference)");
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
    if (!strcmp(name, "image") || !strncmp(name, "image ", 6) || !strncmp(name, "image\t", 6))
        return tui_image_command(app, name + 5, error);
    if (!strcmp(name, "history"))
        return app->history ? tui_history_page(app, app->history, error) : AH_OK;
    if (!strcmp(name, "pending"))
        return tui_open_pending_picker(app, error);
    if (!strcmp(name, "note") || !strncmp(name, "note ", 5) || !strncmp(name, "note\t", 5)) {
        if (!app->session[0])
            return ah_fail(error, AH_BUSY, "Notes require an active run");
        const char *text = name + 4;
        while (*text == ' ' || *text == '\t')
            ++text;
        char id[80];
        command_id(id);
        ah_code code = ah_queue_turn_input(app->cli->engine, app->session, app->turn, id, text,
                                           NULL, error);
        if (code == AH_OK) {
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Note queued for the next model request");
        }
        return code;
    }
    if (!strcmp(name, "inspect")) {
        if (!app->session[0])
            return ah_fail(error, AH_NOT_FOUND, "Send a message to start a conversation");
        char *json = NULL;
        ah_code code = ah_reader_session_json(app->reader, app->session, &json, error);
        if (code == AH_OK)
            tui_show_json(app, json);
        ah_free(json);
        return code;
    }
    if (!strcmp(name, "auto-approve")) {
        ah_local_tools *tools = app->cli->local_tools;
        if (!tools) {
            snprintf(error->message, sizeof(error->message), "Start with --workspace to enable auto-approve");
            return AH_INVALID;
        }
        if (app->running || app->catalog_loading) {
            snprintf(error->message, sizeof(error->message), "Available when execution pauses");
            return AH_BUSY;
        }
        bool enabled = !ah_local_tools_auto_approve(tools);
        ah_local_tools_set_auto_approve(tools, enabled);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Auto-approve %s", enabled ? "on" : "off");
        return AH_OK;
    }
    if (!strcmp(name, "approve-all") || !strcmp(name, "yolo")) {
        set_yolo(app, !app->approve_all);
        return AH_OK;
    }
    if (!strncmp(name, "yolo ", 5)) {
        const char *arg = name + 5;
        while (*arg == ' ' || *arg == '\t')
            ++arg;
        if (strcmp(arg, "on") && strcmp(arg, "off")) {
            snprintf(error->message, sizeof(error->message), "Usage: /yolo [on|off]");
            return AH_INVALID;
        }
        set_yolo(app, !strcmp(arg, "on"));
        return AH_OK;
    }
    if (app->running || app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Available when execution pauses");
        return AH_BUSY;
    }
    ah_engine *engine = app->cli->engine;
    ah_code code = AH_OK;
    char *json = NULL;
    if (!strcmp(name, "sessions"))
        return tui_open_sessions_picker(app, error);
    if (!strcmp(name, "model"))
        return tui_models_open(app, error);
    if (!strcmp(name, "login") || !strcmp(name, "config")) {
        tui_auth_open(app, false);
        return AH_OK;
    }
    if (!strcmp(name, "approve") || !strcmp(name, "deny"))
        return decide_pending(app, !strcmp(name, "approve"), error);
    if (!strcmp(name, "workspace"))
        code = ah_workspace_json(engine, &json, error);
    else if (!app->session[0])
        return ah_fail(error, AH_NOT_FOUND, "Send a message to start a conversation");
    else if (!strcmp(name, "agents"))
        return open_agents_picker(app, error);
    else if (!strcmp(name, "context"))
        code = ah_context_json(engine, app->session, &json, error);
    else if (!strcmp(name, "artifacts"))
        code = ah_artifacts_json(engine, app->session, &json, error);
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
    if (key == TUI_ESCAPE && !app->picker.open && app->reply_operation[0]) {
        app->reply_operation[0] = 0;
        app->notice[0] = 0;
        return;
    }
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
        if (app->pick_kind == TUI_PICK_ASK) {
            if (app->catalog_loading || !action.text || !app->ask_seen[0]) {
                snprintf(error.message, sizeof(error.message), "Pending question changed");
                code = AH_BUSY;
            } else if (app->running) {
                snprintf(app->reply_operation, sizeof(app->reply_operation), "%s", app->ask_seen);
                if (*action.text)
                    code = submit(app, action.text, &error);
                if (code == AH_OK)
                    tui_picker_close(app);
                else
                    app->reply_operation[0] = 0;
            } else if (strcmp(app->pending_status, "input") || strcmp(app->ask_seen, app->operation)) {
                snprintf(error.message, sizeof(error.message), "Pending question changed");
                code = AH_BUSY;
            } else {
                if (*action.text)
                    code = submit(app, action.text, &error);
                if (code == AH_OK)
                    tui_picker_close(app);
            }
            break;
        }
        code = app->pick_kind == TUI_PICK_MODELS || app->pick_kind == TUI_PICK_EFFORT
                   ? tui_models_choose(app, action.text, &error)
                   : tui_switch_session(app, action.text, &error);
        break;
    case TUI_PICK_APPROVE:
    case TUI_PICK_APPROVE_ALL:
    case TUI_PICK_DENY:
        if (!action.text || strcmp(action.text, app->operation)) {
            snprintf(error.message, sizeof(error.message), "Pending operation changed");
            code = AH_BUSY;
        } else {
            if (action.kind == TUI_PICK_APPROVE_ALL)
                set_yolo(app, true);
            code = decide_pending(app, action.kind != TUI_PICK_DENY, &error);
        }
        break;
    case TUI_PICK_NEW:
        code = tui_open_draft(app, app->default_model, "", &error);
        break;
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
                snprintf(app->auth_error, sizeof(app->auth_error), "%.*s",
                         (int)sizeof(app->auth_error) - 1,
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
