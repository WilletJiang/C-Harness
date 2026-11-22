#include "tui_app.h"
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { REPLY_CAP = 16390 };
extern char **environ;

static void release_reply(char *reply) {
    if (!reply)
        return;
    volatile unsigned char *p = (volatile unsigned char *)reply;
    for (size_t i = 0; i < REPLY_CAP; ++i)
        p[i] = 0;
    free(reply);
}

static void reap(pid_t pid) {
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        continue;
}

static ah_code start_helper(bool interactive, pid_t *pid, int *read_fd, ah_error *error) {
#ifndef AH_TUI_PYTHON
    (void)interactive;
    (void)pid;
    (void)read_fd;
    return ah_fail(error, AH_AUTH, "ChatGPT sign-in requires the optional host adapter");
#else
    int fd[2];
    if (pipe(fd))
        return ah_fail(error, AH_IO, "Cannot open the ChatGPT credential channel");
    for (unsigned i = 0; i < 2; ++i) {
        int moved = fcntl(fd[i], F_DUPFD_CLOEXEC, 4);
        if (moved < 0) {
            close(fd[0]);
            close(fd[1]);
            return ah_fail(error, AH_IO, "Cannot protect the ChatGPT credential channel");
        }
        close(fd[i]);
        fd[i] = moved;
    }
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        static char bootstrap[] =
            "import sys; sys.path.insert(0, sys.argv.pop(1)); "
            "from packages.host.tui_auth import main; raise SystemExit(main())";
        char *argv[] = {AH_TUI_PYTHON, "-I", "-B", "-c", bootstrap, AH_TUI_HOST_ROOT,
                       interactive ? "login" : "reuse", "3", NULL};
        rc = posix_spawn_file_actions_adddup2(&actions, fd[1], 3);
        for (int i = 0; !rc && i < 3; ++i)
            rc = posix_spawn_file_actions_addopen(&actions, i, "/dev/null", O_RDWR, 0);
        if (!rc)
            rc = posix_spawn(pid, AH_TUI_PYTHON, &actions, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(fd[1]);
    if (rc) {
        close(fd[0]);
        return ah_fail(error, AH_AUTH, "Cannot start the ChatGPT host adapter");
    }
    *read_fd = fd[0];
    return AH_OK;
#endif
}

static const char *read_token(char *reply, size_t used, ah_error *error) {
    if (used > 4 && reply[used - 1] == '\n' && !memcmp(reply, "ok ", 3) &&
        !memchr(reply, '\0', used)) {
        reply[used - 1] = 0;
        return reply + 3;
    }
    reply[used] = 0;
    const char *message = !strcmp(reply, "error login_required\n")
                              ? "Sign in to ChatGPT in the browser"
                          : !strcmp(reply, "error keychain\n")
                              ? "ChatGPT sign-in requires macOS Keychain and the host requirements"
                          : !strcmp(reply, "error access_denied\n")
                              ? "Browser sign-in was cancelled"
                          : !strcmp(reply, "error timeout\n")
                              ? "Browser sign-in timed out"
                          : !strcmp(reply, "error network\n")
                              ? "ChatGPT sign-in could not reach OpenAI"
                              : "ChatGPT host adapter did not return a credential";
    ah_fail(error, AH_AUTH, "%s", message);
    return NULL;
}

static ssize_t read_more(int fd, char *reply, size_t used) {
    ssize_t n;
    do {
        n = read(fd, reply + used, REPLY_CAP - 1 - used);
    } while (n < 0 && errno == EINTR);
    return n;
}

ah_code tui_oauth_start(tui_app *app, const char *model, bool interactive, ah_error *error) {
    if (app->oauth_pid || app->running)
        return ah_fail(error, AH_BUSY, "Another action is running");
    if (!model || !*model || strlen(model) >= sizeof(app->oauth_model))
        return ah_fail(error, AH_INVALID, "Enter a default model of at most 256 bytes");
    app->oauth_reply = calloc(REPLY_CAP, 1);
    if (!app->oauth_reply)
        return ah_fail(error, AH_IO, "Cannot allocate the ChatGPT credential channel");
    ah_code code = start_helper(interactive, &app->oauth_pid, &app->oauth_fd, error);
    if (code == AH_OK && fcntl(app->oauth_fd, F_SETFL, O_NONBLOCK) != 0)
        code = ah_fail(error, AH_IO, "Cannot watch the ChatGPT credential channel");
    if (code != AH_OK) {
        tui_oauth_cancel(app);
        return code;
    }
    strcpy(app->oauth_model, model);
    app->auth_error[0] = 0;
    app->auth.waiting = true;
    return AH_OK;
}

void tui_oauth_cancel(tui_app *app) {
    if (app->oauth_pid) {
        (void)kill(app->oauth_pid, SIGKILL);
        reap(app->oauth_pid);
        app->oauth_pid = 0;
    }
    if (app->oauth_fd >= 0) {
        close(app->oauth_fd);
        app->oauth_fd = -1;
    }
    release_reply(app->oauth_reply);
    app->oauth_reply = NULL;
    app->oauth_used = 0;
    app->auth.waiting = false;
}

void tui_oauth_read(tui_app *app) {
    if (!app->oauth_pid)
        return;
    ssize_t n = read_more(app->oauth_fd, app->oauth_reply, app->oauth_used);
    if (n > 0) {
        app->oauth_used += (size_t)n;
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    ah_error error = {0};
    const char *token = read_token(app->oauth_reply, app->oauth_used, &error);
    if (token)
        (void)tui_config_chatgpt(app, token, app->oauth_model, &error);
    if (n == 0) {
        reap(app->oauth_pid);
        app->oauth_pid = 0;
    }
    tui_oauth_cancel(app);
    if (error.code != AH_OK)
        snprintf(app->auth_error, sizeof(app->auth_error), "%.255s", error.message);
}

ah_code tui_oauth_refresh(ah_responses *responses, ah_error *error) {
    char *reply = calloc(REPLY_CAP, 1);
    if (!reply)
        return ah_fail(error, AH_IO, "Cannot allocate the ChatGPT credential channel");
    pid_t pid = 0;
    int fd = -1;
    ah_code code = start_helper(false, &pid, &fd, error);
    if (code == AH_OK) {
        size_t used = 0;
        ssize_t n;
        while ((n = read_more(fd, reply, used)) > 0)
            used += (size_t)n;
        close(fd);
        reap(pid);
        const char *token = read_token(reply, used, error);
        code = token ? ah_responses_set_token(responses, token, error) : AH_AUTH;
    }
    release_reply(reply);
    return code;
}
