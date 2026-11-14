#include "core_test.h"
#include "ah/jobs.h"
#include <fcntl.h>

extern char **environ;

static ah_code crash_effect(void *context, const ah_tool_call *call, ah_tool_result *result,
                            ah_error *error) {
    (void)call;
    (void)result;
    (void)error;
    const char *marker = context;
    if (!marker)
        abort(); /* Recovery must not repeat the callback. */
    int fd = open(marker, O_WRONLY | O_CREAT | O_APPEND, 0600);
    assert(fd >= 0 && write(fd, "x", 1) == 1 && fsync(fd) == 0 && close(fd) == 0);
    _exit(77);
}

static void register_crash(ah_engine *engine, ah_effect effect, const char *marker) {
    ah_tool tool = {.name = "test_tool",
                    .description = "Interrupted background job",
                    .parameters_json =
                        "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},"
                        "\"required\":[\"value\"],\"additionalProperties\":false}",
                    .effect = effect,
                    .execute = crash_effect,
                    .context = (void *)marker};
    ah_error error = {0};
    ok(ah_register_background_tool(engine, &tool, &error), &error);
}

int job_recovery_child(int argc, char **argv) {
    if (argc != 6 || strcmp(argv[1], "--crash-job"))
        return -1;
    fixture f = {.answers = {call}};
    ah_engine *engine = open_engine(&f, argv[2], NULL);
    register_crash(engine, (ah_effect)atoi(argv[5]), argv[4]);
    ah_error error = {0};
    ah_run(engine, argv[3], &error);
    return 88;
}

void job_recovery_cases(const char *executable) {
    for (unsigned external = 0; external < 2; external++) {
        char database[] = "/tmp/ah-job-recovery-XXXXXX", marker[256], lock[256];
        int fd = mkstemp(database);
        assert(fd >= 0 && close(fd) == 0);
        snprintf(marker, sizeof(marker), "%s.effect", database);
        snprintf(lock, sizeof(lock), "%s.lock", database);
        fixture f = {.answers = {done}};
        ah_engine *engine = open_engine(&f, database, NULL);
        char session[AH_ID_CAP];
        create_input(engine, session);
        ah_close(engine);
        char *args[] = {(char *)executable,   "--crash-job", database, session, marker,
                        external ? "2" : "0", NULL};
        pid_t pid;
        assert(posix_spawn(&pid, executable, NULL, NULL, args, environ) == 0);
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        f.last_seq = 0;
        engine = open_engine(&f, database, NULL);
        register_crash(engine, external ? AH_EFFECT_EXTERNAL : AH_EFFECT_READ, NULL);
        ah_error error = {0};
        if (external) {
            state_is(engine, session, "outcome_unknown");
            assert(ah_run(engine, session, &error) == AH_UNCERTAIN);
            char *operation = pending(engine, session);
            ok(ah_resolve(engine, session, operation, "{\"reconciled\":true}", false, &error),
               &error);
            free(operation);
        } else {
            state_is(engine, session, "stopped");
            ok(ah_resume(engine, session, &error), &error);
        }
        ok(ah_run(engine, session, &error), &error);
        state_is(engine, session, "completed");
        FILE *file = fopen(marker, "rb");
        assert(file && fgetc(file) == 'x' && fgetc(file) == EOF);
        fclose(file);
        ah_close(engine);
        free(f.last_request);
        assert(unlink(database) == 0 && unlink(lock) == 0 && unlink(marker) == 0);
    }
}
