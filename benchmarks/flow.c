/* Paired disk/FULL benchmark. Time ah_run only; check the same file summaries
 * on the second model request. Interleave modes across independent fresh databases. */
#include <ah/sdk.h>
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { MAX_FILES = 32, MAX_BYTES = 65536, MODES = 4, MAX_RUNS = 200 };
static const char *modes[] = {"native", "flow_parallel", "flow_serial", "flow_full"};
static unsigned long long nanos(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
typedef struct {
    unsigned mode, calls, files, bytes;
    char *plan, *last;
} fixture;

static char *plan(unsigned mode, unsigned files) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d), *out = yyjson_mut_obj_add_arr(d, root, "output");
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_str(d, root, "status", "completed");
    char script[512];
    if (mode == 1)
        snprintf(script, sizeof script,
                 "return (await Promise.all(Array.from({length:%u},"
                 "(_,i)=>tools.read_file({path:'row'+i}))))"
                 ".map(r=>[r.path,r.content.length,r.content[0]])",
                 files);
    if (mode == 2)
        snprintf(script, sizeof script,
                 "let a=[];for(let i=0;i<%u;i++){let r=await "
                 "tools.read_file({path:'row'+i});a.push([r.path,r.content.length,r.content[0]])}"
                 "return a",
                 files);
    if (mode == 3)
        snprintf(script, sizeof script,
                 "return await Promise.all(Array.from({length:%u},"
                 "(_,i)=>tools.read_file({path:'row'+i})))",
                 files);
    for (unsigned i = 0; i < (mode ? 1 : files); i++) {
        char id[16], path[16];
        snprintf(id, sizeof id, "c%u", i);
        snprintf(path, sizeof path, "row%u", i);
        yyjson_mut_val *c = yyjson_mut_arr_add_obj(d, out), *a = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, c, "type", "function_call");
        yyjson_mut_obj_add_strcpy(d, c, "call_id", id);
        yyjson_mut_obj_add_str(d, c, "name", mode ? "flow" : "read_file");
        if (!mode)
            yyjson_mut_obj_add_strcpy(d, a, "path", path);
        else
            yyjson_mut_obj_add_str(d, a, "script", script);
        char *args = yyjson_mut_val_write(a, 0, NULL);
        yyjson_mut_obj_add_strcpy(d, c, "arguments", args);
        free(args);
    }
    char *json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    return json;
}
static ah_code model(void *ctx, const char *req, ah_delta_sink sink, void *sc,
                     ah_cancel_check cancel, void *cc, char **out, ah_error *err) {
    (void)sink;
    (void)sc;
    (void)cancel;
    (void)cc;
    (void)err;
    fixture *f = ctx;
    if (++f->calls > 2)
        return AH_PROTOCOL;
    if (f->calls == 2)
        f->last = strdup(req);
    *out = strdup(f->calls == 1 ? f->plan : "{\"status\":\"completed\",\"output\":[]}");
    return *out ? AH_OK : AH_IO;
}
static bool valid(fixture *f) {
    if (f->calls != 2 || !f->last)
        return false;
    yyjson_doc *d = yyjson_read(f->last, strlen(f->last), 0);
    yyjson_val *in = yyjson_obj_get(yyjson_doc_get_root(d), "input"), *item;
    size_t i, n;
    unsigned seen = 0, receipts = 0;
    bool ok = true;
    yyjson_arr_foreach(in, i, n, item) {
        if (!yyjson_equals_str(yyjson_obj_get(item, "type"), "function_call_output"))
            continue;
        yyjson_val *text = yyjson_obj_get(item, "output");
        receipts++;
        yyjson_doc *o = yyjson_read(yyjson_get_str(text), yyjson_get_len(text), 0);
        yyjson_val *body = yyjson_doc_get_root(o), *v = yyjson_obj_get(body, "result");
        ok &= yyjson_is_false(yyjson_obj_get(body, "is_error"));
        for (size_t j = 0; j < (f->mode ? yyjson_arr_size(v) : 1); j++, seen++) {
            char path[32];
            snprintf(path, sizeof path, "row%u", seen);
            bool summary = f->mode == 1 || f->mode == 2;
            yyjson_val *row = f->mode ? yyjson_arr_get(v, j) : v;
            yyjson_val *p = summary ? yyjson_arr_get(row, 0) : yyjson_obj_get(row, "path");
            yyjson_val *s = summary ? yyjson_arr_get(row, 2) : yyjson_obj_get(row, "content");
            size_t length = summary ? yyjson_get_uint(yyjson_arr_get(row, 1)) : yyjson_get_len(s);
            char expected = (char)('a' + seen % 26);
            ok &= yyjson_equals_str(p, path) && yyjson_is_str(s) && yyjson_get_len(s) > 0 &&
                  yyjson_get_str(s)[0] == expected && length == f->bytes;
            if (!summary && yyjson_is_str(s))
                for (size_t byte = 0; byte < yyjson_get_len(s); byte++)
                    ok &= yyjson_get_str(s)[byte] == expected;
        }
        yyjson_doc_free(o);
    }
    yyjson_doc_free(d);
    return ok && seen == f->files && receipts == (f->mode ? 1 : f->files);
}
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 25;
    int files = argc > 2 ? atoi(argv[2]) : 8, bytes = argc > 3 ? atoi(argv[3]) : 4096;
    bool full = argc == 5 && !strcmp(argv[4], "full");
    const unsigned compact_modes[] = {0, 1, 2}, full_modes[] = {0, 3};
    const unsigned *order = full ? full_modes : compact_modes;
    unsigned modes_count = full ? 2 : 3;
    if ((argc >= 5 && !full) || runs < 1 || runs > MAX_RUNS || files < 1 || files > MAX_FILES ||
        bytes < 1 || bytes > MAX_BYTES)
        return 2;
    char ws[] = "/tmp/ah-bench-flow-XXXXXX";
    if (!mkdtemp(ws))
        return 1;
    char paths[MAX_FILES][128], db[128], lock[140];
    snprintf(db, sizeof db, "%s/state", ws);
    snprintf(lock, sizeof lock, "%s.lock", db);
    for (int i = 0; i < files; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/row%d", ws, i);
        FILE *f = fopen(paths[i], "w");
        if (!f)
            return 1;
        for (int j = 0; j < bytes; j++)
            fputc('a' + i % 26, f);
        if (fclose(f))
            return 1;
    }
    double samples[MODES][MAX_RUNS];
    int rc = 0;
    for (int round = -3; round < runs && !rc; round++)
        for (unsigned k = 0; k < modes_count && !rc; k++) {
            unsigned mode = order[(k + (unsigned)(round + 3)) % modes_count];
            fixture f = {.mode = mode,
                         .plan = plan(mode, (unsigned)files),
                         .files = (unsigned)files,
                         .bytes = (unsigned)bytes};
            ah_sdk_config c;
            ah_sdk_config_init(&c);
            c.engine.database_path = db;
            if (bytes > 8192) {
                c.engine.max_model_tool_bytes = (size_t)bytes + 4096;
                c.engine.max_context_bytes = 4 * 1024 * 1024;
            }
            if (full) {
                size_t limit = (size_t)files * ((size_t)bytes + 1024) + 4096;
                c.engine.max_model_tool_bytes = limit;
                if (c.engine.max_output_bytes < limit)
                    c.engine.max_output_bytes = limit;
                c.engine.max_context_bytes = 4 * 1024 * 1024;
            }
            c.local.workspace = ws;
            c.engine.provider = (ah_provider){model, &f};
            ah_sdk *sdk = NULL;
            ah_error err = {0};
            char sid[AH_ID_CAP];
            ah_code status = ah_sdk_open(&c, &sdk, &err);
            ah_engine *e = ah_sdk_engine(sdk);
            if (status == AH_OK)
                status = ah_session_create(e, "offline", "", sid, &err);
            if (status == AH_OK)
                status = ah_submit(e, sid, "sample", "Summarize files", &err);
            unsigned long long start = nanos();
            if (status == AH_OK)
                status = ah_run(e, sid, &err);
            double ms = (double)(nanos() - start) / 1e6;
            if (status != AH_OK || !valid(&f)) {
                fprintf(stderr, "%s: %s invalid result\n", modes[mode], err.message);
                rc = 1;
            }
            if (round >= 0)
                samples[mode][round] = ms;
            free(f.last);
            free(f.plan);
            ah_sdk_close(sdk);
            unlink(db);
            unlink(lock);
        }
    for (unsigned k = 0; !rc && k < modes_count; k++) {
        unsigned i = order[k];
        qsort(samples[i], (size_t)runs, sizeof(double), compare);
        printf("%s files=%d bytes=%d n=%d p50_ms=%.3f p95_ms=%.3f\n", modes[i], files, bytes, runs,
               samples[i][(runs - 1) / 2], samples[i][(95 * runs + 99) / 100 - 1]);
    }
    for (int i = 0; i < files; i++)
        unlink(paths[i]);
    rmdir(ws);
    return rc;
}
