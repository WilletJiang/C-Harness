#include "search.h"
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <strings.h>
#include <time.h>

/* Keenable publishes the hourly reset as UTC ISO 8601, including optional
 * fractional seconds. Round up so we do not restart before that boundary. */
static time_t reset_time(const char *value) {
    struct tm tm = {0};
    char *end = strptime(value, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end)
        return 0;
    bool fraction = *end == '.';
    if (fraction) {
        ++end;
        if (!isdigit((unsigned char)*end))
            return 0;
        while (isdigit((unsigned char)*end))
            ++end;
    }
    return !strcmp(end, "Z") ? timegm(&tm) + fraction : 0;
}

size_t search_header(char *data, size_t size, size_t count, void *context) {
    size_t n = size * count;
    search_headers *h = context;
    if (n >= 5 && !memcmp(data, "HTTP/", 5))
        *h = (search_headers){0};
    if (n >= 128)
        return n;
    const char *colon = memchr(data, ':', n);
    if (!colon)
        return n;
    const char *v = colon + 1, *end = data + n;
    while (v < end && isspace((unsigned char)*v))
        v++;
    while (end > v && isspace((unsigned char)end[-1]))
        end--;
    char value[128];
    memcpy(value, v, (size_t)(end - v));
    value[end - v] = 0;
    size_t name = (size_t)(colon - data);
#define HEADER(s) (name == sizeof(s) - 1 && !strncasecmp(data, s, name))
    if (HEADER("X-RateLimit-Reset")) {
        h->reset = reset_time(value);
    } else if (HEADER("X-RateLimit-Remaining") || HEADER("Retry-After")) {
        char *tail;
        unsigned long num = strtoul(value, &tail, 10);
        bool numeric = isdigit((unsigned char)value[0]) && !*tail && num <= UINT_MAX;
        if (HEADER("X-RateLimit-Remaining")) {
            h->has_remaining = numeric;
            if (numeric)
                h->remaining = (unsigned)num;
        } else if (numeric)
            h->retry_ms = (uint64_t)num * 1000;
        else {
            time_t until = curl_getdate(value, NULL), wall = time(NULL);
            if (until > wall)
                h->retry_ms = (uint64_t)(until - wall) * 1000;
        }
    }
#undef HEADER
    return n;
}

/* A single IP allowance is shared by all engines and simultaneous batches in
 * this process. Count attempts, including retries, at admission. HTTP 429 is
 * authoritative for traffic outside this process. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static search_quota quota;

uint64_t search_reserve(search_quota *q, uint64_t now) {
    uint64_t until = q->blocked;
    if (!q->remaining && until < q->reported_until)
        until = q->reported_until;
    if (q->recorded) {
        unsigned previous = (q->next + SEARCH_HOURLY - 1) % SEARCH_HOURLY;
        unsigned interval = q->spacing_ms ? q->spacing_ms : 1000 / SEARCH_RPS;
        uint64_t paced = q->starts[previous] + interval;
        if (until < paced)
            until = paced;
    }
    if (q->recorded == SEARCH_HOURLY) {
        uint64_t hourly = q->starts[q->next] + 3600000;
        if (until < hourly)
            until = hourly;
    }
    if (until > now)
        return until - now;
    if (q->active >= SEARCH_MAX)
        return 20;
    q->starts[q->next] = now;
    q->next = (q->next + 1) % SEARCH_HOURLY;
    if (q->recorded < SEARCH_HOURLY)
        q->recorded++;
    if (q->reported_until > now)
        q->remaining--;
    q->active++;
    return 0;
}

uint64_t search_admit(void) {
    pthread_mutex_lock(&lock);
    uint64_t wait = search_reserve(&quota, ah_now_ms());
    pthread_mutex_unlock(&lock);
    return wait;
}

void search_release(const search_headers *h, long status, unsigned attempt) {
    pthread_mutex_lock(&lock);
    uint64_t now = ah_now_ms();
    quota.active--;
    if (h && h->has_remaining && h->reset) {
        time_t wall = time(NULL);
        if (h->reset > wall && (now >= quota.reported_until || h->remaining <= quota.remaining)) {
            /* A delayed reply cannot replenish credit reserved by later starts. */
            uint64_t seconds = (uint64_t)(h->reset - wall);
            quota.reported_until = now + (seconds < 3600 ? seconds : 3600) * 1000;
            quota.remaining = h->remaining;
        }
    }
    if (status == 429 || status == 503 || status == 529 || (h && h->retry_ms && status >= 500)) {
        if (quota.blocked <= now && status == 429) {
            /* Starts reach the upstream after variable network delays. Give
             * the shared IP headroom, then cautiously reclaim its capacity. */
            unsigned interval = quota.spacing_ms ? quota.spacing_ms : 1000 / SEARCH_RPS;
            quota.spacing_ms = interval < 500 ? interval + interval / 2 : 750;
            quota.successes = 0;
        }
        uint64_t wait = h && h->retry_ms ? h->retry_ms : 1000u << (attempt ? attempt - 1 : 0);
        if (quota.blocked < now + wait)
            quota.blocked = now + wait;
    } else if (status == 200 && now >= quota.blocked && quota.spacing_ms > 1000 / SEARCH_RPS) {
        if (++quota.successes >= SEARCH_RPS) {
            unsigned excess = quota.spacing_ms - 1000 / SEARCH_RPS;
            quota.spacing_ms -= (excess + 1) / 2;
            quota.successes = 0;
        }
    }
    pthread_mutex_unlock(&lock);
}
