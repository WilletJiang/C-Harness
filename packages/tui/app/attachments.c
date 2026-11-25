#include "app.h"
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

ah_code tui_image_command(tui_app *app, const char *arg, ah_error *error) {
    while (*arg == ' ' || *arg == '\t')
        ++arg;
    if (!strcmp(arg, "clear")) {
        app->image_id[0] = app->image_name[0] = 0;
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
        code = ah_artifact_import(app->cli->engine, app->session, path, mime, id, error);
        if (code == AH_OK) {
            strcpy(app->image_id, id);
            // On filesystems with a 255-byte component limit, this keeps the entire name.
            snprintf(app->image_name, sizeof(app->image_name), "%s", name);
            app->status.notice_error = false;
            app->notice[0] = 0;
        }
    }
    free(path);
    return code;
}

ah_code tui_submit_image(tui_app *app, const char *command, const char *text, ah_error *error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *parts = doc ? yyjson_mut_arr(doc) : NULL;
    yyjson_mut_val *message = parts && *text ? yyjson_mut_arr_add_obj(doc, parts) : NULL;
    bool ok = parts && (!*text || (message && yyjson_mut_obj_add_str(doc, message, "type", "input_text") &&
                                   yyjson_mut_obj_add_str(doc, message, "text", text)));
    yyjson_mut_val *image = ok ? yyjson_mut_arr_add_obj(doc, parts) : NULL;
    ok = image && yyjson_mut_obj_add_str(doc, image, "type", "input_image") &&
         yyjson_mut_obj_add_str(doc, image, "artifact_id", app->image_id);
    char *json = NULL;
    if (ok) {
        yyjson_mut_doc_set_root(doc, parts);
        json = yyjson_mut_write(doc, 0, NULL);
    }
    ah_code code = json ? ah_submit_content_after_interruption(app->cli->engine, app->session,
                                                               command, json, error)
                        : AH_IO;
    free(json);
    yyjson_mut_doc_free(doc);
    if (code == AH_OK)
        app->image_id[0] = app->image_name[0] = 0;
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
