/* Isolated VM for one flow. Only the owner can settle outstanding tool promises. */
#include "../vendor/quickjs/quickjs.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define INPUT_LIMIT (16u * 1024u * 1024u)
#define OUTPUT_LIMIT (4u * 1024u * 1024u)
static const char bootstrap[] =
#include "prelude.inc"
    ;

static char *receive(size_t *size) {
    uint32_t header;
    if (fread(&header, sizeof header, 1, stdin) != 1)
        return NULL;
    *size = ntohl(header);
    if (!*size || *size > INPUT_LIMIT)
        return NULL;
    char *data = malloc(*size + 1);
    if (!data || fread(data, 1, *size, stdin) != *size) {
        free(data);
        return NULL;
    }
    data[*size] = 0;
    return data;
}

static int reply(JSContext *ctx, JSValue value) {
    size_t n = 0;
    const char *json = JS_ToCStringLen(ctx, &n, value);
    uint32_t header = htonl((uint32_t)n);
    int rc = !json || !n || n > OUTPUT_LIMIT || fwrite(&header, sizeof header, 1, stdout) != 1 ||
             fwrite(json, 1, n, stdout) != n || fflush(stdout);
    JS_FreeCString(ctx, json);
    return rc;
}

static int exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, exc);
    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    if (message)
        JS_SetPropertyStr(ctx, obj, "error", JS_NewStringLen(ctx, message, strnlen(message, 2048)));
    JSValue encoded = JS_JSONStringify(ctx, obj, JS_UNDEFINED, JS_UNDEFINED);
    int rc = !message || JS_IsException(encoded) || reply(ctx, encoded);
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, encoded);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, exc);
    return rc;
}

/* Charge CPU per active phase. A helper orphaned in JS still has a hard stop. */
static int timer(int seconds) {
    struct itimerval limit = {.it_value = {.tv_sec = seconds}};
    return setitimer(ITIMER_PROF, &limit, NULL);
}

static JSValue request(JSContext *ctx) {
    size_t n = 0;
    char *data = receive(&n);
    if (!data)
        return JS_UNDEFINED;
    if (timer(5)) {
        free(data);
        return JS_ThrowInternalError(ctx, "Cannot set flow CPU limit");
    }
    JSValue value = JS_ParseJSON(ctx, data, n, "flow request");
    free(data);
    return value;
}

static int run(JSContext *ctx) {
    JSValue factory = JS_Eval(ctx, bootstrap, sizeof(bootstrap) - 1, "flow", JS_EVAL_TYPE_GLOBAL);
    JSValue input = request(ctx);
    if (JS_IsUndefined(input)) {
        JS_FreeValue(ctx, factory);
        return 1;
    }
    JSValue step = JS_EXCEPTION;
    if (!JS_IsException(factory) && !JS_IsException(input))
        step = JS_Call(ctx, factory, JS_UNDEFINED, 1, &input);
    JS_FreeValue(ctx, factory);
    JS_FreeValue(ctx, input);
    if (JS_IsException(step))
        return exception(ctx);
    int rc = 0;
    JSValue wake = JS_UNDEFINED;
    for (;;) {
        JSContext *job = NULL;
        int pending;
        while ((pending = JS_ExecutePendingJob(JS_GetRuntime(ctx), &job)) > 0) {
        }
        if (pending < 0) {
            rc = exception(job ? job : ctx);
            break;
        }
        JSValue v = JS_Call(ctx, step, JS_UNDEFINED, JS_IsUndefined(wake) ? 0 : 1, &wake);
        JS_FreeValue(ctx, wake);
        wake = JS_UNDEFINED;
        if (JS_IsException(v)) {
            rc = exception(ctx);
            break;
        }
        if (JS_IsUndefined(v))
            continue;
        rc = reply(ctx, v);
        JS_FreeValue(ctx, v);
        rc = rc || timer(0);
        if (rc)
            break;
        wake = request(ctx);
        if (JS_IsUndefined(wake))
            break;
        if (JS_IsException(wake)) {
            rc = 1;
            break;
        }
    }
    JS_FreeValue(ctx, wake);
    JS_FreeValue(ctx, step);
    return rc;
}

int main(void) {
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPROF);
    if (sigaction(SIGPROF, &action, NULL) || sigprocmask(SIG_UNBLOCK, &mask, NULL))
        return 2;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt)
        return 2;
    JS_SetMemoryLimit(rt, 64u * 1024u * 1024u);
    JS_SetMaxStackSize(rt, 512u * 1024u);
    JSContext *ctx = JS_NewContext(rt);
    int rc = ctx ? run(ctx) : 2;
    if (ctx)
        JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}
