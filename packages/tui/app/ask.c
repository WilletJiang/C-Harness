#include "app.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

static const char *option(yyjson_val *value) {
    const char *text = yyjson_get_str(value);
    return text && *text && strlen(text) == yyjson_get_len(value) ? text : NULL;
}

bool tui_ask_has_options(yyjson_val *arguments) {
    yyjson_val *choices = yyjson_obj_get(arguments, "options"), *choice;
    size_t i, n;
    yyjson_arr_foreach(choices, i, n, choice) {
        if (option(choice))
            return true;
    }
    return false;
}

ah_code tui_ask_picker(tui_app *app, const char *operation, yyjson_val *arguments,
                       ah_error *error) {
    const char *question = option(yyjson_obj_get(arguments, "question"));
    yyjson_val *choices = yyjson_obj_get(arguments, "options"), *choice;
    size_t i, n, count = 0;
    yyjson_arr_foreach(choices, i, n, choice) {
        if (option(choice))
            ++count;
    }
    if (!tui_picker_alloc(app, count + 1))
        return ah_fail(error, AH_IO, "Cannot allocate reply choices");
    app->ask_prompt = ah_strdup(question ? question : "");
    if (!app->ask_prompt) {
        tui_picker_close(app);
        return ah_fail(error, AH_IO, "Cannot copy agent question");
    }
    size_t row = 0;
    yyjson_arr_foreach(choices, i, n, choice) {
        const char *text = option(choice);
        if (text)
            tui_picker_set(app, row++, text, text, NULL, NULL, NULL, 0, false, false, false);
    }
    tui_picker_set(app, row, "", "Type a reply", NULL, NULL, NULL, 0, false, false, false);
    snprintf(app->ask_seen, sizeof(app->ask_seen), "%s", operation);
    app->pick_kind = TUI_PICK_ASK;
    app->picker = (tui_picker){
        .open = true, .title = "Agent question", .question = app->ask_prompt,
        .can_open = true, .items = app->pick_items, .count = app->pick_count,
    };
    tui_view_select_picker(app->view, 0);
    return AH_OK;
}

void tui_ask_maybe_open(tui_app *app) {
    if (!app->pending_ask || strcmp(app->pending_status, "input") || !app->operation[0] ||
        !strcmp(app->ask_seen, app->operation) ||
        app->picker.open || app->auth.open || app->running || app->catalog_loading)
        return;
    snprintf(app->ask_seen, sizeof(app->ask_seen), "%s", app->operation);
    ah_error error = {0};
    ah_code code = tui_open_pending_picker(app, &error);
    if (code != AH_OK)
        tui_show_error(app, code, &error);
}
