#include "core.h"
#include "ah/observe.h"
#include <sys/stat.h>

#define READER_BYTES (4 * 1024 * 1024)
#define READER_EVENTS 256

struct ah_reader {
    ah_engine view;
    bool active;
};

typedef struct {
    ah_event event;
    char *storage;
} reader_event;

static ah_code reader_error(ah_reader *reader, ah_error *error, const char *message) {
    int code = sqlite3_errcode(reader->view.db);
    return ah_fail(error, code == SQLITE_BUSY || code == SQLITE_LOCKED ? AH_BUSY : AH_IO, "%s: %s",
                   message, sqlite3_errmsg(reader->view.db));
}

static ah_code reader_guard(ah_reader *reader, ah_error *error) {
    if (!reader)
        return ah_fail(error, AH_INVALID, "reader required");
    if (reader->active)
        return ah_fail(error, AH_BUSY, "reader callback is active");
    ah_error_clear(error);
    return AH_OK;
}

void ah_reader_close(ah_reader *reader) {
    if (!reader || reader->active)
        return;
    sqlite3_close(reader->view.db);
    free(reader);
}

ah_code ah_reader_open(const char *path, ah_reader **out, ah_error *error) {
    if (out)
        *out = NULL;
    if (!path || !*path || !strcmp(path, ":memory:") || !out)
        return ah_fail(error, AH_INVALID, "existing database file and reader output required");
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return ah_fail(error, AH_IO, "reader requires an existing regular database file");
    ah_reader *reader = calloc(1, sizeof(*reader));
    if (!reader)
        return ah_fail(error, AH_IO, "out of memory");
    int opened =
        sqlite3_open_v2(path, &reader->view.db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    ah_code code = opened == SQLITE_OK ? AH_OK : reader_error(reader, error, "cannot open reader");
    if (reader->view.db)
        sqlite3_busy_timeout(reader->view.db, 1000);
    reader->view.has_work = true;
    sqlite3_stmt *row = NULL;
    if (code == AH_OK)
        code = core_prepare(&reader->view, "PRAGMA user_version", &row, error);
    if (code == AH_OK) {
        if (sqlite3_step(row) != SQLITE_ROW)
            code = reader_error(reader, error, "cannot read schema version");
        else if (sqlite3_column_int(row, 0) != 8)
            code = ah_fail(error, AH_PROTOCOL, "reader requires schema version 8");
    }
    sqlite3_finalize(row);
    if (code != AH_OK) {
        ah_reader_close(reader);
        return code;
    }
    *out = reader;
    ah_error_clear(error);
    return AH_OK;
}

static ah_code snapshot_cursor(ah_reader *reader, char **json, ah_error *error) {
    sqlite3_stmt *row = NULL;
    ah_code code =
        core_prepare(&reader->view, "SELECT COALESCE(MAX(seq),0) FROM events", &row, error);
    uint64_t cursor = 0;
    if (code == AH_OK) {
        if (sqlite3_step(row) == SQLITE_ROW)
            cursor = (uint64_t)sqlite3_column_int64(row, 0);
        else
            code = reader_error(reader, error, "cannot read snapshot cursor");
    }
    sqlite3_finalize(row);
    if (code != AH_OK)
        return code;
    yyjson_doc *source = yyjson_read(*json, strlen(*json), 0);
    yyjson_mut_doc *out = source ? yyjson_doc_mut_copy(source, NULL) : NULL;
    yyjson_doc_free(source);
    if (!out)
        return ah_fail(error, AH_IO, "cannot copy session snapshot");
    bool added = yyjson_mut_obj_add_uint(out, yyjson_mut_doc_get_root(out), "cursor", cursor);
    char *encoded = added ? yyjson_mut_write(out, 0, NULL) : NULL;
    yyjson_mut_doc_free(out);
    if (!encoded)
        return ah_fail(error, AH_IO, "cannot encode session snapshot cursor");
    free(*json);
    *json = encoded;
    return AH_OK;
}

ah_code ah_reader_session_json(ah_reader *reader, const char *sid, char **json, ah_error *error) {
    if (json)
        *json = NULL;
    ah_code code = reader_guard(reader, error);
    if (code != AH_OK)
        return code;
    if (!sid || !*sid || !json)
        return ah_fail(error, AH_INVALID, "session and JSON output required");
    code = core_sql(&reader->view, "BEGIN", error);
    if (code == AH_OK)
        code = ah_session_json(&reader->view, sid, json, error);
    if (code == AH_OK)
        code = snapshot_cursor(reader, json, error);
    if (code == AH_OK)
        code = core_sql(&reader->view, "COMMIT", error);
    if (code != AH_OK) {
        core_rollback(&reader->view);
        free(*json);
        *json = NULL;
    }
    return code;
}

static ah_code copy_event(sqlite3_stmt *row, reader_event *out, size_t *remaining,
                          ah_error *error) {
    size_t bytes = 0;
    size_t lengths[4];
    for (int i = 0; i < 4; i++) {
        lengths[i] = sqlite3_column_type(row, i + 1) == SQLITE_NULL
                         ? 0
                         : (size_t)sqlite3_column_bytes(row, i + 1) + 1;
        if (lengths[i] > *remaining - bytes)
            return AH_LIMIT;
        bytes += lengths[i];
    }
    out->storage = malloc(bytes);
    if (!out->storage)
        return ah_fail(error, AH_IO, "out of memory copying events");
    char *next = out->storage;
    const char **fields[] = {&out->event.session_id, &out->event.operation_id, &out->event.type,
                             &out->event.json};
    for (int i = 0; i < 4; i++) {
        if (!lengths[i])
            continue;
        const char *text = (const char *)sqlite3_column_text(row, i + 1);
        if (!text)
            return ah_fail(error, AH_IO, "cannot copy stored event");
        memcpy(next, text, lengths[i]);
        *fields[i] = next;
        next += lengths[i];
    }
    out->event.seq = (uint64_t)sqlite3_column_int64(row, 0);
    *remaining -= bytes;
    return AH_OK;
}

ah_code ah_reader_events(ah_reader *reader, const char *sid, uint64_t after, unsigned limit,
                         ah_observer observer, void *context, uint64_t *last, ah_error *error) {
    if (last)
        *last = after;
    ah_code code = reader_guard(reader, error);
    if (code != AH_OK)
        return code;
    if (!observer || !last || !limit || limit > READER_EVENTS || (sid && !*sid))
        return ah_fail(error, AH_INVALID, "observer, cursor and event limit 1..256 required");
    if (after >= INT64_MAX)
        return AH_OK;
    reader_event *events = calloc(limit, sizeof(*events));
    if (!events)
        return ah_fail(error, AH_IO, "out of memory copying events");
    sqlite3_stmt *row = NULL;
    code = core_prepare(&reader->view,
                        sid ? "SELECT seq,session_id,operation_id,type,json FROM events "
                              "WHERE session_id=?2 AND seq>?1 ORDER BY seq LIMIT ?3"
                            : "SELECT seq,session_id,operation_id,type,json FROM events "
                              "WHERE seq>?1 ORDER BY seq LIMIT ?3",
                        &row, error);
    unsigned count = 0;
    size_t remaining = READER_BYTES;
    if (code == AH_OK) {
        sqlite3_bind_int64(row, 1, (sqlite3_int64)after);
        if (sid)
            sqlite3_bind_text(row, 2, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(row, 3, (int)limit);
        int step = SQLITE_DONE;
        while (count < limit && (step = sqlite3_step(row)) == SQLITE_ROW) {
            code = copy_event(row, &events[count], &remaining, error);
            if (code == AH_LIMIT) {
                code = count ? AH_OK : ah_fail(error, AH_LIMIT, "first event exceeds 4 MiB batch");
                break;
            }
            if (code != AH_OK)
                break;
            count++;
        }
        if (code == AH_OK && step != SQLITE_DONE && step != SQLITE_ROW)
            code = reader_error(reader, error, "cannot read events");
    }
    sqlite3_finalize(row);
    if (code == AH_OK) {
        reader->active = true;
        for (unsigned i = 0; i < count; i++) {
            observer(context, &events[i].event);
            *last = events[i].event.seq;
        }
        reader->active = false;
    }
    for (unsigned i = 0; i < limit; i++)
        free(events[i].storage);
    free(events);
    return code;
}
