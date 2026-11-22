#include "core.h"

static void free_message(core_steering *message) {
    free(message->session);
    free(message->command);
    free(message->text);
    free(message);
}
void core_mailbox_active(ah_engine *e, const char *sid, unsigned turn) {
    pthread_mutex_lock(&e->mailbox_lock);
    e->active_turn = turn;
    if (sid)
        memcpy(e->active_session, sid, AH_ID_CAP);
    else
        e->active_session[0] = '\0';
    pthread_mutex_unlock(&e->mailbox_lock);
}
void core_mailbox_free(ah_engine *e) {
    if (!e->mailbox_initialized)
        return;
    core_steering *message = e->mailbox_head;
    while (message) {
        core_steering *next = message->next;
        free_message(message);
        message = next;
    }
    pthread_mutex_destroy(&e->mailbox_lock);
}
ah_code ah_steer(ah_engine *e, const char *sid, const char *command, const char *text,
                 ah_error *err) {
    return ah_steer_turn(e, sid, 0, command, text, NULL, err);
}
ah_code ah_steer_turn(ah_engine *e, const char *sid, unsigned expected_turn, const char *command,
                      const char *text, unsigned *accepted_turn, ah_error *err) {
    if (!e || !sid || !command || !*command || strlen(command) > 256 || !text || !*text ||
        strlen(text) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded session, command id and steering text required");
    if (strlen(sid) != AH_ID_CAP - 1)
        return ah_fail(err, AH_INVALID, "invalid session id");
    pthread_mutex_lock(&e->mailbox_lock);
    if (strcmp(e->active_session, sid)) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_BUSY, "steering requires the active running session");
    }
    if (expected_turn && expected_turn != e->active_turn) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_BUSY, "steering turn is no longer active");
    }
    unsigned turn = e->active_turn;
    for (core_steering *m = e->mailbox_head; m; m = m->next) {
        if (m->turn == turn && !strcmp(m->session, sid) && !strcmp(m->command, command)) {
            bool same = !strcmp(m->text, text);
            pthread_mutex_unlock(&e->mailbox_lock);
            if (!same)
                return ah_fail(err, AH_INVALID, "queued command id has different text");
            if (accepted_turn)
                *accepted_turn = turn;
            ah_error_clear(err);
            return AH_OK;
        }
    }
    size_t bytes = strlen(sid) + strlen(command) + strlen(text) + 3;
    if (bytes > e->config.max_context_bytes ||
        e->mailbox_bytes > e->config.max_context_bytes - bytes) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_LIMIT, "steering mailbox byte budget exhausted");
    }
    core_steering *message = calloc(1, sizeof(*message));
    if (message) {
        message->turn = turn;
        message->session = ah_strdup(sid);
        message->command = ah_strdup(command);
        message->text = ah_strdup(text);
    }
    if (!message || !message->session || !message->command || !message->text) {
        if (message)
            free_message(message);
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_IO, "out of memory");
    }
    if (e->mailbox_tail)
        e->mailbox_tail->next = message;
    else
        e->mailbox_head = message;
    e->mailbox_tail = message;
    e->mailbox_bytes += bytes;
    if (accepted_turn)
        *accepted_turn = turn;
    pthread_mutex_unlock(&e->mailbox_lock);
    ah_error_clear(err);
    return AH_OK;
}
/* Owner-only, including provider progress checks. Duplicate or conflicting command
 * IDs get their normal durable receipt without interrupting another generation. */
bool core_model_has_steer(ah_engine *e, const char *sid) {
    if (!atomic_load_explicit(&e->mailbox_bytes, memory_order_acquire))
        return false;
    bool pending = false;
    pthread_mutex_lock(&e->mailbox_lock);
    for (core_steering *m = e->mailbox_head; m; m = m->next) {
        if (m->command_status == STEER_RECORDED || m->turn != e->active_turn ||
            strcmp(m->session, sid))
            continue;
        if (m->command_status == STEER_UNCHECKED) {
            sqlite3_stmt *s = NULL;
            if (core_prepare(e, "SELECT 1 FROM commands WHERE session_id=? AND id=?", &s, NULL) ==
                AH_OK) {
                sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, m->command, -1, SQLITE_TRANSIENT);
                int step = sqlite3_step(s);
                if (step == SQLITE_ROW)
                    m->command_status = STEER_RECORDED;
                else if (step == SQLITE_DONE)
                    m->command_status = STEER_NEW;
            }
            sqlite3_finalize(s);
        }
        if (m->command_status == STEER_NEW) {
            pending = true;
            break;
        }
    }
    pthread_mutex_unlock(&e->mailbox_lock);
    return pending;
}
static char *ack_json(const core_steering *message, bool duplicate) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_strcpy(d, o, "command_id", message->command);
    yyjson_mut_obj_add_strcpy(d, o, "text", message->text);
    yyjson_mut_obj_add_bool(d, o, "duplicate", duplicate);
    yyjson_mut_obj_add_uint(d, o, "turn", message->turn);
    char *json = ah_mut_write(d);
    ah_mut_doc_free(d);
    return json;
}
static ah_code persist_message(ah_engine *e, core_session *session, core_steering *message,
                               bool *added, const char *interruption, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "SELECT text,kind FROM commands WHERE session_id=? AND id=?", &s, err);
    if (rc != AH_OK)
        return rc;
    sqlite3_bind_text(s, 1, message->session, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, message->command, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(s);
    bool exists = step == SQLITE_ROW,
         same = exists && sqlite3_column_int(s, 1) == 0 &&
                !strcmp((const char *)sqlite3_column_text(s, 0), message->text);
    sqlite3_finalize(s);
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return ah_fail(err, AH_IO, "steering command lookup failed");
    bool stale = message->turn != session->turn;
    char *ack = ack_json(message, exists);
    if (!ack)
        return ah_fail(err, AH_IO, "out of memory");
    uint64_t after = 0;
    rc = core_begin(e, &after, err);
    if (rc == AH_OK && !exists && !stale) {
        rc = core_prepare(e, "INSERT INTO commands(session_id,id,text) VALUES(?,?,?)", &s, err);
        if (rc == AH_OK) {
            sqlite3_bind_text(s, 1, message->session, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, message->command, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 3, message->text, -1, SQLITE_TRANSIENT);
            rc = core_done(e, s, err);
        }
        yyjson_mut_doc *d = ah_mut_doc_new();
        if (!d)
            rc = ah_fail(err, AH_IO, "out of memory");
        char *json = NULL;
        if (d) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            yyjson_mut_doc_set_root(d, o);
            yyjson_mut_obj_add_str(d, o, "role", "user");
            yyjson_mut_obj_add_strcpy(d, o, "content", message->text);
            json = ah_mut_write(d);
            ah_mut_doc_free(d);
            if (!json)
                rc = ah_fail(err, AH_IO, "out of memory");
        }
        if (rc == AH_OK)
            rc = core_add_item(e, session->id, session->turn, json, err);
        free(json);
        if (rc == AH_OK)
            rc = core_state(e, session->id, AH_RUNNING, err);
    }
    if (rc == AH_OK && !exists && !stale && !*added && interruption)
        rc = core_event(e, session->id, NULL, "model.interrupted", interruption, err);
    if (rc == AH_OK)
        rc = core_event(e, session->id, NULL,
                        stale || (exists && !same) ? "input.rejected" : "input.steered", ack, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else
        core_rollback(e);
    free(ack);
    if (rc == AH_OK && !exists && !stale) {
        *added = true;
        session->state = AH_RUNNING;
        session->completion_committed = false;
    }
    return rc;
}
ah_code core_drain_steering(ah_engine *e, core_session *session, bool terminal,
                            const char *interruption, ah_error *err) {
    pthread_mutex_lock(&e->mailbox_lock);
    if (terminal && !e->agents_enabled && !strcmp(e->active_session, session->id))
        e->active_session[0] = '\0';
    core_steering *messages = NULL, *tail = NULL, **link = &e->mailbox_head;
    while (*link) {
        core_steering *message = *link;
        if (!strcmp(message->session, session->id)) {
            *link = message->next;
            message->next = NULL;
            if (tail)
                tail->next = message;
            else
                messages = message;
            tail = message;
        } else
            link = &message->next;
    }
    e->mailbox_tail = e->mailbox_head;
    while (e->mailbox_tail && e->mailbox_tail->next)
        e->mailbox_tail = e->mailbox_tail->next;
    pthread_mutex_unlock(&e->mailbox_lock);
    ah_code rc = AH_OK;
    bool added = false;
    while (messages) {
        rc = persist_message(e, session, messages, &added, interruption, err);
        if (rc != AH_OK) {
            pthread_mutex_lock(&e->mailbox_lock);
            tail->next = e->mailbox_head;
            e->mailbox_head = messages;
            if (!e->mailbox_tail)
                e->mailbox_tail = tail;
            pthread_mutex_unlock(&e->mailbox_lock);
            break;
        }
        core_steering *next = messages->next;
        size_t bytes =
            strlen(messages->session) + strlen(messages->command) + strlen(messages->text) + 3;
        pthread_mutex_lock(&e->mailbox_lock);
        e->mailbox_bytes -= bytes;
        pthread_mutex_unlock(&e->mailbox_lock);
        free_message(messages);
        messages = next;
    }
    if (terminal && added && rc == AH_OK)
        core_mailbox_active(e, session->id, session->turn);
    return rc;
}
