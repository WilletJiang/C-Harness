#include "http.h"
#include <curl/curl.h>
#include <pthread.h>
#include <strings.h>

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_init_code;
static void init_curl(void) {
    curl_init_code = curl_global_init(CURL_GLOBAL_DEFAULT);
}

ah_code ah_http_init(ah_error *error) {
    pthread_once(&curl_once, init_curl);
    if (curl_init_code != CURLE_OK)
        return ah_fail(error, AH_IO, "curl initialization failed");
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (!(version->features & CURL_VERSION_ASYNCHDNS))
        return ah_fail(error, AH_INVALID,
                       "curl must support asynchronous DNS for bounded cancellation");
    return AH_OK;
}

static bool url_part_present(CURLU *url, CURLUPart part) {
    char *value = NULL;
    bool present = curl_url_get(url, part, &value, 0) == CURLUE_OK;
    curl_free(value);
    return present;
}

static bool literal_loopback(const char *endpoint) {
    const char *authority = strstr(endpoint, "://");
    if (!authority)
        return false;
    authority += 3;
    const char *allowed[] = {"127.0.0.1", "[::1]", "localhost"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        size_t length = strlen(allowed[i]);
        if (!strncasecmp(authority, allowed[i], length) &&
            (!authority[length] || authority[length] == ':' || authority[length] == '/'))
            return true;
    }
    return false;
}

bool ah_http_endpoint(const char *endpoint, bool allow_loopback_http, bool *loopback) {
    if (!endpoint || strlen(endpoint) > 8192 || strchr(endpoint, '?') || strchr(endpoint, '#'))
        return false;
    for (const unsigned char *p = (const unsigned char *)endpoint; *p; p++)
        if (*p <= 0x20 || *p == 0x7f)
            return false;
    CURLU *url = curl_url();
    char *scheme = NULL, *host = NULL;
    bool valid = false;
    if (!url || curl_url_set(url, CURLUPART_URL, endpoint, 0))
        goto done;
    if (curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
        curl_url_get(url, CURLUPART_HOST, &host, 0))
        goto done;
    if (url_part_present(url, CURLUPART_USER) || url_part_present(url, CURLUPART_PASSWORD) ||
        url_part_present(url, CURLUPART_OPTIONS) || url_part_present(url, CURLUPART_QUERY) ||
        url_part_present(url, CURLUPART_FRAGMENT))
        goto done;
    *loopback =
        !strcmp(host, "127.0.0.1") || !strcasecmp(host, "localhost") || !strcmp(host, "[::1]");
    valid = !strcasecmp(scheme, "https") || (!strcasecmp(scheme, "http") && allow_loopback_http &&
                                             *loopback && literal_loopback(endpoint));
    /* Older curl versions resolve localhost through externally configured DNS. */
    if (!strcasecmp(scheme, "http") && !strcasecmp(host, "localhost") &&
        curl_version_info(CURLVERSION_NOW)->version_num < 0x074e00)
        valid = false;
done:
    curl_free(scheme);
    curl_free(host);
    if (url)
        curl_url_cleanup(url);
    return valid;
}
