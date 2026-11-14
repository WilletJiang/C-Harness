#include "extensions/mcp.h"
#include <signal.h>
#include <stdio.h>

typedef struct {
    uint64_t cancel_at;
    unsigned yields;
} control;

static bool cancelled(void *context) {
    control *c = context;
    return c->cancel_at && ah_now_ms() >= c->cancel_at;
}

static void yield(void *context) {
    ++((control *)context)->yields;
}

int main(void) {
    char *input = malloc(1024 * 1024);
    if (!input || !fgets(input, 1024 * 1024, stdin))
        return 2;
    yyjson_doc *doc = yyjson_read(input, strlen(input), 0);
    free(input);
    if (!doc)
        return 2;
    yyjson_val *root = yyjson_doc_get_root(doc);
    char *config = ah_json_write(yyjson_obj_get(root, "config"));
    size_t limit = (size_t)yyjson_get_uint(yyjson_obj_get(root, "limit"));
    if (!limit)
        limit = 1024 * 1024;
    ah_error error = {0};
    mcp_client *client = NULL;
    struct sigaction before, after;
    sigaction(SIGPIPE, NULL, &before);
    ah_code code = mcp_client_open(config, limit, true, &client, &error);
    free(config);
    printf("{\"open\":%d}\n", code);
    yyjson_val *requests = yyjson_obj_get(root, "requests"), *item;
    size_t i, count;
    yyjson_arr_foreach(requests, i, count, item) {
        uint64_t start = ah_now_ms();
        control state = {0};
        uint64_t cancel_ms = yyjson_get_uint(yyjson_obj_get(item, "cancel_after_ms"));
        if (cancel_ms)
            state.cancel_at = start + cancel_ms;
        if (yyjson_get_bool(yyjson_obj_get(item, "cancelled")))
            state.cancel_at = start;
        uint64_t timeout_ms = yyjson_get_uint(yyjson_obj_get(item, "deadline_ms"));
        ah_tool_call call = {.cancelled = cancelled,
                             .cancel_context = &state,
                             .yield = yield,
                             .yield_context = &state,
                             .deadline_ms = start + (timeout_ms ? timeout_ms : 3000)};
        char *params = ah_json_write(yyjson_obj_get(item, "params")), *result = NULL;
        bool sent = false;
        code = mcp_client_request(client, yyjson_get_str(yyjson_obj_get(item, "method")), params,
                                  &call, &result, &sent, &error);
        free(params);
        yyjson_mut_doc *out = ah_mut_doc_new();
        yyjson_mut_val *object = yyjson_mut_obj(out);
        yyjson_mut_doc_set_root(out, object);
        yyjson_mut_obj_add_uint(out, object, "code", code);
        yyjson_mut_obj_add_bool(out, object, "sent", sent);
        yyjson_mut_obj_add_uint(out, object, "elapsed", ah_now_ms() - start);
        yyjson_mut_obj_add_uint(out, object, "yields", state.yields);
        yyjson_mut_obj_add_str(out, object, "error", error.message);
        yyjson_doc *result_doc = result ? yyjson_read(result, strlen(result), 0) : NULL;
        yyjson_mut_obj_add_val(out, object, "result",
                               result_doc
                                   ? yyjson_val_mut_copy(out, yyjson_doc_get_root(result_doc))
                                   : yyjson_mut_null(out));
        char *line = ah_mut_write(out);
        puts(line);
        free(line);
        free(result);
        yyjson_doc_free(result_doc);
        ah_mut_doc_free(out);
    }
    mcp_client_close(client);
    sigaction(SIGPIPE, NULL, &after);
    yyjson_doc_free(doc);
    return before.sa_handler == after.sa_handler ? 0 : 3;
}
