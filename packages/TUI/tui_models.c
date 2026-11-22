#include "tui_app.h"
#include "internal.h"
#include <string.h>

enum { MODEL_ROWS = 256, EFFORT_ROWS = 12 };

static const char *field(yyjson_val *object, const char *key, size_t limit) {
    yyjson_val *value = yyjson_obj_get(object, key);
    const char *text = yyjson_get_str(value);
    return text && *text && strlen(text) == yyjson_get_len(value) && strlen(text) <= limit
               ? text : NULL;
}

static const char *model_id(yyjson_val *entry, bool chatgpt) {
    return field(entry, chatgpt ? "slug" : "id", 256);
}

static bool visible(yyjson_val *entry, bool chatgpt) {
    return model_id(entry, chatgpt) &&
           (!chatgpt || yyjson_equals_str(yyjson_obj_get(entry, "visibility"), "list"));
}

static yyjson_val *model_entries(tui_app *app) {
    return yyjson_doc_get_root(app->catalog);
}

static yyjson_doc *catalog_summary(yyjson_val *entries, bool chatgpt) {
    yyjson_mut_doc *summary = yyjson_mut_doc_new(NULL);
    if (!summary)
        return NULL;
    yyjson_mut_val *models = yyjson_mut_arr(summary);
    yyjson_mut_doc_set_root(summary, models);
    bool ok = models != NULL;
    size_t i, n;
    yyjson_val *entry;
    const char *const keys[] = {chatgpt ? "slug" : "id", "display_name",
                               "default_reasoning_level", "supported_reasoning_levels"};
    yyjson_arr_foreach(entries, i, n, entry) {
        if (!ok || yyjson_mut_arr_size(models) == MODEL_ROWS)
            break;
        if (!visible(entry, chatgpt))
            continue;
        yyjson_mut_val *model = yyjson_mut_arr_add_obj(summary, models);
        for (size_t key = 0; key < sizeof(keys) / sizeof(*keys); key++) {
            yyjson_val *value = yyjson_obj_get(entry, keys[key]);
            if (value && !yyjson_mut_obj_add_val(summary, model, keys[key],
                                                 yyjson_val_mut_copy(summary, value))) {
                ok = false;
                break;
            }
        }
    }
    yyjson_doc *result = ok ? yyjson_mut_doc_imut_copy(summary, NULL) : NULL;
    yyjson_mut_doc_free(summary);
    return result;
}

static void open_picker(tui_app *app, tui_pick_kind kind, const char *title, const char *subtitle) {
    app->pick_kind = kind;
    app->picker = (tui_picker){.open = true, .title = title, .subtitle = subtitle,
                              .can_open = true, .items = app->pick_items, .count = app->pick_count};
}

static ah_code model_picker(tui_app *app, ah_error *error) {
    yyjson_val *entries = model_entries(app), *entry;
    size_t i, n, count = yyjson_arr_size(entries);
    if (!count)
        return ah_fail(error, AH_NOT_FOUND, "No models listed. Enter a model ID in /config.");
    if (!tui_picker_alloc(app, count))
        return ah_fail(error, AH_IO, "Cannot allocate model picker");
    yyjson_arr_foreach(entries, i, n, entry) {
        const char *id = model_id(entry, app->chatgpt);
        const char *name = field(entry, "display_name", 120);
        const char *level = field(entry, "default_reasoning_level", 16);
        tui_picker_set(app, i, id, name ? name : id, NULL, level, id, 0,
                       !strcmp(id, app->model), false, false);
    }
    open_picker(app, TUI_PICK_MODELS, "Model", "Enter starts a new session");
    return AH_OK;
}

static ah_code effort_picker(tui_app *app, ah_error *error) {
    yyjson_val *entries = model_entries(app), *entry, *levels = NULL;
    const char *default_level = NULL;
    size_t i, n;
    yyjson_arr_foreach(entries, i, n, entry) {
        const char *id = model_id(entry, app->chatgpt);
        if (id && !strcmp(id, app->model)) {
            levels = yyjson_obj_get(entry, "supported_reasoning_levels");
            default_level = field(entry, "default_reasoning_level", 16);
            break;
        }
    }
    static const char *const fallback[] = {"none", "minimal", "low", "medium", "high", "xhigh", "max"};
    const char *ids[EFFORT_ROWS], *details[EFFORT_ROWS];
    size_t count = 0;
    yyjson_arr_foreach(levels, i, n, entry) {
        const char *id = field(entry, "effort", 16);
        if (!id || !strcmp(id, "default"))
            continue;
        ids[count] = id;
        details[count++] = field(entry, "description", 512);
        if (count == EFFORT_ROWS)
            break;
    }
    if (!yyjson_is_arr(levels)) {
        for (i = 0; i < sizeof(fallback) / sizeof(*fallback); i++) {
            ids[count] = fallback[i];
            details[count++] = "Availability depends on the provider.";
        }
    }
    char *effort = NULL;
    ah_code code = ah_session_reasoning(app->cli->engine, app->session, &effort, error);
    if (code != AH_OK)
        return code;
    if (!tui_picker_alloc(app, count + 1)) {
        ah_free(effort);
        return ah_fail(error, AH_IO, "Cannot allocate reasoning picker");
    }
    tui_picker_set(app, 0, "default", "Provider default", NULL, default_level, NULL, 0,
                   !*effort, false, false);
    for (i = 0; i < count; i++)
        tui_picker_set(app, i + 1, ids[i], ids[i], NULL, NULL, details[i], 0,
                       !strcmp(effort, ids[i]), false, false);
    ah_free(effort);
    open_picker(app, TUI_PICK_EFFORT, "Reasoning effort", "Applies from the next turn");
    return AH_OK;
}

void tui_models_sync(tui_app *app) {
    char *effort = NULL;
    ah_error ignored = {0};
    app->effort[0] = 0;
    if (ah_session_reasoning(app->cli->engine, app->session, &effort, &ignored) == AH_OK)
        snprintf(app->effort, sizeof(app->effort), "%s", effort);
    ah_free(effort);
}

static void *load_catalog(void *context) {
    tui_app *app = context;
    char *json = NULL;
    app->run_code = app->chatgpt ? tui_oauth_refresh(app->responses, &app->run_error) : AH_OK;
    if (app->run_code == AH_OK)
        app->run_code = ah_responses_models(app->responses, &json, &app->run_error);
    if (app->run_code == AH_OK) {
        yyjson_doc *raw = yyjson_read(json, strlen(json), 0);
        yyjson_val *entries = yyjson_obj_get(yyjson_doc_get_root(raw),
                                             app->chatgpt ? "models" : "data");
        if (!yyjson_is_arr(entries)) {
            app->run_code = ah_fail(&app->run_error, AH_PROTOCOL, "Invalid model catalog format");
        } else {
            app->catalog = catalog_summary(entries, app->chatgpt);
            if (!app->catalog)
                app->run_code = ah_fail(&app->run_error, AH_IO, "Cannot allocate model catalog");
        }
        yyjson_doc_free(raw);
    }
    ah_free(json);
    atomic_store(&app->worker_done, true);
    tui_notify(app->wake[1]);
    return NULL;
}

ah_code tui_models_open(tui_app *app, bool effort, ah_error *error) {
    if (effort && app->catalog)
        return effort_picker(app, error);
    yyjson_doc_free(app->catalog);
    app->catalog = NULL;
    app->catalog_for_effort = effort;
    atomic_store(&app->worker_done, false);
    if (pthread_create(&app->worker, NULL, load_catalog, app))
        return ah_fail(error, AH_IO, "Cannot start model catalog request");
    app->catalog_loading = true;
    app->status.notice_error = false;
    snprintf(app->notice, sizeof(app->notice), "Loading model catalog");
    return AH_OK;
}

void tui_models_ready(tui_app *app) {
    pthread_join(app->worker, NULL);
    app->catalog_loading = false;
    app->notice[0] = 0;
    ah_error error = {0};
    if (!app->catalog_for_effort && app->run_code != AH_OK) {
        ah_fail(&error, app->run_code, "%.400s. Enter a model ID in /config.", app->run_error.message);
        tui_show_error(app, error.code, &error);
        return;
    }
    ah_code code = app->catalog_for_effort ? effort_picker(app, &error) : model_picker(app, &error);
    if (code != AH_OK)
        tui_show_error(app, code, &error);
}

ah_code tui_models_choose(tui_app *app, const char *id, ah_error *error) {
    if (!id || !*id)
        return ah_fail(error, AH_INVALID, "Choose an item");
    if (app->pick_kind == TUI_PICK_EFFORT) {
        const char *effort = !strcmp(id, "default") ? "" : id;
        ah_code code = ah_session_set_reasoning(app->cli->engine, app->session, effort, error);
        if (code == AH_OK) {
            snprintf(app->effort, sizeof(app->effort), "%s", effort);
            tui_picker_close(app);
            app->status.notice_error = false;
            snprintf(app->notice, sizeof(app->notice), "Reasoning effort: %s", *effort ? effort : "provider default");
        }
        return code;
    }
    char model[257], session[AH_ID_CAP];
    snprintf(model, sizeof(model), "%s", id);
    ah_code code = ah_session_create(app->cli->engine, model, app->instructions, session, error);
    if (code == AH_OK)
        code = tui_switch_session(app, session, error);
    if (code == AH_OK && !tui_config_save_model(app, model)) {
        app->status.notice_error = true;
        snprintf(app->notice, sizeof(app->notice), "Cannot save the default model to %s",
                 app->config_display);
    }
    return code;
}
