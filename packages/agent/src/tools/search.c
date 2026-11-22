#include "search.h"
#include "../providers/http.h"
#include "ah/jobs.h"

static const char schema[] =
    "{\"type\":\"object\",\"properties\":{\"queries\":{\"type\":\"array\","
    "\"minItems\":1,\"maxItems\":100,\"items\":{\"type\":\"string\",\"minLength\":1,"
    "\"maxLength\":600}},\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10}},"
    "\"required\":[\"queries\"],\"additionalProperties\":false}";

static void text_field(yyjson_mut_doc *d, yyjson_mut_val *hit, const char *key, yyjson_val *value,
                       size_t limit, bool *truncated) {
    const char *p = yyjson_get_str(value);
    size_t n = yyjson_get_len(value);
    if (!p)
        return;
    if (n > limit) {
        ah_utf8_prefix(p, limit, &n);
        *truncated = true;
    }
    yyjson_mut_obj_add_strncpy(d, hit, key, p, n);
}

ah_code search_parse_hits(search_query *q, unsigned count, size_t budget, ah_error *error) {
    yyjson_doc *src = q->body ? yyjson_read(q->body, q->used, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(src), *rows = yyjson_obj_get(root, "results");
    if (!yyjson_is_arr(rows)) {
        q->error = "invalid_response";
        yyjson_doc_free(src);
        return AH_OK;
    }
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *hits = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, hits);
    size_t used = 2, index, total;
    yyjson_val *row;
    yyjson_arr_foreach(rows, index, total, row) {
        if (yyjson_mut_arr_size(hits) == count)
            break;
        yyjson_val *url = yyjson_obj_get(row, "url");
        const char *link = yyjson_get_str(url);
        if (!link || strlen(link) != yyjson_get_len(url) || strlen(link) > 8192 ||
            (strncmp(link, "https://", 8) && strncmp(link, "http://", 7))) {
            q->truncated = true;
            continue;
        }
        yyjson_mut_val *hit = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, hit, "url", link);
        text_field(d, hit, "title", yyjson_obj_get(row, "title"), 256, &q->truncated);
        yyjson_val *snippet = yyjson_obj_get(row, "snippet");
        if (!yyjson_is_str(snippet) || !yyjson_get_len(snippet))
            snippet = yyjson_obj_get(row, "description");
        text_field(d, hit, "snippet", snippet, 512, &q->truncated);
        yyjson_val *published = yyjson_obj_get(row, "published_at");
        if (yyjson_is_str(published) && yyjson_get_len(published) <= 64)
            yyjson_mut_obj_add_strncpy(d, hit, "published_at", yyjson_get_str(published),
                                       yyjson_get_len(published));
        size_t bytes = 0;
        char *encoded = yyjson_mut_val_write(hit, 0, &bytes);
        if (!encoded) {
            ah_mut_doc_free(d);
            yyjson_doc_free(src);
            return ah_fail(error, AH_IO, "cannot encode search hit");
        }
        free(encoded);
        if (bytes + 1 > budget - used) {
            q->truncated = true;
            break;
        }
        yyjson_mut_arr_append(hits, hit);
        used += bytes + 1;
    }
    q->hits = ah_mut_write(d);
    ah_mut_doc_free(d);
    yyjson_doc_free(src);
    return q->hits ? AH_OK : ah_fail(error, AH_IO, "cannot encode search results");
}

static char *receipt(search_query *qs, const unsigned *order, unsigned n,
                     const search_stats *stats) {
    yyjson_mut_doc *d = ah_mut_doc_new();
    yyjson_mut_val *root = yyjson_mut_obj(d), *rows = yyjson_mut_obj_add_arr(d, root, "results");
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_val *summary = yyjson_mut_obj_add_obj(d, root, "stats");
    yyjson_mut_obj_add_uint(d, summary, "attempts", stats->attempts);
    yyjson_mut_obj_add_uint(d, summary, "rate_limits", stats->rate_limits);
    yyjson_mut_obj_add_uint(d, summary, "peak_in_flight", stats->peak_in_flight);
    for (unsigned i = 0; i < n; i++) {
        search_query *q = &qs[order[i]];
        yyjson_mut_val *row = yyjson_mut_arr_add_obj(d, rows);
        yyjson_mut_obj_add_str(d, row, "query", q->query);
        if (q->error) {
            yyjson_mut_obj_add_str(d, row, "error", q->error);
            yyjson_mut_obj_add_uint(d, row, "attempts", q->attempts);
            if (q->transport != CURLE_OK)
                yyjson_mut_obj_add_int(d, row, "curl_code", q->transport);
            if (q->http_status)
                yyjson_mut_obj_add_int(d, row, "http_status", q->http_status);
            if (q->retry_ms)
                yyjson_mut_obj_add_uint(d, row, "retry_after_ms", q->retry_ms);
        } else {
            yyjson_mut_obj_add_val(d, row, "hits", yyjson_mut_raw(d, q->hits));
            if (q->truncated)
                yyjson_mut_obj_add_bool(d, row, "truncated", true);
        }
    }
    char *out = ah_mut_write(d);
    ah_mut_doc_free(d);
    return out;
}

static ah_code execute(void *ctx, const ah_tool_call *call, ah_tool_result *out, ah_error *error) {
    yyjson_doc *args = NULL;
    ah_code code = ah_schema_parse(schema, call->arguments_json, &args, error);
    if (code != AH_OK)
        return code;
    yyjson_val *root = yyjson_doc_get_root(args), *queries = yyjson_obj_get(root, "queries");
    unsigned count = (unsigned)yyjson_get_uint(yyjson_obj_get(root, "count"));
    unsigned n = (unsigned)yyjson_arr_size(queries), unique = 0, order[SEARCH_MAX];
    search_query qs[SEARCH_MAX] = {0};
    search_stats stats = {0};
    size_t reserved = 96;
    for (unsigned i = 0; i < n; i++) {
        yyjson_val *v = yyjson_arr_get(queries, i);
        const char *q = yyjson_get_str(v);
        if (strlen(q) != yyjson_get_len(v)) {
            code = ah_fail(error, AH_INVALID, "search queries cannot contain NUL");
            break;
        }
        size_t wire = 0;
        char *quoted = yyjson_val_write(v, 0, &wire);
        if (!quoted) {
            code = ah_fail(error, AH_IO, "cannot size search query");
            break;
        }
        free(quoted);
        reserved += wire + 110;
        unsigned j = 0;
        while (j < unique && strcmp(qs[j].query, q))
            j++;
        order[i] = j;
        if (j == unique) {
            qs[unique++].query = q;
            if (q[strspn(q, " \t\v\r\n\f")] == 0)
                qs[j].error = "invalid_query";
        }
    }
    if (code == AH_OK && reserved + n * 2 > call->output_limit)
        code = ah_fail(error, AH_LIMIT, "output limit too small for this search batch");
    if (code == AH_OK)
        code = search_transfer(ctx, call, qs, unique, count ? count : 3,
                               (call->output_limit - reserved) / n, &stats, error);
    if (code == AH_OK) {
        out->json = receipt(qs, order, n, &stats);
        if (!out->json)
            code = ah_fail(error, AH_IO, "cannot encode web search receipt");
    }
    for (unsigned i = 0; i < unique; i++)
        free(qs[i].hits);
    yyjson_doc_free(args);
    return code;
}

void search_close(ah_search *s) {
    if (!s)
        return;
    free(s->endpoint);
    free(s);
}

ah_code search_open(ah_engine *e, const ah_search_config *c, ah_search **out, ah_error *error) {
    const char *endpoint = c->endpoint ? c->endpoint : "https://api.keenable.ai/v1/search/public";
    bool loopback = false;
    ah_code code = AH_OK;
    if (c->endpoint) {
        code = ah_http_init(error);
        if (code != AH_OK)
            return code;
        if (!ah_http_endpoint(endpoint, c->allow_loopback_http, &loopback))
            return ah_fail(error, AH_INVALID, "invalid web search endpoint");
    }
    ah_search *s = calloc(1, sizeof(*s));
    if (!s)
        return ah_fail(error, AH_IO, "cannot initialize web search");
    s->endpoint = ah_strdup(endpoint);
    s->loopback = loopback;
    const ah_tool tool = {
        .name = "web_run",
        .parameters_json = schema,
        .description =
            "Search the public web via Keenable. Batch independent queries. Choose only as many "
            "queries as needed (1-100). Results follow input order with source URLs, titles, "
            "snippets or per-query errors. count is hits per query (default 3).",
        .effect = AH_EFFECT_READ,
        .execute = execute,
        .context = s,
        .parallel_safe = true};
    code = s->endpoint ? ah_register_background_tool(e, &tool, error)
                       : ah_fail(error, AH_IO, "cannot configure web search");
    if (code == AH_OK)
        *out = s;
    else
        search_close(s);
    return code;
}
