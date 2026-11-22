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

typedef struct {
    char *data;
    size_t used, capacity;
} model_catalog;

static size_t catalog_append(char *data, size_t size, size_t count, void *context) {
    model_catalog *catalog = context;
    const size_t limit = 2 * 1024 * 1024;
    if (size && count > (limit - catalog->used) / size)
        return 0;
    size_t bytes = size * count, needed = catalog->used + bytes + 1;
    if (needed > catalog->capacity) {
        size_t capacity = catalog->capacity ? catalog->capacity * 2 : 4096;
        if (capacity < needed)
            capacity = needed;
        if (capacity > limit + 1)
            capacity = limit + 1;
        char *next = realloc(catalog->data, capacity);
        if (!next)
            return 0;
        catalog->data = next;
        catalog->capacity = capacity;
    }
    memcpy(catalog->data + catalog->used, data, bytes);
    catalog->used += bytes;
    catalog->data[catalog->used] = 0;
    return bytes;
}

ah_code ah_responses_models(ah_responses *provider, char **json, ah_error *error) {
    if (!json)
        return ah_fail(error, AH_INVALID, "model catalog output required");
    *json = NULL;
    if (!provider || !provider->token)
        return ah_fail(error, AH_AUTH, "model catalog requires a connection");
    if (provider->requests || provider->dispatching)
        return ah_fail(error, AH_BUSY, "provider is running");
    const char *path = strchr(strstr(provider->endpoint, "://") + 3, '/');
    const char *tail = path ? strrchr(path, '/') : NULL;
    if (!tail || strcmp(tail, "/responses"))
        return ah_fail(error, AH_NOT_FOUND, "connection URL has no sibling model catalog");
    char url[8193];
    snprintf(url, sizeof(url), "%.*s/models", (int)(tail - provider->endpoint), provider->endpoint);
    CURL *easy = curl_easy_init();
    if (!easy)
        return ah_fail(error, AH_IO, "cannot start model catalog request");
    model_catalog catalog = {0};
    long status = 0;
    ah_code code = AH_IO;
    CURLcode result = CURLE_OK;
#define SET(option, value) \
    do { if (curl_easy_setopt(easy, option, value) != CURLE_OK) goto done; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, 20000L);
    SET(CURLOPT_ACCEPT_ENCODING, "");
    SET(CURLOPT_HTTPAUTH, (long)CURLAUTH_BEARER);
    SET(CURLOPT_XOAUTH2_BEARER, provider->token);
    SET(CURLOPT_WRITEFUNCTION, catalog_append);
    SET(CURLOPT_WRITEDATA, &catalog);
    if (provider->loopback)
        SET(CURLOPT_PROXY, "");
#undef SET
    result = curl_easy_perform(easy);
    if (result == CURLE_OK &&
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK) {
        if (status == 200 && catalog.used && !memchr(catalog.data, 0, catalog.used)) {
            *json = catalog.data;
            catalog.data = NULL;
            code = AH_OK;
        } else {
            code = status == 401 || status == 403 ? AH_AUTH : status == 404 ? AH_NOT_FOUND : AH_PROTOCOL;
        }
    } else {
        code = AH_NETWORK;
    }
done:
    curl_easy_cleanup(easy);
    free(catalog.data);
    if (result != CURLE_OK)
        return ah_fail(error, code, "cannot load model catalog: %s", curl_easy_strerror(result));
    if (code != AH_OK)
        return ah_fail(error, code, "cannot load model catalog (HTTP %ld)", status);
    ah_error_clear(error);
    return AH_OK;
}
