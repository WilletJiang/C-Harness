#include "parallel_test.h"
#include <assert.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int parallel_worker_child(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], "--crash-read-window"))
        return -1;
    fixture f = {.names = {"read", "background"},
                 .count = 2,
                 .database_path = argv[2],
                 .barrier_count = 2,
                 .crash_on_capture = true};
    unsigned slow = (unsigned)atoi(argv[4]);
    assert(slow == 1 || slow == 2);
    f.wait_captured[slow] = slow == 1 ? 2 : 1;
    open_fixture(&f, 2, 2, 10000);
    ah_error error = {0};
    ah_run(f.engine, argv[3], &error);
    return 88;
}

void parallel_crash_cases(const char *executable) {
    for (unsigned slow = 1; slow <= 2; slow++) {
        char database[] = "/tmp/ah-read-window-recovery-XXXXXX", lock[256];
        int fd = mkstemp(database);
        assert(fd >= 0 && close(fd) == 0);
        snprintf(lock, sizeof(lock), "%s.lock", database);
        fixture f = {.names = {"read", "background"}, .count = 2, .database_path = database};
        open_fixture(&f, 2, 2, 10000);
        char sid[AH_ID_CAP];
        ah_error error = {0};
        ok(ah_session_create(f.engine, "mock", "", sid, &error), &error);
        ok(ah_submit(f.engine, sid, "command", "run tools", &error), &error);
        ah_config config = f.engine->config;
        ah_close(f.engine);
        f.engine = NULL;
        char *args[] = {
            (char *)executable, "--crash-read-window", database, sid, slow == 1 ? "1" : "2", NULL};
        pid_t pid;
        assert(posix_spawn(&pid, executable, NULL, NULL, args, environ) == 0);
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        /* Reopen without initializing the fixture's synchronization twice. */
        ok(ah_open(&config, &f.engine, &error), &error);
        assert(scalar(&f, "SELECT state FROM sessions") == AH_STOPPED);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE status='done'") == 2);
        assert(scalar(&f, "SELECT COUNT(*) FROM operations WHERE is_error=1") == 1);
        assert(scalar(&f, "SELECT COUNT(*) FROM events WHERE type='tool.received'") == 1);
        assert_receipt_order(&f, 2);
        close_fixture(&f);
        assert(unlink(database) == 0 && unlink(lock) == 0);
    }
}
