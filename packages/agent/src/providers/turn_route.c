#include "responses_internal.h"

static void clear_header(char **header) {
    responses_wipe(*header);
    free(*header);
    *header = NULL;
}

void responses_routes_clear(ah_responses *p) {
    for (size_t i = 0; i < sizeof(p->routes) / sizeof(p->routes[0]); i++) {
        clear_header(&p->routes[i].header);
        p->routes[i].session[0] = 0;
        p->routes[i].generation++;
    }
}

void responses_route_bind(responses_request *t, const char *sid, unsigned turn) {
    ah_responses *p = t->provider;
    size_t count = sizeof(p->routes) / sizeof(p->routes[0]);
    if (!turn || strlen(sid) != AH_ID_CAP - 1)
        return;
    responses_route *slot = NULL;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(p->routes[i].session, sid)) {
            slot = &p->routes[i];
            break;
        }
    if (!slot)
        slot = &p->routes[p->next_route++ % count];
    if (slot->turn != turn || strcmp(slot->session, sid)) {
        clear_header(&slot->header);
        memcpy(slot->session, sid, AH_ID_CAP);
        slot->turn = turn;
        slot->generation++;
    }
    t->route = slot;
    t->route_generation = slot->generation;
}

void responses_route_reset(responses_request *t) {
    clear_header(&t->candidate_route);
    t->route_header_seen = false;
}

void responses_route_capture(responses_request *t, const char *value, size_t length) {
    if (!t->route || t->route_generation != t->route->generation || t->route->header)
        return;
    if (t->route_header_seen) {
        clear_header(&t->candidate_route);
        return;
    }
    t->route_header_seen = true;
    while (length && (*value == ' ' || *value == '\t')) {
        value++;
        length--;
    }
    if (length && value[length - 1] == '\n') {
        length--;
        if (length && value[length - 1] == '\r')
            length--;
    }
    while (length && (value[length - 1] == ' ' || value[length - 1] == '\t'))
        length--;
    if (!length || length > 8192)
        return;
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] > 0x7e)
            return;
    const char prefix[] = "x-codex-turn-state: ";
    t->candidate_route = malloc(sizeof(prefix) + length);
    if (t->candidate_route) {
        memcpy(t->candidate_route, prefix, sizeof(prefix) - 1);
        memcpy(t->candidate_route + sizeof(prefix) - 1, value, length);
        t->candidate_route[sizeof(prefix) - 1 + length] = 0;
    }
}

void responses_route_commit(responses_request *t) {
    if (t->route && t->route_generation == t->route->generation && !t->route->header &&
        t->status >= 200 && t->status < 300 && t->content_type) {
        t->route->header = t->candidate_route;
        t->candidate_route = NULL;
    }
    responses_route_reset(t);
}

ah_code responses_route_apply(responses_request *t) {
    if (t->route_sent || !t->route || t->route_generation != t->route->generation ||
        !t->route->header)
        return AH_OK;
    struct curl_slist *next = curl_slist_append(t->headers, t->route->header);
    if (!next)
        return ah_fail(&t->error, AH_IO, "cannot allocate turn routing header");
    t->headers = next;
    t->route_sent = true;
    return AH_OK;
}
