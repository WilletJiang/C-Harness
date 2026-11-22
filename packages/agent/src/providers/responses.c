#include "responses_internal.h"
#include "http.h"
#include <limits.h>

void responses_wipe(char *value) {
    if (!value)
        return;
    volatile unsigned char *p = (volatile unsigned char *)value;
    size_t length = strlen(value);
    while (length--)
        *p++ = 0;
}

void ah_responses_config_init(ah_responses_config *config) {
    if (!config)
        return;
    *config = (ah_responses_config){.endpoint = "https://api.openai.com/v1/responses",
                                    .timeout_ms = 120000,
                                    .max_response_bytes = 32 * 1024 * 1024,
                                    .max_event_bytes = 8 * 1024 * 1024,
                                    .max_retries = 2};
}

static ah_code set_connection(ah_responses *provider, const char *endpoint, const char *token,
                              ah_error *error) {
    ah_error_clear(error);
    if (!provider)
        return ah_fail(error, AH_INVALID, "Responses provider is required");
    if (provider->requests || provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider is running");
    if (token) {
        if (!*token || strlen(token) > 16384)
            return ah_fail(error, AH_INVALID, "invalid bearer token");
        for (const unsigned char *p = (const unsigned char *)token; *p; p++)
            if (*p < 0x21 || *p > 0x7e)
                return ah_fail(error, AH_INVALID, "invalid bearer token");
    }
    bool loopback = false;
    if (endpoint && !ah_http_endpoint(endpoint, provider->config.allow_loopback_http, &loopback))
        return ah_fail(error, AH_INVALID,
                       "endpoint must be HTTPS without credentials, query, or fragment");
    char *url = endpoint ? ah_strdup(endpoint) : NULL;
    char *bearer = token ? ah_strdup(token) : NULL;
    if ((endpoint && !url) || (token && !bearer)) {
        free(url);
        responses_wipe(bearer);
        free(bearer);
        return ah_fail(error, AH_IO, "cannot allocate Responses connection");
    }
    if ((url && strcmp(provider->endpoint, url)) ||
        (bearer && (!provider->token || strcmp(provider->token, bearer))))
        responses_routes_clear(provider);
    if (url) {
        free(provider->endpoint);
        provider->endpoint = url;
        provider->config.endpoint = url;
        provider->loopback = loopback;
    }
    if (bearer) {
        responses_wipe(provider->token);
        free(provider->token);
        provider->token = bearer;
    }
    return AH_OK;
}

ah_code ah_responses_set_token(ah_responses *provider, const char *token, ah_error *error) {
    if (!token)
        return ah_fail(error, AH_INVALID, "invalid bearer token");
    return set_connection(provider, NULL, token, error);
}

ah_code ah_responses_set_endpoint(ah_responses *provider, const char *endpoint, ah_error *error) {
    return ah_responses_set_connection(provider, endpoint, NULL, error);
}

ah_code ah_responses_set_connection(ah_responses *provider, const char *endpoint,
                                    const char *bearer, ah_error *error) {
    if (!endpoint || !*endpoint)
        return ah_fail(error, AH_INVALID, "invalid Responses endpoint");
    return set_connection(provider, endpoint, bearer, error);
}

ah_code ah_responses_open(const ah_responses_config *config, ah_responses **out, ah_error *error) {
    ah_error_clear(error);
    if (out)
        *out = NULL;
    if (!config || !out || !config->timeout_ms || config->timeout_ms > LONG_MAX ||
        !config->max_event_bytes || config->max_event_bytes > config->max_response_bytes ||
        config->max_response_bytes > 256 * 1024 * 1024 || config->max_retries > 5)
        return ah_fail(error, AH_INVALID, "invalid Responses configuration");
    ah_code initialized = ah_http_init(error);
    if (initialized != AH_OK)
        return initialized;
    bool loopback = false;
    if (!ah_http_endpoint(config->endpoint, config->allow_loopback_http, &loopback))
        return ah_fail(error, AH_INVALID,
                       "endpoint must be HTTPS without credentials, query, or fragment");
    ah_responses *provider = calloc(1, sizeof(*provider));
    if (!provider)
        return ah_fail(error, AH_IO, "provider allocation failed");
    provider->config = *config;
    provider->config.bearer_token = NULL;
    provider->endpoint = ah_strdup(config->endpoint);
    provider->config.endpoint = provider->endpoint;
    provider->loopback = loopback;
    provider->multi = curl_multi_init();
    if (!provider->endpoint || !provider->multi) {
        ah_responses_close(provider);
        return ah_fail(error, AH_IO, "provider initialization failed");
    }
    if (config->bearer_token) {
        ah_code code = ah_responses_set_token(provider, config->bearer_token, error);
        if (code) {
            ah_responses_close(provider);
            return code;
        }
    }
    *out = provider;
    return AH_OK;
}

void ah_responses_close(ah_responses *provider) {
    if (!provider || provider->dispatching)
        return;
    ah_async_provider async = ah_responses_async_provider(provider);
    while (provider->requests)
        async.release(provider, provider->requests);
    curl_multi_cleanup(provider->multi);
    responses_routes_clear(provider);
    responses_wipe(provider->token);
    free(provider->token);
    free(provider->endpoint);
    free(provider);
}

ah_provider ah_responses_provider(ah_responses *provider) {
    return (ah_provider){.complete = responses_complete, .context = provider};
}
