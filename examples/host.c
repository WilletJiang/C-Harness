/* A headless SDK host: credentials stay in the host; UI consumes events and state. */
#include <ah/sdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void observe(void *context, const ah_event *event) {
    (void)context;
    /* Values are borrowed for this callback. A UI thread must copy and enqueue
     * what it needs, then answer Ask/approval after ah_run has returned. */
    if (event->seq || !strcmp(event->type, "model.delta"))
        fprintf(stderr, "%s: %s\n", event->type, event->json);
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fputs("usage: ah_host MODEL DATABASE WORKSPACE PROMPT\n"
              "Set AH_ACCESS_TOKEN in the host environment.\n",
              stderr);
        return 2;
    }
    ah_sdk_config config;
    ah_sdk_config_init(&config);
    config.engine.database_path = argv[2];
    config.engine.observer = observe;
    config.responses.bearer_token = getenv("AH_ACCESS_TOKEN");
    config.local.workspace = argv[3];
    /* Image bytes expand to base64 on the wire; choose explicit host budgets. */
    config.engine.max_context_bytes = 16u * 1024u * 1024u;
    config.engine.max_output_bytes = 12u * 1024u * 1024u;
    ah_sdk *sdk = NULL;
    ah_error error = {0};
    ah_code code = ah_sdk_open(&config, &sdk, &error);
    ah_engine *engine = ah_sdk_engine(sdk);
    char session[AH_ID_CAP];
    if (code == AH_OK)
        code = ah_session_create(engine, argv[1], "Complete the user's task and verify the result.",
                                 session, &error);
    if (code == AH_OK)
        code = ah_submit(engine, session, "first-input", argv[4], &error);
    if (code == AH_OK)
        code = ah_run(engine, session, &error);
    if (code == AH_OK) {
        ah_state state;
        code = ah_session_state(engine, session, &state, &error);
        if (code == AH_OK)
            printf("session=%s state=%s\n", session, ah_state_name(state));
    }
    if (code != AH_OK)
        fprintf(stderr, "%s: %s\n", ah_code_name(code), error.message);
    ah_sdk_close(sdk);
    return code == AH_OK ? 0 : 1;
}
