#include "core.h"
#include <stdio.h>

typedef struct {
    sqlite3_int64 seq, turn, round;
    size_t bytes;
    bool keep;
} context_row;

/* Exact user messages and host/user answers are not summaries. Keep their complete
 * exchange, including the question, even when older tool work is archived. */
static bool protected_item(yyjson_val *item) {
    const char *role = yyjson_get_str(yyjson_obj_get(item, "role"));
    return (role &&
            (!strcmp(role, "user") || !strcmp(role, "developer") || !strcmp(role, "system"))) ||
           yyjson_equals_str(yyjson_obj_get(item, "type"), "compaction");
}
ah_code core_context_compact(ah_engine *e, core_session *session, size_t request_bytes,
                             bool *changed, ah_error *error) {
    *changed = false;
    if (!e->context_enabled || request_bytes <= e->context_policy.trigger_bytes)
        return AH_OK;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT 1 FROM operations WHERE session_id=? AND status!='done' LIMIT 1", &s, error);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    if (step == SQLITE_ROW)
        return ah_fail(error, AH_BUSY, "context maintenance requires settled tool exchanges");
    if (step != SQLITE_DONE)
        return ah_fail(error, AH_IO, "cannot inspect pending exchanges");
    rc = core_prepare(
        e,
        "SELECT i.seq,i.turn,i.round,i.json,EXISTS(SELECT 1 FROM operations o JOIN result_refs rr "
        "ON rr.operation_id=o.id "
        "WHERE rr.session_id=i.session_id AND o.call_id=json_extract(i.json,'$.call_id') "
        "AND o.name IN ('ask','wait','harness_ask','harness_wait')) "
        "FROM items i WHERE i.session_id=? AND i.active=1 ORDER BY i.seq DESC",
        &s, error);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
    context_row *rows = NULL;
    size_t count = 0, capacity = 0, saved = 0;
    unsigned exchanges = 0;
    sqlite3_int64 turn = -1, round = -1;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            if (next > 65536) {
                rc = ah_fail(error, AH_LIMIT, "context item count exceeds maintenance bound");
                break;
            }
            context_row *grown = realloc(rows, next * sizeof(*rows));
            if (!grown) {
                rc = ah_fail(error, AH_IO, "out of memory");
                break;
            }
            rows = grown;
            capacity = next;
        }
        const char *text = (const char *)sqlite3_column_text(s, 3);
        yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
        if (!doc) {
            rc = ah_fail(error, AH_IO, "corrupt model context");
            break;
        }
        context_row row = {.seq = sqlite3_column_int64(s, 0),
                           .turn = sqlite3_column_int64(s, 1),
                           .round = sqlite3_column_int64(s, 2),
                           .bytes = strlen(text),
                           .keep = protected_item(yyjson_doc_get_root(doc)) ||
                                   sqlite3_column_int(s, 4)};
        if (!row.keep) {
            if (row.turn != turn || row.round != round) {
                ++exchanges;
                turn = row.turn;
                round = row.round;
            }
            if (exchanges <= e->context_policy.keep_recent_exchanges)
                row.keep = true;
        }
        yyjson_doc_free(doc);
        rows[count++] = row;
    }
    if (rc == AH_OK && step != SQLITE_DONE)
        rc = ah_fail(error, AH_IO, "cannot inspect context items");
    sqlite3_finalize(s);
    if (rc != AH_OK) {
        free(rows);
        return rc;
    }
    /* Archive oldest complete rounds until the target, never half an exchange. */
    size_t selected = 0;
    turn = round = -1;
    for (size_t i = count; i > 0; --i) {
        context_row *row = &rows[i - 1];
        if (row->keep)
            continue;
        bool new_round = row->turn != turn || row->round != round;
        if (new_round && saved >= request_bytes - e->context_policy.target_bytes) {
            row->keep = true;
            continue;
        }
        turn = row->turn;
        round = row->round;
        saved += row->bytes;
        ++selected;
    }
    if (!selected) {
        free(rows);
        return AH_OK;
    }
    char id[AH_ID_CAP], note[512];
    rc = core_id(id, error);
    snprintf(note, sizeof(note),
             "Earlier model/tool exchanges are archived, not discarded. "
             "Use recall to list checkpoints and retrieve exact items when needed. "
             "Latest checkpoint: %s (%zu items). User inputs and recent exchanges remain exact.",
             id, selected);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, error);
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "INSERT INTO context_checkpoints(id,session_id,summary,before_bytes,after_bytes) "
            "VALUES(?,?,?,?,?)",
            &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, note, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)request_bytes);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)(request_bytes - saved));
        rc = core_done(e, s, error);
    }
    for (size_t i = 0; i < count && rc == AH_OK; ++i)
        if (!rows[i].keep) {
            rc = core_prepare(
                e, "INSERT INTO context_archive_items(checkpoint_id,item_seq) VALUES(?,?)", &s,
                error);
            if (rc == AH_OK) {
                sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 2, rows[i].seq);
                rc = core_done(e, s, error);
            }
            if (rc == AH_OK)
                rc = core_prepare(e, "UPDATE items SET active=0 WHERE seq=?", &s, error);
            if (rc == AH_OK) {
                sqlite3_bind_int64(s, 1, rows[i].seq);
                rc = core_done(e, s, error);
            }
        }
    if (rc == AH_OK)
        rc = core_prepare(
            e, "INSERT INTO context_checkpoint_refs(session_id,checkpoint_id) VALUES(?,?)", &s,
            error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, session->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, error);
    }
    char *event = core_object_string("checkpoint", id);
    if (rc == AH_OK)
        rc = event ? core_event(e, session->id, NULL, "context.archived", event, error)
                   : ah_fail(error, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_commit(e, after, error);
    else
        core_rollback(e);
    free(event);
    free(rows);
    *changed = rc == AH_OK;
    if (*changed)
        e->context_history = true;
    return rc;
}

/* Runs inside response acceptance. Preserve exact user/human inputs; only the
 * already-settled prefix is removed, never calls from the current response. */
ah_code core_context_native(ah_engine *e, const char *sid, sqlite3_int64 boundary,
                            ah_error *error) {
    char id[AH_ID_CAP];
    ah_code rc = core_id(id, error);
    sqlite3_stmt *s = NULL;
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "INSERT INTO context_checkpoints(id,session_id,summary,before_bytes,after_bytes) "
            "VALUES(?,?,?,0,0)",
            &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3,
                          "Provider-compacted history is represented by encrypted context. "
                          "Use recall to retrieve exact earlier exchanges when needed.",
                          -1, SQLITE_STATIC);
        rc = core_done(e, s, error);
    }
    if (rc == AH_OK)
        rc = core_prepare(
            e,
            "INSERT INTO context_archive_items(checkpoint_id,item_seq) "
            "SELECT ?,i.seq FROM items i WHERE i.session_id=? AND i.seq<=? AND i.active=1 "
            "AND COALESCE(json_extract(i.json,'$.role'),'') NOT IN ('user','developer','system') "
            "AND NOT EXISTS(SELECT 1 FROM operations o JOIN result_refs rr ON rr.operation_id=o.id "
            "WHERE rr.session_id=i.session_id "
            "AND o.call_id=json_extract(i.json,'$.call_id') AND o.name IN "
            "('ask','wait','harness_ask','harness_wait'))",
            &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, boundary);
        rc = core_done(e, s, error);
    }
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "UPDATE items SET active=0 WHERE seq IN "
                          "(SELECT item_seq FROM context_archive_items WHERE checkpoint_id=?)",
                          &s, error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, error);
    }
    if (rc == AH_OK)
        rc = core_prepare(
            e, "INSERT INTO context_checkpoint_refs(session_id,checkpoint_id) VALUES(?,?)", &s,
            error);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, error);
    }
    char *event = core_object_string("checkpoint", id);
    if (rc == AH_OK)
        rc = event ? core_event(e, sid, NULL, "context.native_compacted", event, error)
                   : ah_fail(error, AH_IO, "out of memory");
    free(event);
    return rc;
}
