#include "mcp_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <strings.h>

typedef struct http_transfer {
    mcp_exchange *exchange;
    CURL *easy;
    struct curl_slist *headers;
    mcp_buffer body, line, event;
    long status;
    size_t header_bytes;
    enum { TYPE_NONE, TYPE_JSON, TYPE_SSE } content_type;
    bool auxiliary, saw_cr, content_seen;
    const char *upload;
    size_t upload_length, uploaded;
    char *wire;
    char *replies[16];
    size_t reply_count;
} http_transfer;

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static CURLcode curl_status;

static void initialize_curl(void) {
    curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static ah_code event_message(http_transfer *t) {
    if (!t->event.length)
        return AH_OK;
    /* SSE removes the final data-line newline; empty priming events carry no RPC. */
    --t->event.length;
    if (!t->event.length)
        return AH_OK;
    char *reply = NULL;
    ah_code code = mcp_message(t->exchange, t->event.data, t->event.length, &reply);
    t->event.length = 0;
    if (!code && reply) {
        if (t->reply_count == 16) {
            free(reply);
            return ah_fail(&t->exchange->error, AH_LIMIT, "MCP server request limit exceeded");
        }
        t->replies[t->reply_count++] = reply;
    } else
        free(reply);
    return code;
}

static ah_code event_line(http_transfer *t) {
    ah_code code = AH_OK;
    if (!t->line.length)
        code = event_message(t);
    else if (t->line.length >= 5 && !memcmp(t->line.data, "data:", 5)) {
        size_t skip = t->line.length > 5 && t->line.data[5] == ' ' ? 6 : 5;
        code = mcp_buffer_add(&t->event, t->line.data + skip, t->line.length - skip,
                              t->exchange->client->max_bytes, &t->exchange->error);
        if (!code)
            code = mcp_buffer_add(&t->event, "\n", 1, t->exchange->client->max_bytes,
                                  &t->exchange->error);
    }
    t->line.length = 0;
    return code;
}

static size_t receive_body(char *bytes, size_t size, size_t count, void *context) {
    http_transfer *t = context;
    mcp_exchange *x = t->exchange;
    if (!size || count > SIZE_MAX / size || mcp_stop(x))
        return 0;
    size_t length = size * count;
    if (length > x->client->max_bytes - x->received) {
        ah_fail(&x->error, AH_LIMIT, "MCP response byte limit exceeded");
        return 0;
    }
    x->received += length;
    if (t->status < 200 || t->status >= 300)
        return length;
    if (t->auxiliary || !x->id) {
        ah_fail(&x->error, AH_PROTOCOL, "MCP notification response must be empty");
        return 0;
    }
    if (t->content_type == TYPE_JSON)
        return mcp_buffer_add(&t->body, bytes, length, x->client->max_bytes, &x->error) ? 0
                                                                                        : length;
    if (t->content_type != TYPE_SSE) {
        ah_fail(&x->error, AH_PROTOCOL, "Expected MCP JSON or event-stream content type");
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        char ch = bytes[i];
        if (t->saw_cr && ch == '\n') {
            t->saw_cr = false;
            continue;
        }
        t->saw_cr = ch == '\r';
        ah_code code = (ch == '\r' || ch == '\n')
                           ? event_line(t)
                           : mcp_buffer_add(&t->line, &ch, 1, x->client->max_bytes, &x->error);
        if (code)
            return 0;
    }
    return length;
}

static size_t receive_header(char *bytes, size_t size, size_t count, void *context) {
    http_transfer *t = context;
    mcp_exchange *x = t->exchange;
    if (!size || count > SIZE_MAX / size || mcp_stop(x))
        return 0;
    size_t length = size * count;
    if (length > 65536 - t->header_bytes) {
        ah_fail(&x->error, AH_LIMIT, "MCP HTTP header limit exceeded");
        return 0;
    }
    t->header_bytes += length;
    if (length > 5 && !memcmp(bytes, "HTTP/", 5)) {
        const char *space = memchr(bytes, ' ', length);
        if (!space || bytes + length - space < 4 || space[1] < '1' || space[1] > '5' ||
            space[2] < '0' || space[2] > '9' || space[3] < '0' || space[3] > '9') {
            ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP HTTP status line");
            return 0;
        }
        t->status = (space[1] - '0') * 100 + (space[2] - '0') * 10 + space[3] - '0';
        t->content_type = TYPE_NONE;
        t->content_seen = false;
    } else {
        const char *colon = memchr(bytes, ':', length);
        if (!colon)
            return length;
        size_t key_len = (size_t)(colon - bytes), start = key_len + 1, end = length;
        while (start < end && (bytes[start] == ' ' || bytes[start] == '\t'))
            ++start;
        while (end > start && (bytes[end - 1] == '\r' || bytes[end - 1] == '\n' ||
                               bytes[end - 1] == ' ' || bytes[end - 1] == '\t'))
            --end;
        size_t value_len = end - start;
        if (key_len == 12 && !strncasecmp(bytes, "Content-Type", 12)) {
            if (t->content_seen) {
                ah_fail(&x->error, AH_PROTOCOL, "Duplicate MCP content type");
                return 0;
            }
            t->content_seen = true;
            if (value_len >= 16 && !strncasecmp(bytes + start, "application/json", 16) &&
                (value_len == 16 || bytes[start + 16] == ';'))
                t->content_type = TYPE_JSON;
            else if (value_len >= 17 && !strncasecmp(bytes + start, "text/event-stream", 17) &&
                     (value_len == 17 || bytes[start + 17] == ';'))
                t->content_type = TYPE_SSE;
        } else if (key_len == 14 && !strncasecmp(bytes, "MCP-Session-Id", 14)) {
            if (!value_len || value_len > 1024) {
                ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP session identifier");
                return 0;
            }
            for (size_t i = start; i < end; ++i)
                if ((unsigned char)bytes[i] < 0x21 || (unsigned char)bytes[i] > 0x7e) {
                    ah_fail(&x->error, AH_PROTOCOL, "Invalid MCP session identifier");
                    return 0;
                }
            mcp_client *c = x->client;
            if (c->session &&
                (strlen(c->session) != value_len || memcmp(c->session, bytes + start, value_len))) {
                ah_fail(&x->error, AH_PROTOCOL, "MCP session identifier changed unexpectedly");
                return 0;
            }
            if (!c->session) {
                c->session = malloc(value_len + 1);
                if (!c->session) {
                    ah_fail(&x->error, AH_IO, "MCP allocation failed");
                    return 0;
                }
                memcpy(c->session, bytes + start, value_len);
                c->session[value_len] = 0;
            }
        }
    }
    return length;
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return mcp_stop(((http_transfer *)context)->exchange) != AH_OK;
}

static int debug(CURL *easy, curl_infotype type, char *bytes, size_t size, void *context) {
    (void)easy;
    http_transfer *t = context;
    if (!t->auxiliary && type == CURLINFO_HEADER_OUT && size >= 5 && !memcmp(bytes, "POST ", 5))
        t->exchange->sent = true;
    return 0; /* Suppress curl's diagnostic output, including credentials. */
}

static size_t upload(char *bytes, size_t size, size_t count, void *context) {
    http_transfer *t = context;
    if (!size || count > SIZE_MAX / size || mcp_stop(t->exchange))
        return CURL_READFUNC_ABORT;
    size_t length = size * count;
    if (length > t->upload_length - t->uploaded)
        length = t->upload_length - t->uploaded;
    memcpy(bytes, t->upload + t->uploaded, length);
    t->uploaded += length;
    return length;
}

static int refuse_rewind(void *context, curl_off_t offset, int origin) {
    (void)offset;
    (void)origin;
    http_transfer *t = context;
    ah_fail(&t->exchange->error, AH_NETWORK, "MCP HTTP automatic replay refused");
    return CURL_SEEKFUNC_FAIL;
}

static ah_code setup(http_transfer *t, const char *wire) {
    mcp_exchange *x = t->exchange;
    mcp_client *c = x->client;
    ah_code code = AH_OK;
    for (struct curl_slist *p = c->headers; p; p = p->next) {
        struct curl_slist *next = curl_slist_append(t->headers, p->data);
        if (!next)
            return ah_fail(&x->error, AH_IO, "MCP allocation failed");
        t->headers = next;
    }
    if (!(code = mcp_header_add(&t->headers, "Content-Type", "application/json", &x->error)) &&
        !(code = mcp_header_add(&t->headers, "Accept", "application/json, text/event-stream",
                                &x->error)))
        code = mcp_header_add(&t->headers, "MCP-Protocol-Version", c->version, &x->error);
    if (!code && c->session)
        code = mcp_header_add(&t->headers, "MCP-Session-Id", c->session, &x->error);
    if (code)
        return code;
    struct curl_slist *next = curl_slist_append(t->headers, "Expect:");
    if (!next)
        return ah_fail(&x->error, AH_IO, "MCP allocation failed");
    t->headers = next;
    const char *url = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(c->config), "url"));
#define SET(option, value)                                                                         \
    do {                                                                                           \
        if (curl_easy_setopt(t->easy, option, value) != CURLE_OK)                                  \
            return ah_fail(&x->error, AH_IO, "MCP HTTP setup failed");                             \
    } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_POST, 1L);
    /* POSTFIELDS is rewindable by libcurl and can silently replay an uncertain effect.
     * A streaming body with a failing seek callback makes every upload single-use. */
    t->upload = wire;
    t->upload_length = strlen(wire);
    SET(CURLOPT_POSTFIELDS, NULL);
    SET(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(wire));
    SET(CURLOPT_READFUNCTION, upload);
    SET(CURLOPT_READDATA, t);
    SET(CURLOPT_SEEKFUNCTION, refuse_rewind);
    SET(CURLOPT_SEEKDATA, t);
    SET(CURLOPT_HTTPHEADER, t->headers);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_MAXREDIRS, 0L);
    SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, progress);
    SET(CURLOPT_XFERINFODATA, t);
    SET(CURLOPT_WRITEFUNCTION, receive_body);
    SET(CURLOPT_WRITEDATA, t);
    SET(CURLOPT_HEADERFUNCTION, receive_header);
    SET(CURLOPT_HEADERDATA, t);
    SET(CURLOPT_DEBUGFUNCTION, debug);
    SET(CURLOPT_DEBUGDATA, t);
    SET(CURLOPT_VERBOSE, 1L);
    SET(CURLOPT_PRIVATE, t);
    uint64_t now = ah_now_ms();
    SET(CURLOPT_TIMEOUT_MS, (long)(x->deadline > now ? x->deadline - now : 1));
    if (c->loopback)
        SET(CURLOPT_PROXY, "");
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https,http");
#else
    SET(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTPS | CURLPROTO_HTTP));
#endif
#undef SET
    return AH_OK;
}

static void transfer_free(http_transfer *t) {
    mcp_headers_free(t->headers);
    mcp_buffer_free(&t->body);
    mcp_buffer_free(&t->line);
    mcp_buffer_free(&t->event);
    free(t->wire);
    for (size_t i = 0; i < t->reply_count; ++i)
        free(t->replies[i]);
}

static ah_code completed(http_transfer *t, CURLcode status) {
    mcp_exchange *x = t->exchange;
    if (x->error.code)
        return x->error.code;
    if (status == CURLE_OPERATION_TIMEDOUT)
        return ah_fail(&x->error, AH_LIMIT, "MCP HTTP deadline exceeded");
    if (status != CURLE_OK)
        return ah_fail(&x->error, AH_NETWORK, "MCP HTTP transport failed");
    if (t->status == 401 || t->status == 403)
        return ah_fail(&x->error, AH_AUTH,
                       "MCP HTTP authentication rejected; configure credentials explicitly");
    if (t->status < 200 || t->status >= 300)
        return ah_fail(&x->error, AH_NETWORK, "MCP HTTP request failed with status %ld", t->status);
    if (t->auxiliary || !x->id)
        return t->status == 202
                   ? AH_OK
                   : ah_fail(&x->error, AH_PROTOCOL, "Expected MCP notification acknowledgement");
    if (t->content_type == TYPE_JSON) {
        char *reply = NULL;
        ah_code code = mcp_message(x, t->body.data, t->body.length, &reply);
        free(reply);
        if (code)
            return code;
    }
    return x->done
               ? AH_OK
               : ah_fail(&x->error, AH_PROTOCOL, "MCP response ended without a matching result");
}

ah_code mcp_http_exchange(mcp_exchange *x, const char *wire) {
    pthread_once(&curl_once, initialize_curl);
    if (curl_status != CURLE_OK)
        return ah_fail(&x->error, AH_IO, "MCP HTTP initialization failed");
    mcp_client *c = x->client;
    if (!c->easy)
        c->easy = curl_easy_init();
    if (!c->multi)
        c->multi = curl_multi_init();
    CURLM *multi = c->multi;
    http_transfer main = {.exchange = x, .easy = c->easy},
                  auxiliary = {.exchange = x, .auxiliary = true};
    ah_code code = !c->easy || !multi ? ah_fail(&x->error, AH_IO, "MCP HTTP allocation failed")
                                      : setup(&main, wire);
    bool main_added = false, auxiliary_added = false, main_done = false;
    size_t replied = 0;
    if (!code) {
        main_added = curl_multi_add_handle(multi, main.easy) == CURLM_OK;
        if (!main_added)
            code = ah_fail(&x->error, AH_IO, "MCP HTTP setup failed");
    }
    while (!code && (!main_done || auxiliary_added || replied < main.reply_count)) {
        if ((code = mcp_stop(x)))
            break;
        mcp_yield(x);
        if ((code = mcp_stop(x)))
            break;
        int running = 0;
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            code = ah_fail(&x->error, AH_NETWORK, "MCP HTTP polling failed");
            break;
        }
        int messages;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &messages))) {
            if (message->msg != CURLMSG_DONE)
                continue;
            http_transfer *t = NULL;
            curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &t);
            code = completed(t, message->data.result);
            curl_multi_remove_handle(multi, t->easy);
            if (t == &main) {
                main_done = true;
                main_added = false;
            } else {
                auxiliary_added = false;
                curl_easy_cleanup(auxiliary.easy);
                transfer_free(&auxiliary);
                auxiliary = (http_transfer){.exchange = x, .auxiliary = true};
            }
            if (code)
                break;
        }
        if (!code && !auxiliary_added && replied < main.reply_count) {
            auxiliary.easy = curl_easy_init();
            auxiliary.wire = main.replies[replied];
            main.replies[replied++] = NULL;
            code = auxiliary.easy ? setup(&auxiliary, auxiliary.wire)
                                  : ah_fail(&x->error, AH_IO, "MCP allocation failed");
            if (!code) {
                auxiliary_added = curl_multi_add_handle(multi, auxiliary.easy) == CURLM_OK;
                if (!auxiliary_added)
                    code = ah_fail(&x->error, AH_IO, "MCP HTTP setup failed");
            }
        }
        /* Servers may keep SSE open after the terminal result. Stop reading immediately. */
        if (!code && x->done && main_added && main.content_type == TYPE_SSE) {
            curl_multi_remove_handle(multi, main.easy);
            main_added = false;
            main_done = true;
        }
        if (!code && (!main_done || auxiliary_added) &&
            curl_multi_poll(multi, NULL, 0, 10, NULL) != CURLM_OK)
            code = ah_fail(&x->error, AH_NETWORK, "MCP HTTP polling failed");
    }
    if (main_added)
        curl_multi_remove_handle(multi, main.easy);
    if (auxiliary_added)
        curl_multi_remove_handle(multi, auxiliary.easy);
    curl_easy_cleanup(auxiliary.easy);
    transfer_free(&auxiliary);
    transfer_free(&main);
    if (c->easy)
        curl_easy_reset(c->easy); /* Reset borrowed pointers; the connection cache survives. */
    return code;
}

void mcp_http_close(mcp_client *c) {
    curl_easy_cleanup(c->easy);
    c->easy = NULL;
    curl_multi_cleanup(c->multi);
    c->multi = NULL;
    mcp_wipe(c->session);
    free(c->session);
    c->session = NULL;
}
