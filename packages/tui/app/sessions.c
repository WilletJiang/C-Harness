#include "app.h"
#include "internal.h"
#include "signals.h"
#include <string.h>
#include <utf8proc.h>

typedef struct {
    char first[192], latest[192], reply[240];
} session_preview;

static const char *field(yyjson_val *object, const char *key) {
    const char *s = yyjson_get_str(yyjson_obj_get(object, key));
    return s ? s : "";
}

static void excerpt(char *out, size_t cap, const char *text) {
    size_t len = strlen(text), used = 0;
    bool gap = false;
    for (size_t i = 0; i < len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + i, len - i, &cp);
        if (n < 0) {
            ++i;
            continue;
        }
        if (cp <= ' ' || (cp >= 0x7f && cp <= 0x9f)) {
            gap = used != 0;
        } else {
            if (used + gap + (size_t)n >= cap)
                break;
            if (gap)
                out[used++] = ' ';
            memcpy(out + used, text + i, (size_t)n);
            used += (size_t)n;
            gap = false;
        }
        i += (size_t)n;
    }
    out[used] = 0;
}

static void read_prompt(yyjson_val *root, char *out, size_t cap) {
    char summary[512];
    tui_input_summary(yyjson_obj_get(root, "content"), summary, sizeof(summary));
    excerpt(out, cap, summary);
}

static void first_event(void *context, const ah_event *event) {
    session_preview *preview = context;
    if (preview->first[0] || !event->json || strcmp(event->type, "input.submitted"))
        return;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    read_prompt(yyjson_doc_get_root(doc), preview->first, sizeof(preview->first));
    yyjson_doc_free(doc);
}

static void recent_event(void *context, const ah_event *event) {
    if (!event->json || (strcmp(event->type, "input.submitted") &&
                         strcmp(event->type, "model.completed")))
        return;
    session_preview *preview = context;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!strcmp(event->type, "input.submitted")) {
        read_prompt(root, preview->latest, sizeof(preview->latest));
    } else {
        size_t i, n, j, m;
        yyjson_val *item, *part;
        yyjson_arr_foreach(yyjson_obj_get(root, "output"), i, n, item) {
            if (strcmp(field(item, "type"), "message") || strcmp(field(item, "role"), "assistant"))
                continue;
            yyjson_arr_foreach(yyjson_obj_get(item, "content"), j, m, part) {
                const char *text = field(part, "text");
                if (*text)
                    excerpt(preview->reply, sizeof(preview->reply), text);
            }
        }
    }
    yyjson_doc_free(doc);
}

static bool needs_attention(const char *state) {
    return !strcmp(state, "waiting_approval") || !strcmp(state, "waiting_input") ||
           !strcmp(state, "outcome_unknown") || !strcmp(state, "stopped") || !strcmp(state, "failed");
}

static bool list_session(yyjson_val *item, const char *current) {
    return !strcmp(field(item, "id"), current) || strcmp(field(item, "state"), "idle") ||
           yyjson_get_uint(yyjson_obj_get(item, "turn")) ||
           yyjson_get_uint(yyjson_obj_get(item, "model_calls"));
}

typedef struct {
    tui_app *app;
    size_t selected;
    ah_code code;
    ah_error *error;
    bool partial;
} session_list;

static ah_code add_sessions(session_list *list, ah_reader *reader, const char *database,
                            ah_error *error) {
    tui_app *app = list->app;
    char *json = NULL;
    ah_code code = ah_reader_sessions_json(reader, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *sessions = yyjson_doc_get_root(doc);
    size_t count = 0, i, total;
    yyjson_val *item;
    yyjson_arr_foreach(sessions, i, total, item)
        count += list_session(item, app->session);
    size_t row = app->pick_count;
    if (!yyjson_is_arr(sessions) || !tui_picker_extend(app, count)) {
        yyjson_doc_free(doc);
        return ah_fail(error, AH_IO, "Cannot load the conversation list");
    }
    for (i = yyjson_arr_size(sessions); i > 0; --i) {
        item = yyjson_arr_get(sessions, i - 1);
        if (!list_session(item, app->session))
            continue;
        const char *id = field(item, "id"), *state = field(item, "state"), *model = field(item, "model");
        uint64_t turn = yyjson_get_uint(yyjson_obj_get(item, "turn"));
        uint64_t calls = yyjson_get_uint(yyjson_obj_get(item, "model_calls"));
        bool current = !strcmp(id, app->session);
        if (current)
            list->selected = row;
        size_t id_len = strlen(id);
        const char *short_id = id + (id_len > 8 ? id_len - 8 : 0);
        session_preview preview = {0};
        if (turn || calls) {
            uint64_t cursor;
            ah_error ignored = {0};
            (void)ah_reader_events(reader, id, 0, 16, first_event, &preview, &cursor, &ignored);
            (void)ah_reader_recent_events(reader, id, 0, 64, recent_event, &preview, &cursor, &ignored);
        }
        char fallback[40], meta[40], detail[800];
        snprintf(fallback, sizeof(fallback), "%s %.8s", turn || calls ? "Conversation" : "Empty chat", short_id);
        snprintf(meta, sizeof(meta), "%s", current ? "current" : short_id);
        const char *title = preview.first[0] ? preview.first : preview.latest[0] ? preview.latest : fallback;
        const char *last = preview.latest[0] ? preview.latest : preview.first;
        snprintf(detail, sizeof(detail), "Last message: %s\nLast reply: %s\n\nModel: %s\nTurns: %llu\nID: %s%s",
                 *last ? last : "No messages yet", preview.reply[0] ? preview.reply : "No reply yet",
                 *model ? model : "-", (unsigned long long)turn, id, current ? " (current)" : "");
        if (database && !(app->pick_strings[row].database = ah_strdup(database))) {
            yyjson_doc_free(doc);
            return ah_fail(error, AH_IO, "Cannot allocate the conversation location");
        }
        tui_picker_set(app, row++, id, title, state, meta, detail, 0, current,
                       needs_attention(state), !strcmp(state, "running"));
    }
    yyjson_doc_free(doc);
    return AH_OK;
}

static bool add_database(const char *path, void *context) {
    session_list *list = context;
    if (!strcmp(path, list->app->database))
        return true;
    ah_reader *reader = NULL;
    ah_error ignored = {0};
    size_t previous_count = list->app->pick_count;
    ah_code code = ah_reader_open_with_writer(path, &reader, &ignored);
    if (code == AH_OK)
        code = ah_reader_set_busy_timeout(reader, 50, &ignored);
    if (code == AH_OK)
        code = add_sessions(list, reader, path, &ignored);
    ah_reader_close(reader);
    if (code != AH_OK && list->app->pick_count != previous_count) {
        *list->error = ignored;
        list->code = code;
        return false;
    }
    list->partial |= code != AH_OK;
    return true;
}

ah_code tui_open_sessions_picker(tui_app *app, ah_error *error) {
    tui_picker_close(app);
    session_list list = {.app = app, .error = error};
    ah_code code = add_sessions(&list, app->reader, NULL, error);
    if (code == AH_OK && app->state_directory)
        code = tui_visit_databases(app->state_directory, add_database, &list, error);
    if (code == AH_OK)
        code = list.code;
    if (code != AH_OK) {
        tui_picker_close(app);
        return code;
    }
    app->pick_kind = TUI_PICK_SESSIONS;
    app->picker = (tui_picker){.open = true, .title = "Sessions",
                              .subtitle = list.partial ? "Some conversations are unavailable" :
                                          "Choose a conversation",
                              .can_open = true, .can_new = true, .items = app->pick_items,
                              .count = app->pick_count};
    tui_view_select_picker(app->view, list.selected);
    return AH_OK;
}

static void reset_conversation_view(tui_app *app) {
    app->ask_seen[0] = 0;
    app->reply_operation[0] = 0;
    if (strcmp(app->pending_session, app->session)) {
        app->pending_ask = false;
        app->pending_background = false;
        app->pending_session[0] = app->operation[0] = app->pending_status[0] = 0;
        app->pending_tool[0] = app->pending_summary[0] = 0;
    }
    app->active_tool[0] = 0;
    app->active_tools = 0;
    app->history = 0;
    app->restore_history = false;
    app->last_block = TUI_BLOCK_NONE;
    memset(app->tool_cache, 0, sizeof(app->tool_cache));
    app->notice[0] = 0;
    app->status.notice_error = false;
    if (app->image_id[0]) {
        tui_clear_image(app);
        snprintf(app->notice, sizeof(app->notice), "Image removed from previous conversation draft");
    }
    tui_picker_close(app);
    tui_print_banner(app);
}

ah_code tui_open_draft(tui_app *app, const char *model, const char *effort, ah_error *error) {
    if (app->running || app->catalog_loading)
        return ah_fail(error, AH_BUSY, "Available when execution pauses");
    app->session[0] = 0;
    app->turn = 0;
    snprintf(app->state, sizeof(app->state), "idle");
    snprintf(app->model, sizeof(app->model), "%s", model);
    snprintf(app->effort, sizeof(app->effort), "%s", effort);
    reset_conversation_view(app);
    return AH_OK;
}

static ah_code switch_database(tui_app *app, const char *database, const char *target,
                               ah_error *error) {
    char *path = ah_strdup(database);
    if (!path)
        return ah_fail(error, AH_IO, "Cannot allocate workspace database path");
    ah_engine *engine = NULL;
    ah_local_tools *tools = NULL;
    ah_reader *reader = NULL;
    ah_observer observer = app->cli->event_sink;
    app->cli->event_sink = NULL;
    ah_code code = app->open_engine(app->engine_context, path, &engine, &tools, error);
    app->cli->event_sink = observer;
    if (code == AH_BUSY)
        code = ah_fail(error, AH_BUSY, "Another window is using this conversation's database");
    if (code == AH_OK)
        code = ah_reader_open_with_writer(path, &reader, error);
    if (code == AH_OK)
        code = ah_reader_set_busy_timeout(reader, 0, error);
    if (code == AH_OK) {
        char previous[AH_ID_CAP];
        strcpy(previous, app->session);
        strcpy(app->session, target);
        ah_reader *old_reader = app->reader;
        app->reader = reader;
        code = tui_snapshot(app, app->session, true, error);
        if (code == AH_OK) {
            ah_set_approve_all(engine, app->approve_all);
            ah_local_tools_set_auto_approve(tools, ah_local_tools_auto_approve(app->cli->local_tools));
            app_signals_set_engine(engine);
            ah_close(app->cli->engine);
            ah_local_tools_close(app->cli->local_tools);
            ah_reader_close(old_reader);
            free(app->cli->owned_database);
            app->cli->engine = engine;
            app->cli->local_tools = tools;
            app->cli->owned_database = path;
            app->database = path;
            app->refresh_events = false;
            atomic_store(&app->journal_dirty, false);
            return AH_OK;
        }
        app->reader = old_reader;
        strcpy(app->session, previous);
    }
    ah_reader_close(reader);
    ah_close(engine);
    ah_local_tools_close(tools);
    free(path);
    return code;
}

static const char *picked_database(const tui_app *app, const char *id) {
    for (size_t i = 0; id && i < app->pick_count; i++) {
        const tui_pick_strings *item = &app->pick_strings[i];
        if (item->id && !strcmp(item->id, id))
            return item->database;
    }
    return NULL;
}

ah_code tui_switch_session(tui_app *app, const char *target, ah_error *error) {
    if (!target || !*target || strlen(target) >= sizeof(app->session))
        return ah_fail(error, AH_INVALID, "Invalid session ID");
    if (!strcmp(target, app->session)) {
        tui_picker_close(app);
        return AH_OK;
    }
    if (app->running || app->catalog_loading)
        return ah_fail(error, AH_BUSY, "Available when execution pauses");
    const char *database = picked_database(app, target);
    ah_code code;
    if (database) {
        code = switch_database(app, database, target, error);
    } else {
        char previous[AH_ID_CAP];
        strcpy(previous, app->session);
        strcpy(app->session, target);
        code = tui_snapshot(app, app->session, true, error);
        if (code != AH_OK)
            strcpy(app->session, previous);
    }
    if (code != AH_OK)
        return code;
    tui_models_sync(app);
    reset_conversation_view(app);
    (void)tui_history_page(app, app->cursor + 1, error);
    return AH_OK;
}
