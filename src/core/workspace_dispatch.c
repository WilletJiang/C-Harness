#include "workspace_internal.h"
#include "../tools/local_internal.h"

static ah_code owns_path(ah_engine *e, int root, const char *sid, const char *path,
                         ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code =
        core_prepare(e, "SELECT path FROM workspace_grants WHERE session_id=?", &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = SQLITE_DONE;
    bool owned = false;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        code = workspace_path_contains(root, (const char *)sqlite3_column_text(s, 0), path, &owned,
                                       error);
        if (code != AH_OK || owned)
            break;
    }
    sqlite3_finalize(s);
    if (code != AH_OK)
        return code;
    if (!owned)
        return ah_fail(error, step == SQLITE_DONE ? AH_AUTH : AH_IO,
                       "Session has no write ownership for this path");
    return AH_OK;
}

static ah_code authorize_path(ah_engine *e, const char *sid, ah_local_tools *tools,
                              yyjson_val *path, bool broad, ah_error *error) {
    const char *text = yyjson_get_str(path);
    if (!text || yyjson_get_len(path) != strlen(text) ||
        !workspace_valid_path(tools->workspace_fd, text) || !strcmp(text, "."))
        return ah_fail(error, AH_INVALID, "Write path must be canonical and without symlinks");
    return broad ? AH_OK : owns_path(e, tools->workspace_fd, sid, text, error);
}

static ah_code broad_barrier(ah_engine *e, ah_error *error) {
    sqlite3_stmt *s = NULL;
    ah_code code = core_prepare(e,
                                "SELECT status FROM operations WHERE effect!=0 AND "
                                "status IN ('executing','unknown') ORDER BY status DESC LIMIT 1",
                                &s, error);
    if (code != AH_OK)
        return code;
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        bool unknown = !strcmp((const char *)sqlite3_column_text(s, 0), "unknown");
        code = ah_fail(error, unknown ? AH_AUTH : AH_BUSY,
                       unknown ? "Resolve unknown effects before broad execution"
                               : "Broad execution waits for other effects");
    } else if (step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot inspect effect barrier");
    sqlite3_finalize(s);
    return code;
}

ah_code core_workspace_authorize(ah_engine *e, const char *sid, const core_operation *op,
                                 const core_tool *registered, bool *coordinated, ah_error *error) {
    *coordinated = false;
    struct stat pinned;
    ah_code code = workspace_load(e, NULL, &pinned, coordinated, error);
    if (code != AH_OK || !*coordinated || op->effect == AH_EFFECT_READ)
        return code;
    bool broad = false;
    sqlite3_stmt *s = NULL;
    code = core_prepare(e, "SELECT broad FROM workspace_owners WHERE session_id=?", &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW)
        broad = sqlite3_column_int(s, 0) != 0;
    else if (step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot inspect write authority");
    sqlite3_finalize(s);
    if (code != AH_OK)
        return code;
    ah_execute_fn execute = registered ? registered->tool.execute : NULL;
    bool write = execute == ah_local_write_file, apply = execute == ah_local_apply;
    bool native = write || apply || execute == ah_local_exec || execute == ah_local_run_checks;
    ah_local_tools *tools = native ? registered->tool.context : NULL;
    struct stat actual;
    if (native && (!tools || fstat(tools->workspace_fd, &actual) ||
                   actual.st_dev != pinned.st_dev || actual.st_ino != pinned.st_ino))
        return ah_fail(error, AH_AUTH, "Tool workspace differs from the coordinated workspace");
    yyjson_doc *d = yyjson_read(op->arguments, strlen(op->arguments), 0);
    yyjson_val *root = d ? yyjson_doc_get_root(d) : NULL;
    if (!root)
        return ah_fail(error, AH_INVALID, "Invalid effect arguments");
    bool scoped = write || (apply && !yyjson_arr_size(yyjson_obj_get(root, "checks")));
    if (!scoped && !broad)
        code =
            ah_fail(error, AH_AUTH,
                    "Arbitrary effects and processes require explicit exclusive broad authority");
    if (code == AH_OK && broad)
        code = broad_barrier(e, error);
    if (code == AH_OK && write)
        code = authorize_path(e, sid, tools, yyjson_obj_get(root, "path"), broad, error);
    if (code == AH_OK && apply) {
        size_t i, n;
        yyjson_val *file;
        yyjson_arr_foreach(yyjson_obj_get(root, "files"), i, n, file) {
            code = authorize_path(e, sid, tools, yyjson_obj_get(file, "path"), broad, error);
            if (code != AH_OK)
                break;
        }
    }
    yyjson_doc_free(d);
    return code;
}
