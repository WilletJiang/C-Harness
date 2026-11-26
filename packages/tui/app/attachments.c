#include "app.h"
#include "core/content_internal.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *image_mime(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext)
        return NULL;
    if (!strcasecmp(ext, ".png"))
        return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))
        return "image/gif";
    return !strcasecmp(ext, ".webp") ? "image/webp" : NULL;
}

static char *image_path(const char *arg) {
    size_t len = strlen(arg);
    char quote = len > 1 && (arg[0] == '\'' || arg[0] == '"') && arg[0] == arg[len - 1]
                     ? arg[0] : 0;
    if (quote) {
        ++arg;
        len -= 2;
    }
    const char *home = len >= 2 && !strncmp(arg, "~/", 2) ? getenv("HOME") : NULL;
    size_t prefix = home ? strlen(home) : 0;
    char *path = malloc(prefix + len + 1);
    if (!path)
        return NULL;
    if (prefix) {
        memcpy(path, home, prefix);
        ++arg;
        --len;
    }
    size_t at = prefix;
    for (size_t i = 0; i < len; ++i) {
        if (!quote && arg[i] == '\\' && i + 1 < len && strchr(" \\'\"", arg[i + 1]))
            ++i;
        path[at++] = arg[i];
    }
    path[at] = 0;
    return path;
}

void tui_clear_image(tui_app *app) {
    if (app->draft_image) {
        core_content content = {.blobs = app->draft_image};
        core_content_free(&content);
        app->draft_image = NULL;
    }
    app->image_id[0] = app->image_name[0] = 0;
}

ah_code tui_image_command(tui_app *app, const char *arg, ah_error *error) {
    while (*arg == ' ' || *arg == '\t')
        ++arg;
    if (!strcmp(arg, "clear")) {
        tui_clear_image(app);
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "Image removed from draft");
        return AH_OK;
    }
    if (app->running || app->catalog_loading) {
        snprintf(error->message, sizeof(error->message), "Attach an image after the current run pauses");
        return AH_BUSY;
    }
    if (!*arg) {
        snprintf(error->message, sizeof(error->message), "Usage: /image PATH or /image clear");
        return AH_INVALID;
    }
    char *path = image_path(arg);
    if (!path)
        return AH_IO;
    const char *leaf = strrchr(path, '/');
    const char *name = leaf ? leaf + 1 : path;
    const char *mime = image_mime(name);
    ah_code code = AH_INVALID;
    if (!mime) {
        snprintf(error->message, sizeof(error->message), "Choose a PNG, JPEG, GIF, or WebP image");
    } else {
        char id[AH_ARTIFACT_ID_CAP];
        content_blob *draft = NULL;
        if (app->session[0]) {
            code = ah_artifact_import(app->cli->engine, app->session, path, mime, id, error);
        } else {
            draft = calloc(1, sizeof(*draft));
            code = draft ? content_artifact_read(path, mime, draft, error)
                         : ah_fail(error, AH_IO, "Cannot allocate image draft");
        }
        if (code == AH_OK) {
            tui_clear_image(app);
            app->draft_image = draft;
            strcpy(app->image_id, draft ? draft->id : id);
            // On filesystems with a 255-byte component limit, this keeps the entire name.
            snprintf(app->image_name, sizeof(app->image_name), "%s", name);
            app->status.notice_error = false;
            app->notice[0] = 0;
        } else
            free(draft);
    }
    free(path);
    return code;
}

static char *image_input(const char *text, const char *image_id) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *parts = doc ? yyjson_mut_arr(doc) : NULL;
    yyjson_mut_val *message = parts && *text ? yyjson_mut_arr_add_obj(doc, parts) : NULL;
    bool ok = parts && (!*text || (message && yyjson_mut_obj_add_str(doc, message, "type", "input_text") &&
                                   yyjson_mut_obj_add_str(doc, message, "text", text)));
    yyjson_mut_val *image = ok ? yyjson_mut_arr_add_obj(doc, parts) : NULL;
    ok = image && yyjson_mut_obj_add_str(doc, image, "type", "input_image") &&
         yyjson_mut_obj_add_str(doc, image, "artifact_id", image_id);
    char *json = NULL;
    if (ok) {
        yyjson_mut_doc_set_root(doc, parts);
        json = yyjson_mut_write(doc, 0, NULL);
    }
    yyjson_mut_doc_free(doc);
    return json;
}

ah_code tui_submit_input(tui_app *app, const char *command, const char *text, ah_error *error) {
    char *json = app->image_id[0] ? image_input(text, app->image_id) : NULL;
    if (app->image_id[0] && !json)
        return ah_fail(error, AH_IO, "Cannot prepare image input");
    ah_engine *engine = app->cli->engine;
    ah_code code;
    if (!app->session[0]) {
        code = core_submit_first(engine, app->model, app->instructions, app->effort, command,
                                 json ? json : text, app->draft_image, app->session,
                                 &app->cursor, error);
        if (code == AH_OK)
            app->turn = 1;
    } else if (json) {
        code = ah_submit_content_after_interruption(engine, app->session, command, json, error);
    } else {
        code = ah_submit_after_interruption(engine, app->session, command, text, error);
    }
    free(json);
    if (code == AH_OK)
        tui_clear_image(app);
    return code;
}

void tui_input_summary(yyjson_val *content, char *out, size_t cap) {
    if (!cap)
        return;
    const char *plain = yyjson_get_str(content);
    if (plain) {
        snprintf(out, cap, "%s", plain);
        return;
    }
    out[0] = 0;
    size_t i, n, used = 0;
    yyjson_val *part;
    yyjson_arr_foreach(content, i, n, part) {
        yyjson_val *type = yyjson_obj_get(part, "type");
        const char *text = yyjson_equals_str(type, "input_text")
                               ? yyjson_get_str(yyjson_obj_get(part, "text"))
                           : yyjson_equals_str(type, "input_image") ? "[Image]"
                           : yyjson_equals_str(type, "input_file") ? "[File]" : NULL;
        if (!text || !*text)
            continue;
        int wrote = snprintf(out + used, cap - used, "%s%s", used ? "\n" : "", text);
        if (wrote < 0 || (size_t)wrote >= cap - used)
            break;
        used += (size_t)wrote;
    }
}
