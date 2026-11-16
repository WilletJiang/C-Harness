#include "ah/sdk.h"
#include "core/core.h"

struct ah_sdk {
    ah_engine *engine;
    ah_responses *responses;
    ah_local_tools *local;
};

void ah_sdk_config_init(ah_sdk_config *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    ah_config_init(&config->engine);
    ah_responses_config_init(&config->responses);
    config->local.approve_writes = true;
    ah_agents_config_init(&config->agents);
    config->agents.max_active = 0;
    ah_context_config_init(&config->context);
    ah_extensions_config_init(&config->extensions);
}

void ah_sdk_close(ah_sdk *sdk) {
    if (!sdk)
        return;
    ah_close(sdk->engine);
    ah_local_tools_close(sdk->local);
    ah_responses_close(sdk->responses);
    free(sdk);
}

ah_code ah_sdk_open(const ah_sdk_config *config, ah_sdk **out, ah_error *error) {
    if (out)
        *out = NULL;
    if (!config || !out || config->check_count > 16 || (config->check_count && !config->checks) ||
        (!config->local.workspace &&
         (config->local.enable_exec || config->local.enable_workflows || config->check_count)))
        return ah_fail(error, AH_INVALID, "SDK configuration and explicit tool workspace required");
    ah_sdk *sdk = calloc(1, sizeof(*sdk));
    if (!sdk)
        return ah_fail(error, AH_IO, "cannot allocate SDK");
    ah_config engine = config->engine;
    ah_agents_config agents = config->agents;
    ah_code code = AH_OK;
    if (!engine.provider.complete) {
        if (agents.max_active && (agents.provider.start || agents.provider.poll ||
                                  agents.provider.result || agents.provider.release))
            code =
                ah_fail(error, AH_INVALID, "custom async provider requires custom engine provider");
        if (code == AH_OK)
            code = ah_responses_open(&config->responses, &sdk->responses, error);
        if (code == AH_OK) {
            engine.provider = ah_responses_provider(sdk->responses);
            agents.provider = ah_responses_async_provider(sdk->responses);
        }
    }
    if (code == AH_OK && config->local.workspace)
        code = ah_local_tools_open(&config->local, &sdk->local, error);
    for (size_t i = 0; code == AH_OK && i < config->check_count; i++)
        code = ah_local_tools_add_check(sdk->local, &config->checks[i], error);
    if (code == AH_OK)
        code = ah_open(&engine, &sdk->engine, error);
    if (code == AH_OK && sdk->local)
        code = ah_local_tools_register(sdk->local, sdk->engine, error);
    if (code == AH_OK && config->automatic_context)
        code = ah_context_enable(sdk->engine, &config->context, error);
    if (code == AH_OK && agents.max_active)
        code = ah_agents_enable(sdk->engine, &agents, error);
    const ah_extensions_config *extensions = &config->extensions;
    if (code == AH_OK &&
        (extensions->enable_skills || extensions->enable_mcp || extensions->enable_plugins)) {
        ah_extensions_config effective = *extensions;
        if (!effective.workspace)
            effective.workspace = config->local.workspace;
        code = ah_extensions_enable(sdk->engine, &effective, error);
    }
    if (code != AH_OK) {
        ah_sdk_close(sdk);
        return code;
    }
    *out = sdk;
    ah_error_clear(error);
    return AH_OK;
}

ah_engine *ah_sdk_engine(ah_sdk *sdk) {
    return sdk ? sdk->engine : NULL;
}
ah_local_tools *ah_sdk_local_tools(ah_sdk *sdk) {
    return sdk ? sdk->local : NULL;
}
ah_code ah_sdk_set_token(ah_sdk *sdk, const char *token, ah_error *error) {
    if (!sdk || !sdk->responses)
        return ah_fail(error, AH_INVALID, "SDK has no owned Responses provider");
    ah_code code = core_guard(sdk->engine, error);
    return code == AH_OK ? ah_responses_set_token(sdk->responses, token, error) : code;
}
