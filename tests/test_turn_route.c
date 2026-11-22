#include "providers/responses_internal.h"
#include <assert.h>
#include <stdio.h>

static const char first[] = "00000000-0000-4000-8000-000000000001";
static const char second[] = "00000000-0000-4000-8000-000000000002";

static void capture(responses_request *r, const char *token) {
    responses_route_capture(r, token, strlen(token));
    responses_route_commit(r);
}

int main(void) {
    ah_responses_config config;
    ah_responses_config_init(&config);
    config.bearer_token = "fixture";
    ah_error err = {0};
    ah_responses *p = NULL;
    assert(ah_responses_open(&config, &p, &err) == AH_OK);
    responses_request a = {.provider = p, .status = 200, .content_type = true}, b = a;
    responses_route_bind(&a, first, 1);
    responses_route_bind(&b, second, 1);
    capture(&a, " route-a\t\r\n");
    capture(&b, "route-b");
    capture(&a, "replacement");
    assert(!strcmp(a.route->header, "x-codex-turn-state: route-a"));
    assert(!strcmp(b.route->header, "x-codex-turn-state: route-b"));

    responses_request next = {.provider = p, .status = 200, .content_type = true};
    responses_route_bind(&next, first, 2);
    assert(!next.route->header);
    capture(&a, "late-old-turn");
    assert(responses_route_apply(&a) == AH_OK && !a.headers);
    assert(!next.route->header);
    responses_route_capture(&next, "one", 3);
    capture(&next, "duplicate");
    capture(&next, "bad\r\nInjected: yes");
    capture(&next, "\t\r\n");
    assert(!next.route->header);
    next.status = 503;
    capture(&next, "error-route");
    next.status = 200;
    next.content_type = false;
    capture(&next, "not-an-SSE-response");
    assert(!next.route->header);
    next.content_type = true;
    capture(&next, "turn-2");
    assert(responses_route_apply(&next) == AH_OK);
    assert(responses_route_apply(&next) == AH_OK && next.headers && !next.headers->next);
    assert(!strcmp(next.headers->data, "x-codex-turn-state: turn-2"));
    curl_slist_free_all(next.headers);

    assert(ah_responses_set_token(p, "another-account", &err) == AH_OK);
    responses_route_bind(&next, first, 2);
    assert(!next.route->header && !b.route->header);
    ah_responses_close(p);
    puts("turn route isolation, first valid header, retries and credential replacement passed");
    return 0;
}
