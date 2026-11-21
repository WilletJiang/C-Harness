#include "trace.h"
#include "internal.h"
#include <stdio.h>

void eval_transport_observe(void *context, const ah_responses_metrics *m) {
    (void)context;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "event", "provider.metrics");
    yyjson_mut_obj_add_uint(doc, root, "attempt", m->attempt);
    yyjson_mut_obj_add_str(doc, root, "code", ah_code_name(m->result));
    yyjson_mut_obj_add_int(doc, root, "http_status", m->http_status);
    yyjson_mut_obj_add_uint(doc, root, "request_bytes", m->request_bytes);
    yyjson_mut_obj_add_uint(doc, root, "response_bytes", m->response_bytes);
    yyjson_mut_obj_add_bool(doc, root, "retry_planned", m->retry_planned);
    if (m->elapsed_present)
        yyjson_mut_obj_add_uint(doc, root, "elapsed_us", m->elapsed_us);
    else
        yyjson_mut_obj_add_null(doc, root, "elapsed_us");
    if (m->new_connections_present)
        yyjson_mut_obj_add_int(doc, root, "new_connections", m->new_connections);
    else
        yyjson_mut_obj_add_null(doc, root, "new_connections");
    const char *names[] = {"first_byte_us",    "upload_complete_us",
                           "first_content_us", "first_item_us",
                           "terminal_us",      "dns_us",
                           "connect_us",       "tls_us",
                           "pretransfer_us",   "curl_first_byte_us",
                           "curl_total_us"};
    ah_responses_time times[] = {m->first_byte,    m->upload_complete,
                                 m->first_content, m->first_output_item,
                                 m->terminal,      m->dns,
                                 m->connect,       m->tls,
                                 m->pretransfer,   m->curl_first_byte,
                                 m->curl_total};
    for (size_t i = 0; i < sizeof(times) / sizeof(*times); i++) {
        if (times[i].present)
            yyjson_mut_obj_add_uint(doc, root, names[i], times[i].us);
        else
            yyjson_mut_obj_add_null(doc, root, names[i]);
    }
    char *line = ah_mut_write(doc);
    ah_mut_doc_free(doc);
    if (line) {
        puts(line);
        fflush(stdout);
        free(line);
    }
}

static void response_event(const char *json, ah_code code, const ah_error *error, uint64_t elapsed,
                           unsigned request_id) {
    yyjson_doc *parsed = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_mut_doc *doc = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(doc), *data = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "event", "provider.response");
    yyjson_mut_obj_add_uint(doc, root, "request_id", request_id);
    yyjson_mut_obj_add_uint(doc, root, "elapsed_ms", elapsed);
    yyjson_mut_obj_add_val(doc, root, "data", data);
    const char *fields[] = {"id",
                            "model",
                            "status",
                            "service_tier",
                            "usage",
                            "reasoning",
                            "prompt_cache_diagnostics",
                            "prompt_cache_key",
                            "prompt_cache_options",
                            "prompt_cache_retention"};
    for (size_t i = 0; parsed && i < sizeof(fields) / sizeof(*fields); i++) {
        yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(parsed), fields[i]);
        if (value)
            yyjson_mut_obj_add_val(doc, data, fields[i], yyjson_val_mut_copy(doc, value));
    }
    if (code != AH_OK) {
        yyjson_mut_val *failure = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, failure, "code", ah_code_name(code));
        yyjson_mut_obj_add_strcpy(doc, failure, "message", error ? error->message : "");
        yyjson_mut_obj_add_val(doc, root, "transport_error", failure);
    }
    char *line = ah_mut_write(doc);
    if (line) {
        puts(line);
        fflush(stdout);
    }
    free(line);
    ah_mut_doc_free(doc);
    yyjson_doc_free(parsed);
}
static char *prepare_request(eval_model *model, const char *request, unsigned request_id,
                             ah_error *error) {
    yyjson_doc *parsed = yyjson_read(request, strlen(request), 0);
    yyjson_mut_doc *doc = ah_mut_doc_new();
    if (!parsed || !doc) {
        yyjson_doc_free(parsed);
        ah_mut_doc_free(doc);
        ah_fail(error, AH_IO, "cannot prepare evaluation request");
        return NULL;
    }
    yyjson_mut_val *root = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_val *reasoning = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, reasoning, "effort", model->effort);
    yyjson_mut_obj_add_val(doc, root, "reasoning", reasoning);
    if (model->omit_cache_key)
        yyjson_mut_obj_remove_str(root, "prompt_cache_key");
    if (model->diagnostics && model->comparison_id) {
        yyjson_mut_val *options = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, options, "comparison_response_id", model->comparison_id);
        yyjson_mut_obj_add_val(doc, root, "prompt_cache_options", options);
    }
    char *json = ah_mut_write(doc);
    yyjson_doc_free(parsed);
    ah_mut_doc_free(doc);
    if (!json) {
        ah_fail(error, AH_IO, "cannot encode evaluation request");
        return NULL;
    }
    /* Optional stdout event is consumed in memory by the evaluator, which
     * persists fingerprints, not the request content. Never includes headers. */
    if (model->trace) {
        printf("{\"event\":\"provider.request\",\"request_id\":%u,\"data\":%s}\n", request_id,
               json);
        fflush(stdout);
    }
    return json;
}

static void remember_response(eval_model *model, const char *response) {
    if (!model->diagnostics || !response)
        return;
    yyjson_doc *parsed = yyjson_read(response, strlen(response), 0);
    const char *id =
        parsed ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(parsed), "id")) : NULL;
    char *next = id && strlen(id) <= 512 ? ah_strdup(id) : NULL;
    free(model->comparison_id);
    model->comparison_id = next;
    yyjson_doc_free(parsed);
}

static ah_code complete(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                        ah_cancel_check cancelled, void *cancel_ctx, char **response,
                        ah_error *error) {
    eval_model *model = context;
    unsigned request_id = ++model->request_serial;
    char *json = prepare_request(model, request, request_id, error);
    if (!json)
        return AH_IO;
    uint64_t started = ah_now_ms();
    ah_code code = model->inner.complete(model->inner.context, json, delta, delta_ctx, cancelled,
                                         cancel_ctx, response, error);
    if (model->trace)
        response_event(*response, code, error, ah_now_ms() - started, request_id);
    if (code == AH_OK)
        remember_response(model, *response);
    free(json);
    return code;
}

typedef struct {
    void *inner;
    char *request;
    uint64_t started;
    bool reported;
    unsigned request_id;
} eval_request;

static ah_code start(void *context, const char *request, ah_delta_sink delta, void *delta_ctx,
                     ah_cancel_check cancelled, void *cancel_ctx, void **out, ah_error *error) {
    eval_model *model = context;
    eval_request *call = calloc(1, sizeof(*call));
    if (!call)
        return ah_fail(error, AH_IO, "cannot allocate evaluation request");
    call->request_id = ++model->request_serial;
    call->request = prepare_request(model, request, call->request_id, error);
    call->started = ah_now_ms();
    ah_async_provider *inner = &model->async_inner;
    ah_code code = call->request ? inner->start(inner->context, call->request, delta, delta_ctx,
                                                cancelled, cancel_ctx, &call->inner, error)
                                 : AH_IO;
    if (code != AH_OK) {
        free(call->request);
        free(call);
    } else
        *out = call;
    return code;
}

static ah_code poll(void *context, unsigned timeout, ah_error *error) {
    ah_async_provider *inner = &((eval_model *)context)->async_inner;
    return inner->poll(inner->context, timeout, error);
}

static ah_code result(void *context, void *handle, bool *done, char **response, ah_error *error) {
    eval_model *model = context;
    eval_request *call = handle;
    ah_async_provider *inner = &model->async_inner;
    ah_code code = inner->result(inner->context, call->inner, done, response, error);
    if ((*done || code != AH_OK) && !call->reported) {
        if (model->trace)
            response_event(*response, code, error, ah_now_ms() - call->started, call->request_id);
        if (code == AH_OK)
            remember_response(model, *response);
        call->reported = true;
    }
    return code;
}

static void release(void *context, void *handle) {
    ah_async_provider *inner = &((eval_model *)context)->async_inner;
    eval_request *call = handle;
    inner->release(inner->context, call->inner);
    free(call->request);
    free(call);
}

ah_async_provider eval_model_async_provider(eval_model *model) {
    return (ah_async_provider){model, start, poll, result, release};
}
ah_provider eval_model_provider(eval_model *model) {
    return (ah_provider){complete, model};
}
void eval_model_close(eval_model *model) {
    free(model->comparison_id);
    model->comparison_id = NULL;
}
