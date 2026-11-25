#include "core.h"
#include <stdio.h>

/* Acknowledging input and exposing it to inference are separate boundaries.
 * In particular, a user item cannot split an open function-call exchange. */
ah_code core_stage_user_item(ah_engine *e, const char *sid, unsigned turn, const char *item,
                             ah_error *err) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *record = doc ? yyjson_mut_obj(doc) : NULL;
    char *json = NULL;
    if (record) {
        yyjson_mut_doc_set_root(doc, record);
        if (yyjson_mut_obj_add_uint(doc, record, "turn", turn) &&
            yyjson_mut_obj_add_str(doc, record, "item", item))
            json = ah_mut_write(doc);
    }
    ah_mut_doc_free(doc);
    ah_code rc = json ? core_event(e, sid, NULL, "input.staged", json, err)
                      : ah_fail(err, AH_IO, "cannot stage user input");
    free(json);
    return rc;
}

ah_code core_publish_user_items(ah_engine *e, const core_session *session, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT seq,json FROM events WHERE session_id=?1 AND type='input.staged' "
           "AND seq>COALESCE((SELECT CAST(json AS INTEGER) FROM events WHERE session_id=?1 "
           "AND type='input.published' ORDER BY seq DESC LIMIT 1),0) ORDER BY seq",
        &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return step == SQLITE_DONE ? AH_OK : ah_fail(err, AH_IO, "cannot read staged input");
    }
    uint64_t after = 0;
    sqlite3_int64 last = 0;
    rc = core_begin(e, &after, err);
    if (rc != AH_OK) {
        sqlite3_finalize(s);
        return rc;
    }
    do {
        last = sqlite3_column_int64(s, 0);
        const char *json = (const char *)sqlite3_column_text(s, 1);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        yyjson_val *record = yyjson_doc_get_root(doc);
        const char *item = yyjson_get_str(yyjson_obj_get(record, "item"));
        yyjson_val *turn = yyjson_obj_get(record, "turn");
        if (!item || !yyjson_is_uint(turn))
            rc = ah_fail(err, AH_IO, "invalid staged input");
        else if (yyjson_get_uint(turn) == session->turn)
            rc = core_add_item(e, session->id, session->turn, item, err);
        yyjson_doc_free(doc);
    } while (rc == AH_OK && (step = sqlite3_step(s)) == SQLITE_ROW);
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(err, AH_IO, "cannot read staged input");
    sqlite3_finalize(s);
    if (rc == AH_OK) {
        char cursor[32];
        snprintf(cursor, sizeof(cursor), "%lld", (long long)last);
        rc = core_event(e, session->id, NULL, "input.published", cursor, err);
    }
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    return rc;
}
