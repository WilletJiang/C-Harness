#include "tui_app.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define T_RST      "\033[0m"
#define T_BOLD     "\033[1m"
#define T_TEXT     "\033[38;5;252m"
#define T_DIM      "\033[38;5;244m"
#define T_FAINT    "\033[38;5;239m"
#define T_ACCENT   "\033[38;5;75m"
#define T_GOLD     "\033[38;5;180m"
#define T_LAVENDER "\033[38;5;146m"
#define T_GREEN    "\033[38;5;114m"
#define T_AMBER    "\033[38;5;215m"
#define T_RED      "\033[38;5;203m"

static const char *field(yyjson_val *value, const char *key) {
    const char *text = yyjson_get_str(yyjson_obj_get(value, key));
    return text ? text : "";
}

static bool type_is(const ah_event *event, const char *type) {
    return !strcmp(event->type, type);
}

static void format_clock(char out[16]) {
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm))
        snprintf(out, 16, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else
        out[0] = 0;
}

void tui_print_banner(tui_app *app) {
    tui_terminal_clear(&app->tty);
    unsigned width = app->cols > 4 ? app->cols - 2 : 60;
    if (width > 76)
        width = 76;
    fputs("\n", stdout);
    fputs(" " T_FAINT "\xe2\x95\xad\xe2\x94\x80 " T_ACCENT ">_" T_BOLD T_TEXT " ah " T_RST T_DIM
          "v" AH_VERSION " " T_FAINT, stdout);
    size_t head_len = 11 + strlen(AH_VERSION);
    for (unsigned i = (unsigned)head_len; i + 1 < width; ++i)
        fputs("\xe2\x94\x80", stdout);
    fputs(T_RST "\n", stdout);

    if (app->model[0]) {
        fputs(" " T_FAINT "\xe2\x94\x82  " T_DIM "model    " T_GOLD, stdout);
        tui_safe_text(stdout, app->model, strlen(app->model));
        fputs(T_RST "\n", stdout);
    }
    fputs(" " T_FAINT "\xe2\x94\x82  " T_DIM "session  " T_TEXT, stdout);
    tui_safe_text(stdout, app->session, strlen(app->session));
    fputs(T_RST "\n", stdout);
    if (app->cwd[0]) {
        fputs(" " T_FAINT "\xe2\x94\x82  " T_DIM "dir      " T_TEXT, stdout);
        tui_safe_text(stdout, app->cwd, strlen(app->cwd));
        if (app->branch[0]) {
            fputs(T_FAINT "  \xc2\xb7  " T_LAVENDER, stdout);
            tui_safe_text(stdout, app->branch, strlen(app->branch));
        }
        fputs(T_RST "\n", stdout);
    }
    fputs(" " T_FAINT "\xe2\x95\xb0", stdout);
    for (unsigned i = 1; i < width; ++i)
        fputs("\xe2\x94\x80", stdout);
    fputs(T_RST "\n\n", stdout);
    fflush(stdout);
}

void tui_print_worked(tui_app *app, uint64_t elapsed_ms) {
    tui_terminal_clear(&app->tty);
    char clock_buf[16];
    format_clock(clock_buf);
    uint64_t s = elapsed_ms / 1000;
    if (s < 60)
        fprintf(stdout, "  " T_DIM "Worked for %us  \xc2\xb7  %s" T_RST "\n\n", (unsigned)s,
                clock_buf);
    else
        fprintf(stdout, "  " T_DIM "Worked for %um %02us  \xc2\xb7  %s" T_RST "\n\n",
                (unsigned)(s / 60), (unsigned)(s % 60), clock_buf);
    fflush(stdout);
}

void tui_observe(void *context, const ah_event *event) {
    tui_app *app = context;
    bool root = event->session_id && !strcmp(event->session_id, app->session);
    if (root)
        tui_stream_observe(app, event);
    if (event->seq)
        atomic_store(&app->journal_dirty, true);
    if (event->seq || (root && type_is(event, "model.delta")))
        tui_notify(app->wake[1]);
}

void tui_show_error(tui_app *app, ah_code code, const ah_error *error) {
    app->status.notice_error = true;
    snprintf(app->notice, sizeof(app->notice), "%s%s%s", ah_code_name(code),
             error->message[0] ? "  \xc2\xb7  " : "", error->message);
}

void tui_line(tui_app *app, const char *prefix, const char *text) {
    tui_terminal_clear(&app->tty);
    if (!strcmp(prefix, "> ")) {
        char clock_buf[16];
        format_clock(clock_buf);
        fputs(T_ACCENT " \xe2\x80\xba " T_BOLD T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        if (clock_buf[0])
            fprintf(stdout, T_RST "  " T_FAINT "%s" T_RST "\n\n", clock_buf);
        else
            fputs(T_RST "\n\n", stdout);
    } else if (!strcmp(prefix, "+ ")) {
        fputs("  " T_DIM "\xe2\x86\xb3 " T_GOLD, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    } else if (!strcmp(prefix, "! ")) {
        fputs(" " T_AMBER "\xe2\x96\x8e " T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    } else {
        fputs("  " T_DIM, stdout);
        fputs(prefix, stdout);
        fputs(T_TEXT, stdout);
        tui_safe_text(stdout, text, strlen(text));
        fputs(T_RST "\n", stdout);
    }
    fflush(stdout);
}

void tui_show_json(tui_app *app, const char *json) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    char *pretty = doc ? yyjson_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, NULL) : NULL;
    const char *src = pretty ? pretty : (json ? json : "");
    tui_terminal_clear(&app->tty);
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        fputs("  " T_FAINT "\xe2\x94\x82 " T_DIM, stdout);
        tui_safe_text(stdout, p, len);
        fputs(T_RST "\n", stdout);
        if (!nl)
            break;
        p = nl + 1;
    }
    fputc('\n', stdout);
    fflush(stdout);
    free(pretty);
    yyjson_doc_free(doc);
}

static void transcript(tui_app *app, const ah_event *event, bool live) {
    bool response = type_is(event, "model.completed");
    bool input = type_is(event, "input.submitted");
    bool tool = type_is(event, "tool.intent");
    if (!response && !input && !tool)
        return;
    yyjson_doc *doc = event->json ? yyjson_read(event->json, strlen(event->json), 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (input) {
        yyjson_val *content = yyjson_obj_get(root, "content");
        const char *text = yyjson_get_str(content);
        tui_line(app, "> ", text ? text : "[typed input]");
    } else if (tool) {
        tui_line(app, "+ ", field(root, "name"));
    } else {
        tui_stream_commit(app, yyjson_obj_get(root, "output"), live);
    }
    yyjson_doc_free(doc);
}

static void history(void *context, const ah_event *event) {
    transcript(context, event, false);
}

static void journal(void *context, const ah_event *event) {
    tui_app *app = context;
    bool root = event->session_id && !strcmp(event->session_id, app->session);
    if (root) {
        if (type_is(event, "model.started"))
            app->stream_active = event->seq;
        else if (type_is(event, "model.interrupted"))
            tui_stream_abort(app, "Response interrupted by steering");
        transcript(app, event, true);
    }
    if (event->session_id && event->operation_id &&
        (type_is(event, "approval.requested") || type_is(event, "input.requested"))) {
        snprintf(app->pending_session, sizeof(app->pending_session), "%s", event->session_id);
        snprintf(app->operation, sizeof(app->operation), "%s", event->operation_id);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "%s  \xc2\xb7  type /pending to review",
                 event->type);
    }
    if (root && type_is(event, "input.steered"))
        tui_line(app, "> ", "steering delivered");
    else if (root && type_is(event, "input.rejected"))
        tui_line(app, "! ", "steering rejected");
}

ah_code tui_snapshot(tui_app *app, const char *session, bool set_cursor, ah_error *error) {
    char *json = NULL;
    ah_code code = ah_reader_session_json(app->reader, session, &json, error);
    if (code != AH_OK)
        return code;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ah_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        error->code = AH_IO;
        snprintf(error->message, sizeof(error->message), "Cannot decode session snapshot");
        return AH_IO;
    }
    if (!strcmp(session, app->session)) {
        snprintf(app->state, sizeof(app->state), "%s", field(root, "state"));
        const char *mdl = field(root, "model");
        if (*mdl)
            snprintf(app->model, sizeof(app->model), "%s", mdl);
        app->turn = (unsigned)yyjson_get_uint(yyjson_obj_get(root, "turn"));
    }
    if (set_cursor)
        app->cursor = yyjson_get_uint(yyjson_obj_get(root, "cursor"));
    yyjson_val *pending = yyjson_obj_get(root, "pending");
    const char *status = field(pending, "status");
    if (!strcmp(status, "approval") || !strcmp(status, "input")) {
        snprintf(app->pending_session, sizeof(app->pending_session), "%s", session);
        snprintf(app->operation, sizeof(app->operation), "%s", field(pending, "operation_id"));
        snprintf(app->pending_status, sizeof(app->pending_status), "%s", status);
        app->pending_ask = !strcmp(status, "input") &&
                           tui_ask_has_options(yyjson_obj_get(pending, "arguments"));
        const char *question = field(yyjson_obj_get(pending, "arguments"), "question");
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Pending %s  \xc2\xb7  %s%s%.560s  (/pending)",
                 status, field(pending, "name"), *question ? ": " : "", question);
    } else if (!strcmp(session, app->pending_session)) {
        app->pending_ask = false;
        app->operation[0] = app->pending_status[0] = app->pending_session[0] = 0;
    }
    yyjson_doc_free(doc);
    return AH_OK;
}

ah_code tui_history_page(tui_app *app, uint64_t boundary, ah_error *error) {
    uint64_t first = 0;
    ah_code code = ah_reader_recent_events(app->reader, app->session, boundary, HISTORY_PAGE,
                                           history, app, &first, error);
    if (code == AH_OK)
        app->history = first;
    return code;
}

void tui_restore_history(tui_app *app) {
    if (!app->restore_history)
        return;
    app->restore_history = false;
    ah_error error = {0};
    ah_code code = tui_history_page(app, app->cursor + 1, &error);
    if (code != AH_OK)
        tui_show_error(app, code, &error);
}

void tui_refresh(tui_app *app) {
    uint64_t last;
    ah_error error = {0};
    ah_code code = ah_reader_events(app->reader, NULL, app->cursor, LIVE_PAGE, journal, app, &last,
                                    &error);
    app->refresh_events = code == AH_BUSY || (code == AH_OK && last > app->cursor);
    if (code == AH_LIMIT) {
        tui_show_error(app, code, &error);
        (void)tui_snapshot(app, app->session, true, &error);
    } else if (code != AH_OK && code != AH_BUSY) {
        tui_show_error(app, code, &error);
    } else if (code == AH_OK) {
        app->cursor = last;
    }
}
