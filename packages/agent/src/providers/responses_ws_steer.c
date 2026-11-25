#include "responses_ws_steer.h"

#ifdef AH_HAVE_CURL_WS
void responses_ws_steer_clear(responses_ws_steer *s) {
    free(s->frame);
    free(s->input);
    *s = (responses_ws_steer){0};
}

static bool encode(responses_ws_steer *s, const char *id, const char *input) {
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!doc)
        return false;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (root && yyjson_mut_obj_add_str(doc, root, "type", "response.steer") &&
        yyjson_mut_obj_add_str(doc, root, "previous_response_id", id) &&
        yyjson_mut_obj_add_str(doc, root, "input", input)) {
        s->frame = yyjson_mut_write(doc, 0, &s->length);
        s->input = ah_strdup(input);
    }
    ah_mut_doc_free(doc);
    if (!s->frame || !s->input)
        return false;
    memcpy(s->parent, id, strlen(id) + 1);
    return true;
}

ah_code responses_ws_send_steer(responses_request *t, responses_ws_steer *s, const char *id,
                                 responses_ws_io *io) {
    responses_turn_steer *control = t->steer;
    if (s->write_done)
        return AH_OK;
    if (!s->frame) {
        if (!control || !control->in_flight || control->successor || !*id || s->accepted)
            return AH_OK;
        const char *input = control->peek(control->context, control->command);
        if (!input || !encode(s, id, input))
            return ah_fail(&t->error, AH_CANCELLED, "cannot send native steer");
    }
    size_t sent = 0;
    CURLcode code = responses_ws_io_send(io, s->frame + s->sent, s->length - s->sent, &sent, NULL);
    t->uploaded_bytes += sent;
    s->sent += sent;
    if (code != CURLE_OK && code != CURLE_AGAIN) {
        if (!responses_ws_io_status(io))
            return ah_fail(&t->error, AH_NETWORK, "provider WebSocket steer write failed");
        /* Receive will report the disconnect after any buffered acknowledgement
         * and terminal response have crossed the validation boundary. */
        s->write_done = true;
        return AH_OK;
    }
    if (code == CURLE_OK) {
        s->write_done = true;
        responses_state(t, "steer_sent", 0);
    }
    return AH_OK;
}

static bool matches_parent(const responses_ws_steer *s, yyjson_val *steer) {
    return *s->parent && yyjson_equals_str(yyjson_obj_get(steer, "previous_response_id"), s->parent);
}

bool responses_ws_pending_event(const responses_ws_steer *s, yyjson_val *event) {
    yyjson_val *steer = yyjson_obj_get(event, "steer");
    return s->accepted && yyjson_equals_str(yyjson_obj_get(event, "type"), "response.steer.pending") &&
           matches_parent(s, steer) && yyjson_equals_str(yyjson_obj_get(steer, "id"), s->id);
}

static bool has_tool_calls(const ah_stream *stream) {
    yyjson_doc *doc = yyjson_read(stream->response, strlen(stream->response), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *items = yyjson_obj_get(root, "output"), *item;
    size_t index, count;
    bool found = false;
    yyjson_arr_foreach(items, index, count, item) {
        yyjson_val *type = yyjson_obj_get(item, "type");
        if (yyjson_equals_str(type, "function_call") || yyjson_equals_str(type, "custom_tool_call")) {
            found = true;
            break;
        }
    }
    yyjson_doc_free(doc);
    return found;
}

static void advance_parent(responses_request *t, responses_ws_steer *s, char *id) {
    if (!s->accepted || !s->parent_ended)
        return;
    if (s->waiting_tools) {
        t->steer->applied = t->steer->awaiting_tools = true;
        responses_state(t, "steer_waiting_tools", 0);
        return;
    }
    id[0] = 0;
    responses_stream_restart(t);
}

void responses_ws_steer_terminal(responses_request *t, responses_ws_steer *s, char *id) {
    if (!t->steer || !*s->parent)
        return;
    if (t->steer->successor) {
        t->steer->applied = true;
        return;
    }
    s->parent_ended = true;
    s->waiting_tools = has_tool_calls(&t->stream);
    advance_parent(t, s, id);
}

bool responses_ws_steer_holds(const responses_request *t, const responses_ws_steer *s) {
    return t->steer && *s->parent && !s->accepted;
}

ah_code responses_ws_steer_event(responses_request *t, responses_ws_steer *s, char *id,
                                 yyjson_val *event, bool *handled) {
    *handled = false;
    if (!*s->parent)
        return AH_OK;
    yyjson_val *type = yyjson_obj_get(event, "type");
    yyjson_val *steer = yyjson_obj_get(event, "steer");
    if (yyjson_equals_str(type, "response.steer.accepted")) {
        *handled = true;
        yyjson_val *value = yyjson_obj_get(steer, "id");
        const char *accepted = yyjson_get_str(value);
        size_t length = yyjson_get_len(value);
        if (!matches_parent(s, steer) || !accepted || !length || length >= sizeof(s->id) ||
            memchr(accepted, 0, length) || s->accepted)
            return ah_fail(&t->error, AH_PROTOCOL, "unexpected WebSocket steer acknowledgement");
        memcpy(s->id, accepted, length + 1);
        s->accepted = true;
        responses_state(t, "steer_accepted", 0);
        advance_parent(t, s, id);
    } else if (yyjson_equals_str(type, "response.steer.failed")) {
        *handled = true;
        if (!matches_parent(s, steer) || (s->accepted &&
            !yyjson_equals_str(yyjson_obj_get(steer, "id"), s->id)))
            return ah_fail(&t->error, AH_PROTOCOL, "unexpected WebSocket steer rejection");
        s->rejected = true;
        responses_state(t, "steer_rejected", 0);
        return ah_fail(&t->error, AH_CANCELLED, "provider rejected native steer");
    } else if (responses_ws_pending_event(s, event)) {
        *handled = true;
    } else if (yyjson_equals_str(type, "response.incomplete") &&
               yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(event, "response"),
                   "incomplete_details"), "reason"), "steered")) {
        *handled = true;
        if (s->parent_ended || !yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(event, "response"),
                                                "id"), s->parent))
            return ah_fail(&t->error, AH_PROTOCOL, "unexpected interrupted WebSocket response");
        s->parent_ended = true;
        advance_parent(t, s, id);
    } else if (yyjson_equals_str(type, "response.created") && !*id && s->parent_ended &&
               s->accepted) {
        t->steer->successor = true;
        responses_state(t, "steer_continued", 0);
    }
    return AH_OK;
}
#endif
