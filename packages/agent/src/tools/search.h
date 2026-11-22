#ifndef AH_SEARCH_H
#define AH_SEARCH_H
#include "../internal.h"
#include <curl/curl.h>
#include <time.h>

enum { SEARCH_MAX = 100, SEARCH_HOURLY = 1000, SEARCH_RPS = 10 };
typedef struct ah_search {
    char *endpoint;
    bool loopback;
} ah_search;

typedef struct {
    uint64_t starts[SEARCH_HOURLY], blocked, reported_until;
    unsigned next, recorded, active, remaining, spacing_ms, successes;
} search_quota;

typedef struct {
    unsigned attempts, peak_in_flight, rate_limits;
} search_stats;

typedef struct {
    uint64_t retry_ms;
    time_t reset;
    unsigned remaining;
    bool has_remaining;
} search_headers;

typedef struct {
    const char *query;
    char *hits;
    const char *error;
    int http_status;
    CURLcode transport;
    unsigned attempts;
    uint64_t ready_at, retry_ms;
    bool truncated;
    CURL *easy;
    char *body;
    size_t used, capacity;
    bool oversized;
    search_headers headers;
} search_query;

ah_code search_open(ah_engine *, const ah_search_config *, ah_search **, ah_error *);
void search_close(ah_search *);
size_t search_header(char *, size_t, size_t, void *);
/* Zero delay reserves one slot; every reservation requires exactly one release. */
uint64_t search_reserve(search_quota *, uint64_t now);
uint64_t search_admit(void);
void search_release(const search_headers *, long status, unsigned attempt);
ah_code search_transfer(const ah_search *, const ah_tool_call *, search_query *, unsigned, unsigned,
                        size_t, search_stats *, ah_error *);
ah_code search_parse_hits(search_query *, unsigned, size_t, ah_error *);
#endif
