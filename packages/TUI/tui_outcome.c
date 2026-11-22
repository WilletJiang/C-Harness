#include "tui_app.h"
#include "internal.h"
#include <string.h>

static void describe_evidence(const char *json, char detail[512]) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *reason = yyjson_get_str(yyjson_obj_get(root, "error"));
    if (!reason)
        reason = yyjson_get_str(yyjson_obj_get(root, "reason"));
    yyjson_val *signal = yyjson_obj_get(root, "signal");
    yyjson_val *exit_code = yyjson_obj_get(root, "exit_code");
    if (yyjson_is_int(exit_code))
        snprintf(detail, 512, "Exit code %lld\n%.400s", (long long)yyjson_get_sint(exit_code),
                 reason ? reason : "");
    else if (yyjson_is_int(signal))
        snprintf(detail, 512, "Terminated by signal %lld before an exit code was recorded\n%.400s",
                 (long long)yyjson_get_sint(signal), reason ? reason : "");
    else
        snprintf(detail, 512, "%s", reason ? reason : "Partial evidence is available in the journal.");
    yyjson_doc_free(doc);
}

ah_code tui_outcome_open(tui_app *app, const char *session, const char *operation, const char *tool,
                         ah_error *error) {
    if (!*session || strlen(session) >= sizeof(app->outcome_session) ||
        !*operation || strlen(operation) >= sizeof(app->outcome_operation))
        return ah_fail(error, AH_INVALID, "Pending session or operation is unavailable");
    char *json = NULL;
    ah_code code = ah_reader_unknown_json(app->reader, session, operation, &json, error);
    bool has_evidence = code == AH_OK;
    char evidence[512] = "Verify the actual result before recording it.";
    if (has_evidence)
        describe_evidence(json, evidence);
    else if (code != AH_NOT_FOUND && code != AH_LIMIT)
        return code;
    ah_free(json);
    if (!tui_picker_alloc(app, has_evidence ? 3 : 2))
        return ah_fail(error, AH_IO, "Cannot allocate outcome picker");

    char detail[680];
    snprintf(detail, sizeof(detail), "Tool: %.96s\nOperation: %s\n%s", *tool ? tool : "tool",
             operation, evidence);
    size_t row = 0;
    if (has_evidence)
        tui_picker_set(app, row++, "evidence", "View saved evidence", NULL, NULL, detail,
                       0, false, false, false);
    tui_picker_set(app, row++, "failure", "Record failed or interrupted", NULL, NULL, detail,
                   0, false, false, false);
    tui_picker_set(app, row, "success", "Record succeeded", NULL, NULL, detail,
                   0, false, false, false);
    snprintf(app->outcome_session, sizeof(app->outcome_session), "%s", session);
    snprintf(app->outcome_operation, sizeof(app->outcome_operation), "%s", operation);
    app->pick_kind = TUI_PICK_OUTCOME;
    app->picker = (tui_picker){
        .open = true, .title = "Tool outcome", .subtitle = has_evidence ? "Review saved evidence first"
                                                                     : "Enter the verified outcome",
        .can_open = true, .items = app->pick_items, .count = app->pick_count,
    };
    tui_view_select_picker(app->view, 0);
    ah_error_clear(error);
    return AH_OK;
}

ah_code tui_outcome_choose(tui_app *app, const char *id, ah_error *error) {
    if (!id)
        return ah_fail(error, AH_INVALID, "Choose an outcome");
    if (!strcmp(id, "evidence")) {
        char *json = NULL;
        ah_code code = ah_reader_unknown_json(app->reader, app->outcome_session,
                                             app->outcome_operation, &json, error);
        if (code == AH_OK) {
            tui_picker_close(app);
            tui_show_json(app, json);
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Use /pending to record the outcome");
        }
        ah_free(json);
        return code;
    }
    if (app->running || app->catalog_loading)
        return ah_fail(error, AH_BUSY, "Available when execution pauses");
    bool failed = !strcmp(id, "failure");
    if (!failed && strcmp(id, "success"))
        return ah_fail(error, AH_INVALID, "Choose an outcome");
    tui_picker_close(app);
    app->outcome_failed = failed;
    app->recording_outcome = true;
    app->status.notice_error = false;
    snprintf(app->notice, sizeof(app->notice), "Describe the %s result and press Enter. Esc cancels.",
             failed ? "failure or interruption" : "successful");
    return AH_OK;
}

ah_code tui_outcome_record(tui_app *app, const char *description, ah_error *error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *value = doc ? yyjson_mut_obj(doc) : NULL;
    bool encoded = value && yyjson_mut_obj_add_bool(doc, value, "ok", !app->outcome_failed) &&
                   yyjson_mut_obj_add_str(doc, value, "summary", description);
    if (encoded)
        yyjson_mut_doc_set_root(doc, value);
    char *json = encoded ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json)
        return ah_fail(error, AH_IO, "Cannot encode outcome");
    ah_code code = ah_resolve(app->cli->engine, app->outcome_session, app->outcome_operation,
                              json, app->outcome_failed, error);
    ah_free(json);
    if (code != AH_OK)
        return code;
    app->recording_outcome = false;
    code = tui_snapshot(app, app->outcome_session, false, error);
    app->status.notice_error = code != AH_OK;
    snprintf(app->notice, sizeof(app->notice), "%s",
             code != AH_OK ? "Outcome recorded. Use /pending to refresh."
             : !strcmp(app->pending_status, "unknown") ? "Outcome recorded. Use /pending for the next result."
                                                       : "Outcome recorded. Use /resume to continue.");
    ah_error_clear(error);
    return AH_OK;
}
