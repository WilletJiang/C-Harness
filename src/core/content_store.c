#include "content_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

ah_code core_content_schema(ah_engine *e, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "SELECT 1 FROM pragma_table_info('commands') WHERE name='kind'", &s, err);
    int step = rc == AH_OK ? sqlite3_step(s) : SQLITE_ERROR;
    sqlite3_finalize(s);
    if (rc == AH_OK && step == SQLITE_DONE)
        rc = core_sql(e, "ALTER TABLE commands ADD COLUMN kind INTEGER NOT NULL DEFAULT 0", err);
    else if (rc == AH_OK && step != SQLITE_ROW)
        rc = ah_fail(err, AH_IO, "cannot inspect command schema");
    if (rc == AH_OK)
        rc =
            core_sql(e,
                     "CREATE TABLE IF NOT EXISTS artifacts(id TEXT PRIMARY KEY,mime TEXT NOT NULL,"
                     "data BLOB NOT NULL CHECK(length(data)>0 AND length(data)<=8388608));"
                     "CREATE TABLE IF NOT EXISTS artifact_refs(session_id TEXT NOT NULL REFERENCES "
                     "sessions(id),artifact_id TEXT NOT NULL REFERENCES artifacts(id),"
                     "PRIMARY KEY(session_id,artifact_id));",
                     err);
    return rc;
}
ah_code content_blob_save(ah_engine *e, const char *sid, const content_blob *blob, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e, "INSERT OR IGNORE INTO artifacts(id,mime,data) VALUES(?,?,?)", &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, blob->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, blob->mime, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(s, 3, blob->data, (int)blob->size, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    if (rc == AH_OK)
        rc = core_prepare(e,
                          "INSERT OR IGNORE INTO artifact_refs(session_id,artifact_id) "
                          "VALUES(?,?)",
                          &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, blob->id, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    return rc;
}
ah_code content_reference(ah_engine *e, const char *sid, const char *id, bool image,
                          ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e,
                     "SELECT a.mime FROM artifacts a JOIN artifact_refs r ON a.id=r.artifact_id "
                     "WHERE r.session_id=? AND a.id=?",
                     &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(s);
        if (step != SQLITE_ROW)
            rc = ah_fail(err, step == SQLITE_DONE ? AH_NOT_FOUND : AH_IO,
                         "artifact is unavailable to this session");
        else if (image && !content_image_mime((const char *)sqlite3_column_text(s, 0)))
            rc = ah_fail(err, AH_INVALID, "input_image requires image MIME");
    }
    sqlite3_finalize(s);
    return rc;
}
ah_code core_content_save(ah_engine *e, const char *sid, const core_content *content,
                          ah_error *err) {
    ah_code rc = AH_OK;
    for (content_blob *blob = content->blobs; blob && rc == AH_OK; blob = blob->next)
        rc = content_blob_save(e, sid, blob, err);
    return rc;
}
ah_code core_content_fork(ah_engine *e, const char *source, const char *dest, ah_error *err) {
    sqlite3_stmt *s = NULL;
    ah_code rc =
        core_prepare(e,
                     "INSERT INTO artifact_refs(session_id,artifact_id) SELECT ?,artifact_id "
                     "FROM artifact_refs WHERE session_id=?",
                     &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, dest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, source, -1, SQLITE_TRANSIENT);
        rc = core_done(e, s, err);
    }
    return rc;
}
static bool same_file(const struct stat *a, const struct stat *b) {
#if defined(__APPLE__)
    struct timespec am = a->st_mtimespec, bm = b->st_mtimespec;
    struct timespec ac = a->st_ctimespec, bc = b->st_ctimespec;
#else
    struct timespec am = a->st_mtim, bm = b->st_mtim;
    struct timespec ac = a->st_ctim, bc = b->st_ctim;
#endif
    return a->st_size == b->st_size && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
static ah_code read_artifact(const char *path, const char *mime, content_blob *blob,
                             ah_error *err) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat before, after;
    if (fd < 0)
        return ah_fail(err, AH_IO, "cannot open artifact regular file");
    ah_code rc = AH_OK;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode))
        rc = ah_fail(err, AH_INVALID, "artifact must be a regular file");
    else if (before.st_size <= 0)
        rc = ah_fail(err, AH_INVALID, "artifact must not be empty");
    else if ((uint64_t)before.st_size > AH_ARTIFACT_MAX_BYTES)
        rc = ah_fail(err, AH_LIMIT, "artifact byte limit exceeded");
    if (rc == AH_OK) {
        blob->size = (size_t)before.st_size;
        blob->data = malloc(blob->size);
        blob->mime = ah_strdup(mime);
        if (!blob->data || !blob->mime)
            rc = ah_fail(err, AH_IO, "out of memory");
    }
    size_t at = 0;
    while (rc == AH_OK && at < blob->size) {
        ssize_t n = read(fd, blob->data + at, blob->size - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            rc = ah_fail(err, AH_IO, "artifact changed or could not be read");
        else
            at += (size_t)n;
    }
    if (rc == AH_OK && (fstat(fd, &after) || !same_file(&before, &after)))
        rc = ah_fail(err, AH_BUSY, "artifact changed while reading");
    close(fd);
    if (rc == AH_OK && !content_mime_valid(mime, blob->data, blob->size))
        rc = ah_fail(err, AH_INVALID, "artifact bytes do not match supported MIME");
    if (rc == AH_OK)
        content_blob_id(blob);
    return rc;
}
ah_code ah_artifact_import(ah_engine *e, const char *sid, const char *path, const char *mime,
                           char id[AH_ARTIFACT_ID_CAP], ah_error *err) {
    if (id)
        id[0] = 0;
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !path || !*path || !mime || !*mime || strlen(mime) > 63 || !id)
        return ah_fail(err, AH_INVALID, "session, artifact path, MIME and output required");
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    core_session_free(&session);
    content_blob blob = {0};
    rc = read_artifact(path, mime, &blob, err);
    uint64_t after = 0;
    if (rc == AH_OK)
        rc = core_begin(e, &after, err);
    if (rc == AH_OK)
        rc = content_blob_save(e, sid, &blob, err);
    char *receipt = rc == AH_OK ? core_object_string("artifact_id", blob.id) : NULL;
    if (rc == AH_OK && !receipt)
        rc = ah_fail(err, AH_IO, "out of memory");
    if (rc == AH_OK)
        rc = core_event(e, sid, NULL, "artifact.imported", receipt, err);
    if (rc == AH_OK)
        rc = core_commit(e, after, err);
    else if (!sqlite3_get_autocommit(e->db))
        core_rollback(e);
    if (rc == AH_OK)
        memcpy(id, blob.id, AH_ARTIFACT_ID_CAP);
    free(receipt);
    free(blob.data);
    free(blob.mime);
    return rc;
}
ah_code ah_artifacts_json(ah_engine *e, const char *sid, char **json, ah_error *err) {
    if (json)
        *json = NULL;
    ah_code rc = core_guard(e, err);
    if (rc != AH_OK)
        return rc;
    if (!sid || !json)
        return ah_fail(err, AH_INVALID, "session and output required");
    core_session session;
    rc = core_load(e, sid, &session, err);
    if (rc != AH_OK)
        return rc;
    core_session_free(&session);
    yyjson_mut_doc *d = ah_mut_doc_new();
    if (!d)
        return ah_fail(err, AH_IO, "out of memory");
    yyjson_mut_val *array = yyjson_mut_arr(d);
    yyjson_mut_doc_set_root(d, array);
    sqlite3_stmt *s = NULL;
    rc = core_prepare(e,
                      "SELECT a.id,a.mime,length(a.data) FROM artifacts a JOIN "
                      "artifact_refs r ON a.id=r.artifact_id WHERE r.session_id=? ORDER BY a.id",
                      &s, err);
    if (rc == AH_OK) {
        sqlite3_bind_text(s, 1, sid, -1, SQLITE_TRANSIENT);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            yyjson_mut_obj_add_strcpy(d, o, "artifact_id", (const char *)sqlite3_column_text(s, 0));
            yyjson_mut_obj_add_strcpy(d, o, "mime", (const char *)sqlite3_column_text(s, 1));
            yyjson_mut_obj_add_uint(d, o, "bytes", (uint64_t)sqlite3_column_int64(s, 2));
            yyjson_mut_arr_append(array, o);
        }
        if (step != SQLITE_DONE)
            rc = ah_fail(err, AH_IO, "artifact lookup failed");
    }
    sqlite3_finalize(s);
    if (rc == AH_OK && !(*json = ah_mut_write(d)))
        rc = ah_fail(err, AH_IO, "out of memory");
    ah_mut_doc_free(d);
    return rc;
}
