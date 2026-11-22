#include "workspace_internal.h"
#include "../tools/local_internal.h"
#include <errno.h>
#include <fcntl.h>

/* Only versioned, individual native file replacements have a precise write set.
 * Consecutive siblings share an admission window. Anything else closes it. */
static char *target(const core_tool *tool, const core_operation *op) {
    if (!tool || tool->work || tool->tool.effect != op->effect ||
        tool->tool.requires_approval != op->approval || !op->schema ||
        strcmp(tool->schema, op->schema))
        return NULL;
    bool apply = tool->tool.execute == ah_local_apply;
    if (!apply && tool->tool.execute != ah_local_write_file)
        return NULL;
    yyjson_doc *args = NULL;
    ah_error error = {0};
    if (ah_schema_parse(tool->schema, op->arguments, &args, &error) != AH_OK)
        return NULL;
    yyjson_val *root = yyjson_doc_get_root(args), *entry = root;
    if (apply) {
        yyjson_val *files = yyjson_obj_get(root, "files");
        entry = yyjson_arr_size(files) == 1 && !yyjson_arr_size(yyjson_obj_get(root, "checks"))
                    ? yyjson_arr_get(files, 0)
                    : NULL;
    }
    const char *path = yyjson_get_str(yyjson_obj_get(entry, "path"));
    ah_local_tools *local = tool->tool.context;
    bool valid = path && strlen(path) == yyjson_get_len(yyjson_obj_get(entry, "path")) &&
                 strcmp(path, ".") && workspace_valid_path(local->workspace_fd, path) &&
                 (yyjson_obj_get(entry, "base_revision") ||
                  yyjson_is_true(yyjson_obj_get(entry, "expect_absent")));
    char *copy = valid ? ah_strdup(path) : NULL;
    yyjson_doc_free(args);
    return copy;
}

/* Linux mount aliases and hard links can resolve distinct spellings to one file.
 * Compare descriptor-resolved identities in addition to path/prefix rules. */
static bool file_identity(ah_local_tools *local, const char *path, struct stat *identity) {
    char leaf[4097];
    int parent = ah_local_parent_path(local, path, strlen(path), leaf);
    if (parent < 0)
        return false;
    int rc = fstatat(parent, leaf, identity, AT_SYMLINK_NOFOLLOW);
    int failure = errno;
    close(parent);
    if (rc == 0)
        return S_ISREG(identity->st_mode);
    memset(identity, 0, sizeof(*identity));
    return failure == ENOENT;
}

static ah_code run_batch(ah_engine *e, core_session *session, core_operation *ops,
                         bool *coordinated, unsigned n, ah_error *error) {
    ah_code code = core_jobs_init(e, error);
    if (code == AH_OK)
        code = core_tool_intents(e, session, ops, n, error);
    if (code != AH_OK)
        return code;
    for (unsigned i = 0; i < n; i++) {
        if (code == AH_OK)
            code = core_dispatch_tool(e, session, &ops[i], true, coordinated[i], error);
        else {
            ah_tool_result failure = {
                .json = core_object_string("error", "file worker was not launched"),
                .is_error = true};
            ah_error err = {0};
            ah_code saved =
                core_capture_operation(e, session, &ops[i], &failure, AH_OK, &err, AH_OK, &err);
            free(failure.json);
            if (saved != AH_OK) {
                code = saved;
                if (error)
                    *error = err;
            }
        }
    }
    /* Drain this window before admitting a dependent read, broad command, or
     * another session. Worker checkpoints are serviced exclusively by the owner. */
    void (*yield)(void *) = e->yield;
    e->yield = NULL;
    while (core_jobs_running(e, session->id)) {
        if (core_cancelled(e))
            core_jobs_cancel(e, session->id);
        core_jobs_wait(e, 10);
        ah_error err = {0};
        ah_code finished = core_jobs_finish(e, session, &err);
        if (finished != AH_OK && code == AH_OK) {
            code = finished;
            if (error)
                *error = err;
        }
    }
    e->yield = yield;
    return code;
}

ah_code core_write_batch(ah_engine *e, core_session *session, const core_operation *first,
                         bool *handled, ah_error *error) {
    *handled = false;
    unsigned limit = core_jobs_capacity(e);
    if (limit < 2 || e->inline_tools || core_jobs_running(e, NULL))
        return AH_OK;
    unsigned remain = e->config.max_tool_calls > session->tool_calls
                          ? e->config.max_tool_calls - session->tool_calls
                          : 0;
    if (remain < 2)
        return AH_OK;
    char *path = target(core_find_tool(e, first->name), first);
    if (!path)
        return AH_OK;
    free(path);
    ah_code code = core_task_allow(e, session->id, false, 0, &remain, error);
    if (code != AH_OK || remain < 2)
        return code;
    if (limit > remain)
        limit = remain;
    sqlite3_stmt *s = NULL;
    code = core_prepare(
        e,
        "SELECT id FROM operations WHERE session_id=?1 AND status!='done' AND "
        "seq>=(SELECT seq FROM operations WHERE id=?2) AND "
        "(SELECT parent_id FROM work_calls WHERE operation_id=id) IS "
        "(SELECT parent_id FROM work_calls WHERE operation_id=?2) ORDER BY seq LIMIT ?3",
        &s, error);
    if (code != AH_OK)
        return code;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, first->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, (int)limit);
    core_operation ops[16] = {0};
    char *paths[16] = {0};
    struct stat identities[16];
    bool coordinated[16] = {0};
    unsigned n = 0;
    const ah_local_tools *workspace = NULL;
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        core_operation *op = &ops[n];
        code =
            core_get_operation(e, session->id, (const char *)sqlite3_column_text(s, 0), op, error);
        if (code != AH_OK)
            break;
        core_tool *tool = core_find_tool(e, op->name);
        if (strcmp(op->status, "pending") && strcmp(op->status, "approved") &&
            strcmp(op->status, "approval"))
            break;
        paths[n] = target(tool, op);
        if (!paths[n])
            break;
        ah_local_tools *local = tool->tool.context;
        if (workspace && workspace != local)
            break;
        workspace = local;
        if (!file_identity(local, paths[n], &identities[n]))
            break;
        bool overlap = false;
        for (unsigned i = 0; i < n && !overlap && code == AH_OK; i++) {
            const struct stat *a = &identities[i], *b = &identities[n];
            overlap = a->st_mode && b->st_mode && a->st_dev == b->st_dev && a->st_ino == b->st_ino;
            if (!overlap)
                code = workspace_paths_overlap(local->workspace_fd, paths[i], paths[n], &overlap,
                                               error);
        }
        if (code != AH_OK || overlap)
            break;
        ah_error denied = {0};
        if (core_workspace_authorize(e, session->id, op, tool, &coordinated[n], &denied) != AH_OK)
            break;
        n++;
    }
    if (code == AH_OK && step != SQLITE_ROW && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot inspect file batch");
    sqlite3_finalize(s);
    if (code == AH_OK && n >= 2) {
        *handled = true;
        for (unsigned i = 0; i < n; i++) {
            if (ops[i].approval && ops[i].decision != 1 &&
                !core_auto_approve(core_find_tool(e, ops[i].name), &ops[i])) {
                if (i && ah_local_tools_auto_approve(workspace)) {
                    *handled = i >= 2;
                    if (*handled)
                        code = run_batch(e, session, ops, coordinated, i, error);
                    goto cleanup;
                }
                if (strcmp(ops[i].status, "approval"))
                    code = core_pause_approval(e, session, &ops[i], error);
                else
                    session->state = AH_WAITING_APPROVAL;
                goto cleanup;
            }
        }
        code = run_batch(e, session, ops, coordinated, n, error);
    }
cleanup:
    for (unsigned i = 0; i < 16; i++) {
        free(paths[i]);
        core_operation_free(&ops[i]);
    }
    return code;
}
