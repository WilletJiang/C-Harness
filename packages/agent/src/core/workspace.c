#include "workspace_internal.h"
#include "../tools/local_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

ah_code core_workspace_schema(ah_engine *e, ah_error *error) {
    return core_sql(e,
                    "CREATE TABLE IF NOT EXISTS workspace_policy("
                    "id INTEGER PRIMARY KEY CHECK(id=1),path TEXT NOT NULL,"
                    "device INTEGER NOT NULL,inode INTEGER NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS workspace_owners("
                    "session_id TEXT PRIMARY KEY REFERENCES sessions(id),"
                    "broad INTEGER NOT NULL CHECK(broad IN (0,1)));"
                    "CREATE TABLE IF NOT EXISTS workspace_grants("
                    "session_id TEXT NOT NULL REFERENCES workspace_owners(session_id) "
                    "ON DELETE CASCADE,path TEXT NOT NULL,PRIMARY KEY(session_id,path));",
                    error);
}

ah_code workspace_load(ah_engine *e, char **path, struct stat *identity, bool *enabled,
                       ah_error *error) {
    *enabled = false;
    if (path)
        *path = NULL;
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e, "SELECT path,device,inode FROM workspace_policy WHERE id=1", &s, error);
    if (code != AH_OK)
        return code;
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        *enabled = true;
        if (identity) {
            memset(identity, 0, sizeof(*identity));
            identity->st_dev = (dev_t)sqlite3_column_int64(s, 1);
            identity->st_ino = (ino_t)sqlite3_column_int64(s, 2);
        }
        if (path && !(*path = ah_strdup((const char *)sqlite3_column_text(s, 0))))
            code = ah_fail(error, AH_IO, "Cannot copy workspace path");
    } else if (step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot read workspace policy");
    sqlite3_finalize(s);
    return code;
}

ah_code workspace_idle(ah_engine *e, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code = core_prepare(e,
                                "SELECT 1 FROM operations WHERE effect!=0 "
                                "AND status IN ('executing','unknown') LIMIT 1",
                                &s, error);
    if (code != AH_OK)
        return code;
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    return step == SQLITE_DONE ? AH_OK
           : step == SQLITE_ROW
               ? ah_fail(error, AH_BUSY,
                         "Settle running and unknown effects before changing ownership")
               : ah_fail(error, AH_IO, "Cannot inspect active effects");
}

ah_code workspace_session(ah_engine *e, const char *sid, ah_error *error) {
    if (!sid || !*sid)
        return ah_fail(error, AH_INVALID, "Session is required");
    core_session session;
    ah_code code = core_load(e, sid, &session, error);
    if (code == AH_OK)
        core_session_free(&session);
    return code;
}

ah_code workspace_open(ah_engine *e, int *out, ah_error *error) {
    *out = -1;
    char *path = NULL;
    struct stat pinned, actual;
    bool enabled = false;
    ah_code code = workspace_load(e, &path, &pinned, &enabled, error);
    int fd =
        code == AH_OK && enabled ? open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    free(path);
    if (code == AH_OK && (fd < 0 || fstat(fd, &actual) || actual.st_dev != pinned.st_dev ||
                          actual.st_ino != pinned.st_ino))
        code = ah_fail(error, AH_AUTH, "Pinned workspace moved or changed");
    if (code != AH_OK && fd >= 0)
        close(fd);
    else if (code == AH_OK)
        *out = fd;
    return code;
}

bool workspace_valid_path(int fd, const char *path) {
    if (!path)
        return false;
    if (!strcmp(path, "."))
        return true;
    ah_local_tools tools = {.workspace_fd = fd};
    char leaf[4097];
    int parent = ah_local_parent_path(&tools, path, strlen(path), leaf);
    if (parent < 0)
        return false;
    struct stat st;
    int found = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    bool valid = found == 0 ? S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) : errno == ENOENT;
    close(parent);
    return valid;
}

ah_code ah_workspace_enable(ah_engine *e, const char *workspace, ah_error *error) {
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!workspace || !*workspace)
        return ah_fail(error, AH_INVALID, "Workspace is required");
    char *canonical = realpath(workspace, NULL);
    int fd = canonical ? open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat actual, previous;
    if (fd < 0 || fstat(fd, &actual)) {
        if (fd >= 0)
            close(fd);
        free(canonical);
        return ah_fail(error, AH_IO, "Cannot pin workspace directory");
    }
    close(fd);
    bool enabled;
    code = workspace_load(e, NULL, &previous, &enabled, error);
    if (code == AH_OK && enabled &&
        (actual.st_dev != previous.st_dev || actual.st_ino != previous.st_ino))
        code = ah_fail(error, AH_AUTH, "Database already coordinates a different workspace");
    if (code == AH_OK && !enabled)
        code = workspace_idle(e, error);
    if (code == AH_OK && !enabled) {
        uint64_t after = 0;
        code = core_begin(e, &after, error);
        sqlite3_stmt *s = NULL;
        if (code == AH_OK)
            code = core_prepare(e, "INSERT INTO workspace_policy VALUES(1,?,?,?)", &s, error);
        if (code == AH_OK) {
            sqlite3_bind_text(s, 1, canonical, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)actual.st_dev);
            sqlite3_bind_int64(s, 3, (sqlite3_int64)actual.st_ino);
            code = core_done(e, s, error);
        }
        if (code == AH_OK)
            code = core_commit(e, after, error);
        else
            core_rollback(e);
    }
    free(canonical);
    return code;
}

ah_code ah_workspace_json(ah_engine *e, char **json, ah_error *error) {
    if (json)
        *json = NULL;
    ah_code code = core_guard(e, error);
    if (code != AH_OK)
        return code;
    if (!json)
        return ah_fail(error, AH_INVALID, "Output is required");
    char *path = NULL;
    bool enabled;
    code = workspace_load(e, &path, NULL, &enabled, error);
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_bool(d, root, "enabled", enabled);
    if (path)
        yyjson_mut_obj_add_strcpy(d, root, "workspace", path);
    free(path);
    yyjson_mut_val *owners = yyjson_mut_obj_add_arr(d, root, "owners");
    sqlite3_stmt *s = NULL;
    if (code == AH_OK)
        code = core_prepare(e,
                            "SELECT o.session_id,o.broad,g.path FROM workspace_owners o "
                            "LEFT JOIN workspace_grants g USING(session_id) "
                            "ORDER BY o.session_id,g.path",
                            &s, error);
    char previous[AH_ID_CAP] = "";
    yyjson_mut_val *paths = NULL;
    int step = SQLITE_DONE;
    while (code == AH_OK && (step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *sid = (const char *)sqlite3_column_text(s, 0);
        if (strcmp(previous, sid)) {
            snprintf(previous, sizeof(previous), "%s", sid);
            yyjson_mut_val *row = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, row, "session_id", sid);
            yyjson_mut_obj_add_bool(d, row, "broad", sqlite3_column_int(s, 1) != 0);
            paths = yyjson_mut_obj_add_arr(d, row, "paths");
            yyjson_mut_arr_append(owners, row);
        }
        if (sqlite3_column_type(s, 2) != SQLITE_NULL)
            yyjson_mut_arr_add_strcpy(d, paths, (const char *)sqlite3_column_text(s, 2));
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot read workspace ownership");
    sqlite3_finalize(s);
    if (code == AH_OK && !(*json = ah_mut_write(d)))
        code = ah_fail(error, AH_IO, "Cannot encode workspace ownership");
    ah_mut_doc_free(d);
    return code;
}
