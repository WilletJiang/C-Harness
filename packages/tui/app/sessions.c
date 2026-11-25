#include "app.h"
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

ah_code tui_open_sessions_picker(tui_app *app, ah_error *error) {
    char *json = NULL;
    ah_code code = ah_sessions_json(app->cli->engine, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *sessions = yyjson_doc_get_root(doc);
    size_t count = 0, i, total;
    yyjson_val *item;
    yyjson_arr_foreach(sessions, i, total, item)
        count += list_session(item, app->session);
    if (!yyjson_is_arr(sessions) || !tui_picker_alloc(app, count)) {
        yyjson_doc_free(doc);
        snprintf(error->message, sizeof(error->message), "Cannot load the conversation list");
        return AH_IO;
    }
    size_t selected = 0, row = 0;
    for (i = yyjson_arr_size(sessions); i > 0; --i) {
        item = yyjson_arr_get(sessions, i - 1);
        if (!list_session(item, app->session))
            continue;
        const char *id = field(item, "id"), *state = field(item, "state"), *model = field(item, "model");
        uint64_t turn = yyjson_get_uint(yyjson_obj_get(item, "turn"));
        uint64_t calls = yyjson_get_uint(yyjson_obj_get(item, "model_calls"));
        bool current = !strcmp(id, app->session);
        if (current)
            selected = row;
        size_t id_len = strlen(id);
        const char *short_id = id + (id_len > 8 ? id_len - 8 : 0);
        session_preview preview = {0};
        if (turn || calls) {
            uint64_t cursor;
            ah_error ignored = {0};
            (void)ah_reader_events(app->reader, id, 0, 16, first_event, &preview, &cursor, &ignored);
            (void)ah_reader_recent_events(app->reader, id, 0, 64, recent_event, &preview, &cursor, &ignored);
        }
        char fallback[40], meta[40], detail[800];
        snprintf(fallback, sizeof(fallback), "%s %.8s", turn || calls ? "Conversation" : "Empty chat", short_id);
        snprintf(meta, sizeof(meta), "%s", current ? "current" : short_id);
        const char *title = preview.first[0] ? preview.first : preview.latest[0] ? preview.latest : fallback;
        const char *last = preview.latest[0] ? preview.latest : preview.first;
        snprintf(detail, sizeof(detail), "Last message: %s\nLast reply: %s\n\nModel: %s\nTurns: %llu\nID: %s%s",
                 *last ? last : "No messages yet", preview.reply[0] ? preview.reply : "No reply yet",
                 *model ? model : "-", (unsigned long long)turn, id, current ? " (current)" : "");
        tui_picker_set(app, row++, id, title, state, meta, detail, 0, current,
                       needs_attention(state), !strcmp(state, "running"));
    }
    yyjson_doc_free(doc);
    app->pick_kind = TUI_PICK_SESSIONS;
    app->picker = (tui_picker){.open = true, .title = "Sessions", .subtitle = "Choose a conversation",
                              .can_open = true, .can_new = true, .items = app->pick_items,
                              .count = app->pick_count};
    tui_view_select_picker(app->view, selected);
    return AH_OK;
}
