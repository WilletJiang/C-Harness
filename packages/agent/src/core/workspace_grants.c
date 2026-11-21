#include "workspace_internal.h"
#include <unistd.h>

static ah_code grant_guard(ah_engine *e, ah_error *error) {
    ah_code code = core_guard(e, error);
    bool enabled = false;
    if (code == AH_OK)
        code = workspace_load(e, NULL, NULL, &enabled, error);
    if (code == AH_OK && !enabled)
        code = ah_fail(error, AH_INVALID, "Enable workspace coordination first");
    return code == AH_OK ? workspace_idle(e, error) : code;
}

static ah_code validate_paths(ah_engine *e, const char *const *paths, size_t count,
                              ah_error *error) {
    if (count > 64 || (count && !paths))
        return ah_fail(error, AH_INVALID, "At most 64 explicit write paths are allowed");
    int fd = -1;
    ah_code code = workspace_open(e, &fd, error);
    for (size_t i = 0; code == AH_OK && i < count; i++) {
        if (!workspace_valid_path(fd, paths[i]))
            code = ah_fail(error, AH_INVALID, "Grant path must be canonical and without symlinks");
        for (size_t j = 0; code == AH_OK && j < i; j++) {
            bool overlap = false;
            code = workspace_paths_overlap(fd, paths[i], paths[j], &overlap, error);
            if (code == AH_OK && overlap)
                code = ah_fail(error, AH_INVALID, "Grant paths must not overlap");
        }
    }
    if (fd >= 0)
        close(fd);
    return code;
}

static ah_code check_owners(ah_engine *e, const char *sid, const char *const *paths, size_t count,
                            bool broad, ah_error *error) {
    if (!count && !broad)
        return AH_OK;
    int fd = -1;
    ah_code code = workspace_open(e, &fd, error);
    if (code != AH_OK)
        return code;
    sqlite3_stmt *s = NULL;
    code = core_prepare(e,
                        "SELECT o.broad,g.path FROM workspace_owners o "
                        "LEFT JOIN workspace_grants g USING(session_id) "
                        "WHERE o.session_id!=?",
                        &s, error);
    if (code != AH_OK) {
        close(fd);
        return code;
    }
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *other = (const char *)sqlite3_column_text(s, 1);
        if (broad || sqlite3_column_int(s, 0))
            code = ah_fail(error, AH_BUSY, "Broad authority requires exclusive write ownership");
        for (size_t i = 0; code == AH_OK && other && i < count; i++) {
            bool overlap = false;
            code = workspace_paths_overlap(fd, other, paths[i], &overlap, error);
            if (code == AH_OK && overlap)
                code = ah_fail(error, AH_BUSY, "Write path overlaps another session's ownership");
        }
        if (code != AH_OK)
            break;
    }
    if (code == AH_OK && step != SQLITE_DONE)
        code = ah_fail(error, AH_IO, "Cannot inspect workspace owners");
    sqlite3_finalize(s);
    close(fd);
    return code;
}

ah_code ah_workspace_assign(ah_engine *e, const char *sid, const char *const *paths, size_t count,
                            bool broad, ah_error *error) {
    ah_code code = grant_guard(e, error);
    if (code == AH_OK)
        code = workspace_session(e, sid, error);
    if (code == AH_OK)
        code = validate_paths(e, paths, count, error);
    if (code == AH_OK)
        code = check_owners(e, sid, paths, count, broad, error);
    if (code != AH_OK)
        return code;
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    sqlite3_stmt *s = NULL;
    if (code == AH_OK)
        code = core_prepare(e, "DELETE FROM workspace_owners WHERE session_id=?", &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK && (count || broad)) {
        code = core_prepare(e, "INSERT INTO workspace_owners VALUES(?,?)", &s, error);
        if (code == AH_OK) {
            sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, broad);
            code = core_done(e, s, error);
        }
    }
    for (size_t i = 0; code == AH_OK && i < count; i++) {
        code = core_prepare(e, "INSERT INTO workspace_grants VALUES(?,?)", &s, error);
        if (code == AH_OK) {
            sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, paths[i], -1, SQLITE_TRANSIENT);
            code = core_done(e, s, error);
        }
    }
    if (code == AH_OK)
        code = core_event(e, sid, NULL, "workspace.assigned", "{}", error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}

static ah_code transfer(ah_engine *e, const char *from, const char *to, const char *path,
                        ah_error *error) {
    ah_code code = workspace_idle(e, error);
    if (code == AH_OK)
        code = workspace_session(e, from, error);
    if (code == AH_OK)
        code = workspace_session(e, to, error);
    if (code == AH_OK)
        code = validate_paths(e, &path, 1, error);
    if (code != AH_OK)
        return code;
    sqlite3_stmt *s = NULL;
    code = core_prepare(e,
                        "SELECT 1 FROM agent_nodes WHERE (session_id=?1 AND parent_id=?2) "
                        "OR (session_id=?2 AND parent_id=?1)",
                        &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, from, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, to, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step != SQLITE_ROW)
            code = ah_fail(error, step == SQLITE_DONE ? AH_AUTH : AH_IO,
                           "Transfers require a direct parent-child relationship");
        sqlite3_finalize(s);
    }
    if (code == AH_OK)
        code = core_prepare(e,
                            "SELECT (SELECT COUNT(*) FROM workspace_grants WHERE session_id=?3) "
                            "FROM workspace_grants g JOIN workspace_owners o "
                            "USING(session_id) WHERE g.session_id=?1 AND g.path=?2 AND o.broad=0",
                            &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, from, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, to, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step != SQLITE_ROW)
            code = ah_fail(error, step == SQLITE_DONE ? AH_AUTH : AH_IO,
                           "Source must own this exact path without broad authority");
        else if (sqlite3_column_int(s, 0) >= 64)
            code = ah_fail(error, AH_LIMIT, "Recipient already owns 64 write paths");
        sqlite3_finalize(s);
    }
    if (code != AH_OK)
        return code;
    if (code == AH_OK)
        code = core_prepare(e, "INSERT OR IGNORE INTO workspace_owners VALUES(?,0)", &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, to, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code = core_prepare(e,
                            "UPDATE workspace_grants SET session_id=? WHERE session_id=? "
                            "AND path=?",
                            &s, error);
    if (code == AH_OK) {
        sqlite3_bind_text(s, 1, to, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, from, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, path, -1, SQLITE_TRANSIENT);
        code = core_done(e, s, error);
    }
    if (code == AH_OK)
        code = core_sql(e,
                        "DELETE FROM workspace_owners WHERE broad=0 AND NOT EXISTS "
                        "(SELECT 1 FROM workspace_grants g WHERE "
                        "g.session_id=workspace_owners.session_id)",
                        error);
    if (code == AH_OK)
        code = core_event(e, from, NULL, "workspace.transferred", "{}", error);
    if (code == AH_OK)
        code = core_event(e, to, NULL, "workspace.received", "{}", error);
    return code;
}

ah_code ah_workspace_transfer(ah_engine *e, const char *from, const char *to, const char *path,
                              ah_error *error) {
    ah_code code = grant_guard(e, error);
    if (code != AH_OK)
        return code;
    uint64_t after = 0;
    code = core_begin(e, &after, error);
    if (code == AH_OK)
        code = transfer(e, from, to, path, error);
    if (code == AH_OK)
        code = core_commit(e, after, error);
    else
        core_rollback(e);
    return code;
}

ah_code core_workspace_delegate(ah_engine *e, const char *from, const char *to, yyjson_val *paths,
                                ah_error *error) {
    if (!yyjson_arr_size(paths))
        return AH_OK;
    bool enabled = false;
    ah_code code = workspace_load(e, NULL, NULL, &enabled, error);
    if (code == AH_OK && !enabled)
        code =
            ah_fail(error, AH_INVALID, "write_paths requires host-enabled workspace coordination");
    size_t i, n;
    yyjson_val *path;
    yyjson_arr_foreach(paths, i, n, path) {
        if (code != AH_OK)
            break;
        const char *text = yyjson_get_str(path);
        if (!text || strlen(text) != yyjson_get_len(path))
            code = ah_fail(error, AH_INVALID, "Invalid delegated write path");
        else
            code = transfer(e, from, to, text, error);
    }
    /* Agent actions reject policy failures with a receipt and roll back their
     * entire savepoint, including the new child and any earlier path transfers. */
    if (code == AH_AUTH || code == AH_BUSY) {
        code = AH_INVALID;
        if (error)
            error->code = code;
    }
    return code;
}
