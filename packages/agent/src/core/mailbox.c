#include "core.h"

static void free_message(core_turn_input *message) {
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
    core_turn_input *message = e->mailbox_head;
    while (message) {
        core_turn_input *next = message->next;
        free_message(message);
        message = next;
    }
    pthread_mutex_destroy(&e->mailbox_lock);
}

bool core_mailbox_close_if_idle(ah_engine *e, const char *sid) {
    pthread_mutex_lock(&e->mailbox_lock);
    core_turn_input *message = e->mailbox_head;
    while (message && strcmp(message->session, sid))
        message = message->next;
    bool idle = message == NULL;
    if (idle && !strcmp(e->active_session, sid))
        e->active_session[0] = '\0';
    pthread_mutex_unlock(&e->mailbox_lock);
    return idle;
}
ah_code ah_steer(ah_engine *e, const char *sid, const char *command, const char *text,
                 ah_error *err) {
    return ah_steer_turn(e, sid, 0, command, text, NULL, err);
}
static ah_code queue_input(ah_engine *e, const char *sid, unsigned expected_turn, const char *command,
                           const char *text, bool reply, bool deferred, unsigned *accepted_turn,
                           ah_error *err) {
    if (!e || !sid || !command || !*command || strlen(command) > 256 || !text || !*text ||
        strlen(text) > e->config.max_context_bytes)
        return ah_fail(err, AH_INVALID, "bounded session, input identity and text required");
    if (strlen(sid) != AH_ID_CAP - 1)
        return ah_fail(err, AH_INVALID, "invalid session id");
    pthread_mutex_lock(&e->mailbox_lock);
    if (strcmp(e->active_session, sid)) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_BUSY, "live input requires the active running session");
    }
    if (expected_turn && expected_turn != e->active_turn) {
        pthread_mutex_unlock(&e->mailbox_lock);
        return ah_fail(err, AH_BUSY, "input turn is no longer active");
    }
    unsigned turn = e->active_turn;
    for (core_turn_input *m = e->mailbox_head; m; m = m->next) {
        if (m->reply == reply && m->turn == turn && !strcmp(m->session, sid) &&
            !strcmp(m->command, command)) {
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
        return ah_fail(err, AH_LIMIT, "input mailbox byte budget exhausted");
    }
    core_turn_input *message = calloc(1, sizeof(*message));
    if (message) {
        message->turn = turn;
        message->session = ah_strdup(sid);
        message->command = ah_strdup(command);
        message->text = ah_strdup(text);
        message->reply = reply;
        message->deferred = deferred;
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

static ah_code queue_turn_text(ah_engine *e, const char *sid, unsigned expected_turn,
                               const char *command, const char *text, bool deferred,
                               unsigned *accepted_turn, ah_error *err) {
    size_t length = text ? strlen(text) : 0, complete = 0;
    if (!e || !length || length > e->config.max_context_bytes ||
        !ah_utf8_prefix(text, length, &complete) || complete != length)
        return ah_fail(err, AH_INVALID, "bounded UTF-8 steering text required");
    return queue_input(e, sid, expected_turn, command, text, false, deferred, accepted_turn, err);
}

ah_code ah_steer_turn(ah_engine *e, const char *sid, unsigned expected_turn, const char *command,
                      const char *text, unsigned *accepted_turn, ah_error *err) {
    return queue_turn_text(e, sid, expected_turn, command, text, false, accepted_turn, err);
}

ah_code ah_queue_turn_input(ah_engine *e, const char *sid, unsigned expected_turn,
                            const char *command, const char *text, unsigned *accepted_turn,
                            ah_error *err) {
    return queue_turn_text(e, sid, expected_turn, command, text, true, accepted_turn, err);
}

ah_code ah_queue_reply(ah_engine *e, const char *sid, unsigned expected_turn, const char *operation,
                       const char *json, ah_error *err) {
    if (!e || !operation || strlen(operation) != AH_ID_CAP - 1 || !json ||
        strlen(json) > e->config.max_output_bytes || strlen(json) > e->config.max_context_bytes ||
        !ah_json_valid(json))
        return ah_fail(err, AH_INVALID, "operation and bounded JSON reply required");
    return queue_input(e, sid, expected_turn, operation, json, true, true, NULL, err);
}
/* Owner-only, including provider progress checks. Duplicate or conflicting command
 * IDs get their normal durable receipt without interrupting another generation. */
static const char *find_steer(ah_engine *e, const char *sid, char *command) {
    if (!atomic_load_explicit(&e->mailbox_bytes, memory_order_acquire))
        return NULL;
    const char *pending = NULL;
    pthread_mutex_lock(&e->mailbox_lock);
    for (core_turn_input *m = e->mailbox_head; m; m = m->next) {
        if (m->turn != e->active_turn || strcmp(m->session, sid))
            continue;
        if (command && m->deferred)
            break;
        if (m->deferred || m->command_status == STEER_RECORDED)
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
            pending = m->text;
            if (command)
                memcpy(command, m->command, strlen(m->command) + 1);
            break;
        }
    }
    pthread_mutex_unlock(&e->mailbox_lock);
    return pending;
}

bool core_model_has_steer(ah_engine *e, const char *sid) {
    return find_steer(e, sid, NULL) != NULL;
}

const char *core_model_peek_steer(ah_engine *e, const char *sid, char command[257]) {
    return find_steer(e, sid, command);
}
static char *ack_json(const core_turn_input *message, bool duplicate) {
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
static ah_code persist_message(ah_engine *e, core_session *session, core_turn_input *message,
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
    bool runnable = session->state == AH_RUNNING || session->state == AH_READY ||
                    session->state == AH_COMPLETED;
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
            rc = core_stage_user_item(e, session->id, session->turn, json, err);
        free(json);
        if (rc == AH_OK && runnable)
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
        if (runnable) {
            session->state = AH_RUNNING;
            session->completion_committed = false;
        }
    }
    return rc;
}

static ah_code persist_reply(ah_engine *e, core_session *session, const core_turn_input *message,
                             bool *added, ah_error *err) {
    bool answered = false;
    ah_code rc = message->turn == session->turn
                     ? core_answer_question(e, session->id, message->command, message->text,
                                            &answered, err)
                     : AH_NOT_FOUND;
    if (rc == AH_INVALID || rc == AH_NOT_FOUND) {
        uint64_t after = 0;
        rc = core_begin(e, &after, err);
        if (rc == AH_OK)
            rc = core_event(e, session->id, message->command, "input.reply_rejected",
                            "{\"reason\":\"question no longer accepts this answer\"}", err);
        if (rc == AH_OK)
            rc = core_commit(e, after, err);
        else
            core_rollback(e);
        return rc;
    }
    if (rc == AH_OK && answered) {
        *added = true;
        if (session->state == AH_COMPLETED && !session->completion_committed)
            session->state = AH_RUNNING;
        else if (session->state == AH_WAITING_INPUT)
            rc = ah_session_state(e, session->id, &session->state, err);
    }
    return rc;
}
static ah_code drain_input(ah_engine *e, core_session *session, bool terminal, bool deferred_only,
                           const char *command, const char *interruption, ah_error *err) {
    pthread_mutex_lock(&e->mailbox_lock);
    if (terminal && !e->agents_enabled && !strcmp(e->active_session, session->id))
        e->active_session[0] = '\0';
    core_turn_input *messages = NULL, *tail = NULL, **link = &e->mailbox_head;
    while (*link) {
        core_turn_input *message = *link;
        if (deferred_only && !message->deferred && !strcmp(message->session, session->id))
            break;
        if ((!deferred_only || message->deferred) && !strcmp(message->session, session->id) &&
            (!command || (!message->deferred && message->turn == session->turn &&
                          !strcmp(message->command, command)))) {
            *link = message->next;
            message->next = NULL;
            if (tail)
                tail->next = message;
            else
                messages = message;
            tail = message;
            if (command)
                break;
        } else
            link = &message->next;
    }
    e->mailbox_tail = e->mailbox_head;
    while (e->mailbox_tail && e->mailbox_tail->next)
        e->mailbox_tail = e->mailbox_tail->next;
    pthread_mutex_unlock(&e->mailbox_lock);
    ah_code rc = AH_OK;
    bool added = false;
    if (command && !messages)
        return ah_fail(err, AH_PROTOCOL, "native steer has no matching local input");
    while (messages) {
        rc = messages->reply ? persist_reply(e, session, messages, &added, err)
                             : persist_message(e, session, messages, &added, interruption, err);
        if (rc != AH_OK) {
            pthread_mutex_lock(&e->mailbox_lock);
            tail->next = e->mailbox_head;
            e->mailbox_head = messages;
            if (!e->mailbox_tail)
                e->mailbox_tail = tail;
            pthread_mutex_unlock(&e->mailbox_lock);
            break;
        }
        core_turn_input *next = messages->next;
        size_t bytes =
            strlen(messages->session) + strlen(messages->command) + strlen(messages->text) + 3;
        pthread_mutex_lock(&e->mailbox_lock);
        e->mailbox_bytes -= bytes;
        pthread_mutex_unlock(&e->mailbox_lock);
        free_message(messages);
        messages = next;
    }
    if (terminal && added && rc == AH_OK &&
        (session->state == AH_RUNNING || session->state == AH_READY))
        core_mailbox_active(e, session->id, session->turn);
    return rc;
}

ah_code core_drain_inputs(ah_engine *e, core_session *session, bool terminal,
                            const char *interruption, ah_error *err) {
    return drain_input(e, session, terminal, false, NULL, interruption, err);
}

ah_code core_drain_deferred_input(ah_engine *e, core_session *session, ah_error *err) {
    if (!atomic_load_explicit(&e->mailbox_bytes, memory_order_acquire))
        return AH_OK;
    return drain_input(e, session, false, true, NULL, NULL, err);
}

ah_code core_apply_native_steer(ah_engine *e, core_session *session, const char *command,
                                ah_error *err) {
    return drain_input(e, session, false, false, command, NULL, err);
}
