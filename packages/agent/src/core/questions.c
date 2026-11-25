#include "core.h"

/* A nonblocking ask settles its tool call immediately. The journal records the
 * separate user question and its answer, with one identity and no polling. */
ah_code core_open_question(ah_engine *e, const core_session *session, const core_operation *op,
                           ah_error *err) {
    yyjson_doc *parsed = yyjson_read(op->arguments, strlen(op->arguments), 0);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *event = doc ? yyjson_mut_obj(doc) : NULL;
    char *opened = NULL;
    if (event && parsed) {
        yyjson_mut_doc_set_root(doc, event);
        if (yyjson_mut_obj_add_uint(doc, event, "turn", session->turn) &&
            yyjson_mut_obj_add_val(doc, event, "arguments",
                                   yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed))))
            opened = ah_mut_write(doc);
    }
    yyjson_doc_free(parsed);
    ah_mut_doc_free(doc);
    char *receipt = core_object_string("pending_question", op->id);
    ah_code rc = opened && receipt
                     ? core_receipt(e, session->id, op, receipt, false, "tool.completed", err)
                     : ah_fail(err, AH_IO, "cannot encode pending question");
    if (rc == AH_OK)
        rc = core_event(e, session->id, op->id, "input.question_opened", opened, err);
    free(receipt);
    free(opened);
    return rc;
}

ah_code core_pending_question(ah_engine *e, const char *sid, core_operation *op, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT q.operation_id FROM events q JOIN sessions s ON s.id=q.session_id "
           "WHERE q.session_id=? AND q.type='input.question_opened' "
           "AND json_extract(q.json,'$.turn')=s.turn AND NOT EXISTS(SELECT 1 FROM events a "
           "WHERE a.session_id=q.session_id AND a.operation_id=q.operation_id "
           "AND a.type='input.question_answered') ORDER BY q.seq LIMIT 1",
        &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW && op)
            rc = core_get_operation(e, sid, (const char *)sqlite3_column_text(s, 0), op, err);
        else if (step != SQLITE_ROW)
            rc = step == SQLITE_DONE ? AH_NOT_FOUND
                                     : ah_fail(err, AH_IO, "cannot read pending question");
    }
    sqlite3_finalize(s);
    return rc;
}

ah_code core_question_finish_state(ah_engine *e, const char *sid, ah_state *state, ah_error *err) {
    ah_code rc = core_pending_question(e, sid, NULL, err);
    if (rc == AH_OK)
        *state = AH_WAITING_INPUT;
    else if (rc == AH_NOT_FOUND) {
        *state = AH_COMPLETED;
        rc = AH_OK;
    }
    return rc;
}

static char *answer_item(const char *id, const char *arguments, const char *answer) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_doc *value = yyjson_read(answer, strlen(answer), 0);
    yyjson_doc *args = yyjson_read(arguments, strlen(arguments), 0);
    yyjson_val *question = yyjson_obj_get(yyjson_doc_get_root(args), "question");
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    yyjson_mut_val *data = root ? yyjson_mut_obj(d) : NULL;
    char *item = NULL;
    if (data && value && question && yyjson_mut_obj_add_str(d, data, "reply_to_question", id) &&
        yyjson_mut_obj_add_val(d, data, "question", yyjson_val_mut_copy(d, question)) &&
        yyjson_mut_obj_add_val(d, data, "answer", yyjson_val_mut_copy(d, yyjson_doc_get_root(value)))) {
        char *content = yyjson_mut_val_write(data, 0, NULL);
        yyjson_mut_doc_set_root(d, root);
        if (content && yyjson_mut_obj_add_str(d, root, "role", "user") &&
            yyjson_mut_obj_add_str(d, root, "content", content))
            item = ah_mut_write(d);
        free(content);
    }
    yyjson_doc_free(args);
    yyjson_doc_free(value);
    ah_mut_doc_free(d);
    return item;
}

ah_code core_answer_question(ah_engine *e, const char *sid, const char *id, const char *json,
                             bool *added, ah_error *err) {
    *added = false;
    sqlite3_stmt *s = NULL;
    ah_code rc = core_prepare(
        e, "SELECT o.arguments,s.turn,s.state,json_extract(q.json,'$.turn'),a.json "
           "FROM events q JOIN sessions s ON s.id=q.session_id JOIN operations o ON o.id=q.operation_id "
           "LEFT JOIN events a ON a.session_id=q.session_id AND a.operation_id=q.operation_id "
           "AND a.type='input.question_answered' WHERE q.session_id=? AND q.operation_id=? "
           "AND q.type='input.question_opened'",
        &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(s);
        return step == SQLITE_DONE ? AH_NOT_FOUND : ah_fail(err, AH_IO, "cannot read question");
    }
    unsigned turn = (unsigned)sqlite3_column_int64(s, 1);
    ah_state state = (ah_state)sqlite3_column_int(s, 2);
    const char *prior = (const char *)sqlite3_column_text(s, 4);
    if (prior || turn != (unsigned)sqlite3_column_int64(s, 3)) {
        bool same = prior && !strcmp(prior, json);
        sqlite3_finalize(s);
        return same ? AH_OK : ah_fail(err, AH_INVALID, "question closed or answer differs");
    }
    char *item = answer_item(id, (const char *)sqlite3_column_text(s, 0), json);
    sqlite3_finalize(s);
    if (!item)
        return ah_fail(err, AH_IO, "cannot encode question answer");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = core_stage_user_item(e, sid, turn, item, err);
    if (rc == AH_OK)
        rc = core_event(e, sid, id, "input.question_answered", json, err);
    if (rc == AH_OK && state == AH_WAITING_INPUT) {
        core_operation blocking;
        rc = core_next_operation(e, sid, &blocking, err);
        if (rc == AH_OK)
            core_operation_free(&blocking);
        else if (rc == AH_NOT_FOUND)
            rc = core_state(e, sid, AH_READY, err);
    }
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    *added = rc == AH_OK;
    free(item);
    return rc;
}
