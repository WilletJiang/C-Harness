#include "tui/app/app.h"
#include "internal.h"
#include "providers/http.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t capacity;
    yyjson_doc *doc;
} profile;

static void wipe(char *data, size_t size) {
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (size--)
        *bytes++ = 0;
}

static void close_profile(profile *p) {
    yyjson_doc_free(p->doc);
    if (p->data)
        wipe(p->data, p->capacity);
    free(p->data);
}

static profile read_profile(const char *path) {
    profile p = {0};
    if (!*path)
        return p;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return p;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077) ||
        st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return p;
    }
    size_t size = (size_t)st.st_size, used = 0;
    p.capacity = size + YYJSON_PADDING_SIZE;
    p.data = calloc(p.capacity, 1);
    while (p.data && used < size) {
        ssize_t n = read(fd, p.data + used, size - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    close(fd);
    if (p.data && used == size)
        p.doc = yyjson_read(p.data, size, YYJSON_READ_INSITU);
    return p;
}

static const char *profile_string(const profile *p, const char *name) {
    yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(p->doc), name);
    const char *s = yyjson_get_str(v);
    return s && *s && strlen(s) == yyjson_get_len(v) ? s : NULL;
}

void tui_config_init_paths(tui_app *app) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int n = 0;
    if (xdg && *xdg == '/') {
        n = snprintf(app->config_path, sizeof(app->config_path), "%s/ah/tui.json", xdg);
        snprintf(app->config_display, sizeof(app->config_display), "$XDG_CONFIG_HOME/ah/tui.json");
    } else if (home && *home == '/') {
        n = snprintf(app->config_path, sizeof(app->config_path), "%s/.config/ah/tui.json", home);
        snprintf(app->config_display, sizeof(app->config_display), "~/.config/ah/tui.json");
    }
    if (n <= 0 || (size_t)n >= sizeof(app->config_path)) {
        app->config_path[0] = 0;
        snprintf(app->config_display, sizeof(app->config_display), "(this run only)");
    }
}

void tui_config_load(tui_app *app) {
    profile saved = read_profile(app->config_path);
    const char *url = profile_string(&saved, "endpoint");
    const char *model = profile_string(&saved, "model");
    const char *token = profile_string(&saved, "access_token");
    const char *provider = profile_string(&saved, "provider");
    bool same = url && !strcmp(url, app->endpoint);
    ah_error ignored;
    if (url && !same && !app->endpoint_explicit && !app->has_token &&
        strlen(url) < sizeof(app->endpoint) &&
        ah_responses_set_endpoint(app->responses, url, &ignored) == AH_OK) {
        strcpy(app->endpoint, url);
        same = true;
    }
    if (same) {
        if (!app->default_model[0] && model && strlen(model) < sizeof(app->default_model))
            strcpy(app->default_model, model);
        if (!app->has_token && !strcmp(app->endpoint, TUI_CHATGPT_ENDPOINT) &&
            (!provider || !strcmp(provider, "chatgpt")))
            app->chatgpt = true;
        if (!app->chatgpt && !app->has_token && token &&
            ah_responses_set_token(app->responses, token, &ignored) == AH_OK) {
            app->has_token = true;
            strcpy(app->token_source, "saved");
        }
    }
    close_profile(&saved);
}

static bool make_profile_directory(const char *path) {
    char parent[512];
    strcpy(parent, path);
    char *leaf = strrchr(parent, '/');
    if (!leaf)
        return false;
    *leaf = 0;
    for (char *p = parent + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            int rc = mkdir(parent, 0700);
            *p = '/';
            if (rc && errno != EEXIST)
                return false;
        }
    }
    if (mkdir(parent, 0700) && errno != EEXIST)
        return false;
    struct stat st;
    return !lstat(parent, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
           !(st.st_mode & 0022);
}

static bool write_profile(const tui_app *app, const char *url, const char *token,
                          const char *model, bool chatgpt) {
    if (!app->config_path[0])
        return true;
    profile saved = {0};
    if (!chatgpt && !token) {
        saved = read_profile(app->config_path);
        const char *previous = profile_string(&saved, "endpoint");
        if (previous && !strcmp(previous, url))
            token = profile_string(&saved, "access_token");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    size_t length = 0;
    char *json = NULL;
    if (root && yyjson_mut_obj_add_str(doc, root, "endpoint", url) &&
        yyjson_mut_obj_add_str(doc, root, "provider", chatgpt ? "chatgpt" : "api") &&
        yyjson_mut_obj_add_str(doc, root, "model", model) &&
        (!token || yyjson_mut_obj_add_str(doc, root, "access_token", token))) {
        yyjson_mut_doc_set_root(doc, root);
        json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
    }
    yyjson_mut_doc_free(doc);
    close_profile(&saved);
    if (!json)
        return false;
    char temp[540];
    snprintf(temp, sizeof(temp), "%s.XXXXXX", app->config_path);
    int fd = make_profile_directory(app->config_path) ? mkstemp(temp) : -1;
    bool ok = fd >= 0 && fchmod(fd, 0600) == 0;
    size_t used = 0;
    while (ok && used < length) {
        ssize_t n = write(fd, json + used, length - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            ok = false;
        else
            used += (size_t)n;
    }
    if (fd >= 0) {
        if (ok && fsync(fd))
            ok = false;
        if (close(fd))
            ok = false;
        if (ok && rename(temp, app->config_path))
            ok = false;
        if (!ok)
            (void)unlink(temp);
    }
    wipe(json, length);
    free(json);
    return ok;
}

bool tui_config_save_model(tui_app *app, const char *model) {
    snprintf(app->default_model, sizeof(app->default_model), "%s", model);
    if (!app->chatgpt && (app->endpoint_explicit || !strcmp(app->token_source, "env") ||
                         !strcmp(app->token_source, "file")))
        return true;
    return write_profile(app, app->endpoint, NULL, model, app->chatgpt);
}

void tui_auth_open(tui_app *app, bool onboarding) {
    tui_picker_close(app);
    app->auth_error[0] = 0;
    app->auth = (tui_auth_state){
        .open = true,
        .onboarding = onboarding,
        .has_token = app->has_token,
        .chatgpt = app->chatgpt ||
                   (!app->has_token && !strcmp(app->endpoint, TUI_CHATGPT_ENDPOINT)),
        .token_source = app->token_source,
        .endpoint = app->endpoint,
        .model = app->default_model,
        .config_path = app->config_display,
        .error = app->auth_error,
    };
}

void tui_auth_close(tui_app *app) {
    app->auth.open = false;
    app->auth_error[0] = 0;
    tui_view_wipe_auth(app->view);
}

static bool normalize_url(const char *input, char result[512]) {
    if (!input || !*input || strlen(input) >= 512)
        return false;
    const char *authority = strstr(input, "://");
    if (!authority)
        return false;
    const char *path = strchr(authority + 3, '/');
    size_t size = strlen(input);
    while (size && input[size - 1] == '/')
        --size;
    const char *suffix = !path || path >= input + size ? "/v1/responses"
                         : size >= 3 && !memcmp(input + size - 3, "/v1", 3) ? "/responses" : "";
    int n = snprintf(result, 512, "%.*s%s", (int)size, input, suffix);
    return n > 0 && n < 512;
}

static bool valid_token(const char *token) {
    if (strlen(token) > 16384)
        return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; ++p)
        if (*p < 0x21 || *p > 0x7e)
            return false;
    return true;
}

static ah_code apply_connection(tui_app *app, const char *url, const char *token,
                                const char *model, bool chatgpt, ah_error *error) {
    bool onboarding = app->auth.onboarding, existing = app->session[0] != 0;
    if (!write_profile(app, url, chatgpt ? NULL : token, model, chatgpt))
        return ah_fail(error, AH_IO, "Cannot write %s", app->config_display);
    ah_code code = ah_responses_set_connection(app->responses, url, token, error);
    if (code != AH_OK)
        return code;
    yyjson_doc_free(app->catalog);
    app->catalog = NULL;
    strcpy(app->endpoint, url);
    strcpy(app->default_model, model);
    app->chatgpt = chatgpt;
    if (token) {
        app->has_token = true;
        strcpy(app->token_source, chatgpt ? "Keychain" : app->config_path[0] ? "saved" : "this run");
    }
    if (!existing) {
        code = ah_session_create(app->cli->engine, app->default_model, app->instructions,
                                 app->session, error);
        if (code != AH_OK)
            return code;
        code = tui_snapshot(app, app->session, true, error);
        if (code != AH_OK)
            return code;
    }
    tui_auth_close(app);
    if (onboarding) {
        tui_print_banner(app);
        if (existing && tui_history_page(app, app->cursor + 1, error) != AH_OK)
            tui_show_error(app, error->code, error);
    } else {
        app->status.notice_error = false;
        snprintf(app->notice, sizeof(app->notice), "%s%s%s",
                 chatgpt ? "ChatGPT account connected" : "API configuration applied",
                 app->config_path[0] ? " and saved to " : "",
                 app->config_path[0] ? app->config_display : "");
    }
    return AH_OK;
}

ah_code tui_config_apply(tui_app *app, const char *endpoint, const char *token, const char *model,
                         ah_error *error) {
    char url[512];
    bool loopback;
    if (app->running)
        return ah_fail(error, AH_BUSY, "Available when execution pauses");
    if (!normalize_url(endpoint, url) ||
        !ah_http_endpoint(url, app->allow_loopback_http, &loopback))
        return ah_fail(error, AH_INVALID, "Enter an HTTPS Responses URL without credentials, query, or fragment");
    if (!model || !*model || strlen(model) >= sizeof(app->default_model))
        return ah_fail(error, AH_INVALID, "Enter a default model of at most 256 bytes");
    if (token && !*token)
        token = NULL;
    if (!token && (app->chatgpt || !app->has_token || strcmp(app->endpoint, url)))
        return ah_fail(error, AH_INVALID, "Enter an API key for this endpoint");
    if (token && !valid_token(token))
        return ah_fail(error, AH_INVALID, "API key must contain printable ASCII without spaces");
    return apply_connection(app, url, token, model, false, error);
}

ah_code tui_config_chatgpt(tui_app *app, const char *token, const char *model, ah_error *error) {
    if (!token || !*token || !valid_token(token))
        return ah_fail(error, AH_AUTH, "Invalid ChatGPT access token");
    return apply_connection(app, TUI_CHATGPT_ENDPOINT, token, model, true, error);
}
