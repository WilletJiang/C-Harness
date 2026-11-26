#include "tui/tui.h"
#include "ah/observe.h"
#include "hash.h"
#include "internal.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static bool private_directory(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == getuid() &&
           !(info.st_mode & 0077);
}

static bool make_directories(char *path) {
    for (char *part = path + 1; *part; ++part) {
        if (*part != '/')
            continue;
        *part = 0;
        int result = mkdir(path, 0700);
        int saved = errno;
        *part = '/';
        if (result != 0 && saved != EEXIST)
            return false;
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

ah_code tui_state_directory(const char *workspace, char path[PATH_MAX], ah_error *error) {
    char *root = realpath(workspace, NULL);
    struct stat info;
    if (!root || stat(root, &info) != 0 || !S_ISDIR(info.st_mode)) {
        free(root);
        return ah_fail(error, AH_INVALID, "Cannot resolve workspace directory");
    }
    char digest[65];
    ah_hash_bytes(root, strlen(root), digest);
    free(root);

    const char *base = getenv("XDG_STATE_HOME");
    const char *suffix = "";
    if (!base || base[0] != '/') {
        base = getenv("HOME");
        suffix = "/.local/state";
    }
    if (!base || base[0] != '/')
        return ah_fail(error, AH_INVALID, "Set HOME or an absolute XDG_STATE_HOME for TUI storage");
    int n = snprintf(path, PATH_MAX, "%s%s/ah", base, suffix);
    if (n < 0 || n >= PATH_MAX)
        return ah_fail(error, AH_LIMIT, "TUI state path is too long");
    if (!make_directories(path) || !private_directory(path))
        return ah_fail(error, AH_IO, "TUI state directory must be private to your account");
    size_t length = (size_t)n;
    n = snprintf(path + length, PATH_MAX - length, "/tui");
    if (n < 0 || (size_t)n >= PATH_MAX - length)
        return ah_fail(error, AH_LIMIT, "TUI state path is too long");
    if ((mkdir(path, 0700) != 0 && errno != EEXIST) || !private_directory(path))
        return ah_fail(error, AH_IO, "Cannot create private TUI state directory");
    length += (size_t)n;
    n = snprintf(path + length, PATH_MAX - length, "/%s", digest);
    if (n < 0 || (size_t)n >= PATH_MAX - length)
        return ah_fail(error, AH_LIMIT, "TUI state path is too long");
    if ((mkdir(path, 0700) != 0 && errno != EEXIST) || !private_directory(path))
        return ah_fail(error, AH_IO, "Cannot create private workspace state directory");
    return AH_OK;
}

static bool window_filename(const char *name) {
    if (strncmp(name, "window-", 7) || name[7] < '1' || name[7] > '9')
        return false;
    const char *end = name + 8;
    while (*end >= '0' && *end <= '9')
        end++;
    return !strcmp(end, ".db");
}

ah_code tui_visit_databases(const char *directory, tui_visit_database visit, void *context,
                           ah_error *error) {
    DIR *dir = opendir(directory);
    if (!dir)
        return ah_fail(error, AH_IO, "Cannot read workspace state directory");
    ah_code code = AH_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno)
                code = ah_fail(error, AH_IO, "Cannot list workspace databases");
            break;
        }
        if (!window_filename(entry->d_name))
            continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            code = ah_fail(error, AH_LIMIT, "Workspace database path is too long");
            break;
        }
        if (!visit(path, context))
            break;
    }
    closedir(dir);
    return code;
}

typedef struct {
    const char *session;
    char *path;
    ah_code error;
} session_location;

static bool find_session(const char *path, void *context) {
    session_location *location = context;
    ah_reader *reader = NULL;
    ah_error ignored = {0};
    ah_code code = ah_reader_open_with_writer(path, &reader, &ignored);
    char *json = NULL;
    if (code == AH_OK)
        code = ah_reader_session_json(reader, location->session, &json, &ignored);
    ah_free(json);
    ah_reader_close(reader);
    if (code == AH_OK) {
        strcpy(location->path, path);
        return false;
    }
    if (code != AH_NOT_FOUND && location->error == AH_OK)
        location->error = code;
    return true;
}

ah_code tui_session_database(const char *directory, const char *session, char path[PATH_MAX],
                             ah_error *error) {
    path[0] = 0;
    if (strlen(session) >= AH_ID_CAP || !*session)
        return ah_fail(error, AH_INVALID, "Invalid session ID");
    session_location location = {.session = session, .path = path};
    ah_code code = tui_visit_databases(directory, find_session, &location, error);
    if (code != AH_OK || path[0])
        return code;
    if (location.error != AH_OK)
        return ah_fail(error, location.error, "Cannot search every workspace conversation");
    return ah_fail(error, AH_NOT_FOUND, "Session not found in workspace state; use --db for a separate database");
}

ah_code tui_open_window(const char *directory, char path[PATH_MAX], tui_open_engine open,
                        void *context, ah_engine **engine, ah_local_tools **tools, ah_error *error) {
    for (unsigned window = 1; window; window++) {
        int n = snprintf(path, PATH_MAX, "%s/window-%u.db", directory, window);
        if (n < 0 || n >= PATH_MAX)
            return ah_fail(error, AH_LIMIT, "Workspace database path is too long");
        ah_code code = open(context, path, engine, tools, error);
        if (code != AH_BUSY)
            return code;
    }
    return ah_fail(error, AH_LIMIT, "No workspace database slots available");
}
