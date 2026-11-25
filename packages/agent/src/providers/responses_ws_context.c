#include "responses_ws_context.h"

#ifdef AH_HAVE_CURL_WS
static bool hash_value(ah_hash *hash, yyjson_val *value) {
    size_t length;
    char *encoded = yyjson_val_write(value, 0, &length);
    if (!encoded)
        return false;
    ah_hash_update(hash, encoded, length + 1);
    free(encoded);
    return true;
}

static bool transport_key(yyjson_val *key) {
    return yyjson_equals_str(key, "stream") || yyjson_equals_str(key, "background") ||
           yyjson_equals_str(key, "previous_response_id") || yyjson_equals_str(key, "type");
}

static size_t resume_at(yyjson_val *input, const responses_ws_context *previous,
                        responses_ws_input *pending) {
    pending->count = yyjson_arr_size(input);
    pending->valid = yyjson_is_arr(input);
    ah_hash_init(&pending->input);
    size_t start = 0, index, count;
    yyjson_val *item;
    yyjson_arr_foreach(input, index, count, item) {
        if (previous && *previous->id && previous->count == index &&
            !strcmp(previous->settings, pending->settings)) {
            ah_hash prefix = pending->input;
            char digest[65];
            ah_hash_finish(&prefix, digest);
            if (!strcmp(digest, previous->prefix))
                start = index;
        }
        if (!hash_value(&pending->input, item)) {
            pending->valid = false;
            return 0;
        }
    }
    return start;
}

char *responses_ws_encode(const char *json, const responses_ws_context *previous,
                          const char *already_queued,
                          responses_ws_input *pending, bool *continued, size_t *length) {
    yyjson_doc *request = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = request ? yyjson_doc_get_root(request) : NULL;
    yyjson_mut_doc *wire = ah_mut_doc_new();
    yyjson_mut_val *out = wire ? yyjson_mut_obj(wire) : NULL;
    char *encoded = NULL;
    *continued = false;
    *pending = (responses_ws_input){0};
    if (!out || !yyjson_is_obj(root) || !yyjson_mut_obj_add_str(wire, out, "type", "response.create"))
        goto done;
    yyjson_mut_doc_set_root(wire, out);
    ah_hash settings;
    ah_hash_init(&settings);
    size_t index, count;
    yyjson_val *key, *value;
    bool hash_ok = true;
    yyjson_obj_foreach(root, index, count, key, value) {
        if (transport_key(key) || yyjson_equals_str(key, "input"))
            continue;
        if (!yyjson_mut_obj_add(out, yyjson_val_mut_copy(wire, key), yyjson_val_mut_copy(wire, value)))
            goto done;
        if (hash_ok)
            hash_ok = hash_value(&settings, key) && hash_value(&settings, value);
    }
    ah_hash_finish(&settings, pending->settings);
    yyjson_val *input = yyjson_obj_get(root, "input");
    if (yyjson_is_arr(input)) {
        size_t start = resume_at(input, hash_ok ? previous : NULL, pending);
        pending->valid &= hash_ok;
        if (already_queued && !start)
            goto done;
        yyjson_mut_val *items = yyjson_mut_arr(wire);
        if (!items || !yyjson_mut_obj_add_val(wire, out, "input", items))
            goto done;
        yyjson_arr_iter iter = yyjson_arr_iter_with(input);
        for (size_t skip = 0; skip < start; ++skip)
            yyjson_arr_iter_next(&iter);
        bool omitted = false, tool_output = false;
        while ((value = yyjson_arr_iter_next(&iter))) {
            yyjson_val *type = yyjson_obj_get(value, "type");
            if (yyjson_equals_str(type, "function_call_output") ||
                yyjson_equals_str(type, "custom_tool_call_output"))
                tool_output = true;
            if (already_queued && yyjson_equals_str(yyjson_obj_get(value, "role"), "user") &&
                yyjson_equals_str(yyjson_obj_get(value, "content"), already_queued)) {
                if (omitted)
                    goto done;
                omitted = true;
                continue;
            }
            if (!yyjson_mut_arr_append(items, yyjson_val_mut_copy(wire, value)))
                goto done;
        }
        if (already_queued) {
            if (!omitted || !tool_output)
                goto done;
            /* The server inserts the steer ahead of tool receipts. The local
             * ledger keeps the receipts and their calls adjacent. */
            pending->valid = false;
        }
        if (start) {
            if (!yyjson_mut_obj_add_strcpy(wire, out, "previous_response_id", previous->id))
                goto done;
            *continued = true;
        }
    } else {
        if (already_queued ||
            (input && !yyjson_mut_obj_add_val(wire, out, "input", yyjson_val_mut_copy(wire, input))))
            goto done;
    }
    encoded = yyjson_mut_write(wire, 0, length);
done:
    ah_mut_doc_free(wire);
    yyjson_doc_free(request);
    return encoded;
}

void responses_ws_remember(responses_ws_context *context, responses_ws_input *pending,
                           const char *id, yyjson_val *response) {
    context->id[0] = 0;
    if (!pending->valid)
        return;
    yyjson_val *output = yyjson_obj_get(response, "output");
    /* An empty output leaves no ledger item proving the core accepted it. */
    bool valid = yyjson_arr_size(output) != 0;
    size_t index, count;
    yyjson_val *item;
    yyjson_arr_foreach(output, index, count, item) {
        if (yyjson_equals_str(yyjson_obj_get(item, "type"), "compaction") ||
            !hash_value(&pending->input, item)) {
            valid = false;
            break;
        }
    }
    if (valid) {
        memcpy(context->id, id, strlen(id) + 1);
        memcpy(context->settings, pending->settings, sizeof(context->settings));
        context->count = pending->count + yyjson_arr_size(output);
        ah_hash_finish(&pending->input, context->prefix);
    }
}
#endif
