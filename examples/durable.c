/* Offline host work: one durable operation, a signal, and no provider calls. */
#include <ah/work.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy(const char *text) {
    if (!text)
        return NULL;
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    if (result)
        memcpy(result, text, size);
    return result;
}

static ah_code provider_guard(void *context, const char *request, ah_delta_sink delta,
                              void *delta_context, ah_cancel_check cancelled, void *cancel_context,
                              char **response, ah_error *error) {
    (void)request;
    (void)delta;
    (void)delta_context;
    (void)cancelled;
    (void)cancel_context;
    (void)error;
    (*(unsigned *)context)++;
    *response = NULL;
    return AH_PROTOCOL;
}

static ah_code await_ready(void *context, const ah_tool_call *call, ah_tool_result *result,
                           ah_error *error) {
    (void)context;
    (void)error;
    if (call->cancelled(call->cancel_context))
        return AH_CANCELLED;
    if (!strcmp(call->checkpoint_json, "{}"))
        result->continuation_json =
            copy("{\"checkpoint\":{\"phase\":\"waiting\"},\"wait\":{\"signal\":\"ready\"}}");
    else
        result->json = copy(call->wake_json);
    return result->continuation_json || result->json ? AH_OK : AH_IO;
}

int main(void) {
    unsigned provider_calls = 0;
    ah_config config;
    ah_config_init(&config);
    config.database_path = ":memory:";
    config.provider = (ah_provider){provider_guard, &provider_calls};
    ah_engine *engine = NULL;
    ah_error error = {0};
    ah_code code = ah_open(&config, &engine, &error);
    const ah_work_config work = {.version = 1, .replay = AH_REPLAY_SAFE};
    const ah_tool tool = {
        .name = "await_ready",
        .description = "Wait for host readiness",
        .parameters_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .effect = AH_EFFECT_READ,
        .execute = await_ready,
        .work = &work,
    };
    char session[AH_ID_CAP], operation[AH_ID_CAP];
    if (code == AH_OK)
        code = ah_register_tool(engine, &tool, &error);
    if (code == AH_OK)
        code = ah_session_create(engine, "unused-host-model", "", session, &error);
    if (code == AH_OK)
        code = ah_work_start(engine, session, "example", tool.name, "{}", operation, &error);
    if (code == AH_OK)
        code = ah_work_run(engine, session, &error);
    ah_state state;
    if (code == AH_OK)
        code = ah_session_state(engine, session, &state, &error);
    if (code == AH_OK && state != AH_WAITING_WORK)
        code = AH_PROTOCOL;
    ah_signal_result delivered;
    if (code == AH_OK)
        code = ah_signal(engine, session, operation, "ready", "{\"ok\":true}", &delivered, &error);
    if (code == AH_OK && delivered != AH_SIGNAL_DELIVERED)
        code = AH_PROTOCOL;
    if (code == AH_OK)
        code = ah_work_run(engine, session, &error);
    if (code == AH_OK)
        code = ah_session_state(engine, session, &state, &error);
    if (code == AH_OK && (state != AH_COMPLETED || provider_calls))
        code = AH_PROTOCOL;
    char *json = NULL;
    if (code == AH_OK)
        code = ah_work_json(engine, session, operation, &json, &error);
    if (json)
        puts(json);
    ah_free(json);
    if (code != AH_OK)
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
    ah_close(engine);
    return code == AH_OK ? 0 : 1;
}
