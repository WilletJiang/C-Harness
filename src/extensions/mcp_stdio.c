#include "mcp_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void close_pair(int fds[2]) {
    for (unsigned i = 0; i < 2; ++i)
        if (fds[i] >= 0)
            close(fds[i]);
}

static bool prepare_pair(int pair[2], bool socket) {
    if (socket ? socketpair(AF_UNIX, SOCK_STREAM, 0, pair) : pipe(pair))
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (pair[i] < 3) {
            int next = fcntl(pair[i], F_DUPFD_CLOEXEC, 3);
            if (next < 0)
                goto failed;
            close(pair[i]);
            pair[i] = next;
        }
        if (fcntl(pair[i], F_SETFD, FD_CLOEXEC))
            goto failed;
    }
    if (fcntl(pair[0], F_SETFL, O_NONBLOCK))
        goto failed;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (socket && setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)))
        goto failed;
#endif
    return true;
failed:
    close_pair(pair);
    pair[0] = pair[1] = -1;
    return false;
}

static void environment_free(char **environment) {
    if (environment) {
        for (char **p = environment; *p; ++p) {
            mcp_wipe(*p);
            free(*p);
        }
        free(environment);
    }
}

static bool environment_set(char **environment, const char *name, const char *value) {
    size_t n = strlen(name), v = strlen(value), index = 0;
    if (!n || n > 256 || strchr(name, '=') || v > 65536)
        return false;
    while (environment[index] &&
           (strncmp(environment[index], name, n) || environment[index][n] != '='))
        ++index;
    if (index >= 255)
        return false;
    char *entry = malloc(n + v + 2);
    if (!entry)
        return false;
    snprintf(entry, n + v + 2, "%s=%s", name, value);
    mcp_wipe(environment[index]);
    free(environment[index]);
    environment[index] = entry;
    return true;
}

static char **make_environment(yyjson_val *root, ah_error *error) {
    char **environment = calloc(256, sizeof(char *));
    if (!environment)
        goto failed;
    const char *safe[] = {"PATH", "HOME", "USER", "LOGNAME", "TMPDIR", "LANG"};
    if (!environment_set(environment, "PATH", "/usr/bin:/bin"))
        goto failed;
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); ++i) {
        const char *value = getenv(safe[i]);
        if (value && !environment_set(environment, safe[i], value))
            goto failed;
    }
    yyjson_val *vars = yyjson_obj_get(root, "env_vars");
    if (vars && (!yyjson_is_arr(vars) || yyjson_arr_size(vars) > 128))
        goto failed;
    size_t i, count;
    yyjson_val *item, *key;
    if (vars)
        yyjson_arr_foreach(vars, i, count, item) {
            const char *name = yyjson_get_str(item), *value = name ? getenv(name) : NULL;
            if (!name || !value || !environment_set(environment, name, value)) {
                ah_fail(error, AH_AUTH,
                        "Configured MCP process environment variable is missing or invalid");
                environment_free(environment);
                return NULL;
            }
        }
    yyjson_val *env = yyjson_obj_get(root, "env");
    if (env && (!yyjson_is_obj(env) || yyjson_obj_size(env) > 128))
        goto failed;
    if (env)
        yyjson_obj_foreach(env, i, count, key, item) {
            const char *value = yyjson_get_str(item);
            if (!value || !environment_set(environment, yyjson_get_str(key), value))
                goto failed;
        }
    return environment;
failed:
    environment_free(environment);
    ah_fail(error, AH_INVALID, "Invalid MCP process environment");
    return NULL;
}

static int spawn_command(pid_t *pid, const char *command, posix_spawn_file_actions_t *actions,
                         posix_spawnattr_t *attr, char **argv, char **env) {
    if (strchr(command, '/'))
        return posix_spawn(pid, command, actions, attr, argv, env);
    const char *path = "/usr/bin:/bin";
    for (char **p = env; *p; ++p)
        if (!strncmp(*p, "PATH=", 5))
            path = *p + 5;
    int rc = ENOENT;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, ':');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        /* Empty PATH segments mean cwd. Resolve there through the spawn cwd action. */
        size_t size = n + strlen(command) + 3;
        char *file = malloc(size);
        if (!file)
            return ENOMEM;
        snprintf(file, size, "%.*s/%s", (int)(n ? n : 1), n ? part : ".", command);
        rc = posix_spawn(pid, file, actions, attr, argv, env);
        free(file);
        if (!rc || (rc != ENOENT && rc != ENOTDIR && rc != EACCES))
            break;
        if (!end)
            break;
        part = end + 1;
    }
    return rc;
}

ah_code mcp_stdio_start(mcp_client *c, ah_error *error) {
    yyjson_val *root = yyjson_doc_get_root(c->config);
    const char *command = yyjson_get_str(yyjson_obj_get(root, "command"));
    yyjson_val *args = yyjson_obj_get(root, "args"), *cwd = yyjson_obj_get(root, "cwd");
    if ((args && (!yyjson_is_arr(args) || yyjson_arr_size(args) > 128)) ||
        (cwd && !yyjson_is_str(cwd)))
        return ah_fail(error, AH_INVALID, "Invalid MCP process arguments or directory");
    char *argv[130] = {(char *)command};
    size_t i, count;
    yyjson_val *arg;
    if (args)
        yyjson_arr_foreach(args, i, count, arg) {
            argv[i + 1] = (char *)yyjson_get_str(arg);
            if (!argv[i + 1] || strlen(argv[i + 1]) > 65536)
                return ah_fail(error, AH_INVALID, "Invalid MCP process arguments");
        }
    char **environment = make_environment(root, error);
    if (!environment)
        return error ? error->code : AH_INVALID;
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    int rc = 0;
    if (!prepare_pair(in, true) || !prepare_pair(out, false) || !prepare_pair(err, false)) {
        rc = errno;
        goto done;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    rc = posix_spawn_file_actions_init(&actions);
    if (rc)
        goto done;
    rc = posix_spawnattr_init(&attr);
    if (rc) {
        posix_spawn_file_actions_destroy(&actions);
        goto done;
    }
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    if (!(rc = posix_spawnattr_setflags(&attr, flags)) &&
        !(rc = posix_spawnattr_setpgroup(&attr, 0)) &&
        !(rc = posix_spawnattr_setsigmask(&attr, &mask)))
        rc = posix_spawnattr_setsigdefault(&attr, &defaults);
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (!rc && cwd)
        rc = posix_spawn_file_actions_addchdir_np(&actions, yyjson_get_str(cwd));
#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, in[1], STDIN_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);
#ifdef __APPLE__
    int descriptors[] = {in[0], in[1], out[0], out[1], err[0], err[1]};
    for (size_t index = 0; !rc && index < 6; ++index)
        rc = posix_spawn_file_actions_addclose(&actions, descriptors[index]);
#elif defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    if (!rc)
        rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#else
    rc = ENOTSUP;
#endif
    if (!rc)
        rc = spawn_command(&c->pid, command, &actions, &attr, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (!rc) {
        c->input = in[0];
        c->output = out[0];
        c->diagnostic = err[0];
        in[0] = out[0] = err[0] = -1;
    }
done:
    close_pair(in);
    close_pair(out);
    close_pair(err);
    environment_free(environment);
    return rc ? ah_fail(error, AH_IO, "Unable to launch MCP server process") : AH_OK;
}

static void drain_diagnostics(mcp_client *c) {
    char buffer[4096];
    for (unsigned i = 0; i < 8 && c->diagnostic >= 0; ++i) {
        ssize_t n = read(c->diagnostic, buffer, sizeof(buffer));
        if (!n) {
            close(c->diagnostic);
            c->diagnostic = -1;
        } else if (n < 0 && errno != EINTR)
            break;
    }
}

ah_code mcp_stdio_send(mcp_exchange *x, const char *wire) {
    size_t length = strlen(wire), offset = 0;
    while (offset <= length) {
        if (mcp_stop(x))
            return x->error.code;
        drain_diagnostics(x->client);
        mcp_yield(x);
        if (mcp_stop(x))
            return x->error.code;
        const char *bytes = offset < length ? wire + offset : "\n";
        size_t remaining = offset < length ? length - offset : 1;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t n = send(x->client->input, bytes, remaining, flags);
        if (n > 0) {
            x->sent = true;
            offset += (size_t)n;
        } else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd fds[] = {{x->client->input, POLLOUT, 0},
                                   {x->client->diagnostic, POLLIN, 0}};
            if (poll(fds, 2, 10) < 0 && errno != EINTR)
                return ah_fail(&x->error, AH_IO, "MCP pipe polling failed");
        } else
            return ah_fail(&x->error, AH_IO, "MCP server input closed");
    }
    return AH_OK;
}

ah_code mcp_stdio_exchange(mcp_exchange *x, const char *wire) {
    ah_code code = mcp_stdio_send(x, wire);
    mcp_client *c = x->client;
    while (!code && !x->done) {
        if ((code = mcp_stop(x)))
            break;
        drain_diagnostics(c);
        mcp_yield(x);
        char *newline = c->pending.length ? memchr(c->pending.data, '\n', c->pending.length) : NULL;
        if (newline) {
            size_t length = (size_t)(newline - c->pending.data);
            if (length && c->pending.data[length - 1] == '\r')
                --length;
            char *reply = NULL;
            code = mcp_message(x, c->pending.data, length, &reply);
            size_t consumed = (size_t)(newline - c->pending.data) + 1;
            memmove(c->pending.data, c->pending.data + consumed, c->pending.length - consumed);
            c->pending.length -= consumed;
            if (!code && reply)
                code = mcp_stdio_send(x, reply);
            free(reply);
            continue;
        }
        char buffer[8192];
        ssize_t n = read(c->output, buffer, sizeof(buffer));
        if (n > 0) {
            if ((size_t)n > c->max_bytes - x->received)
                code = ah_fail(&x->error, AH_LIMIT, "MCP response byte limit exceeded");
            else {
                x->received += (size_t)n;
                code = mcp_buffer_add(&c->pending, buffer, (size_t)n, c->max_bytes, &x->error);
            }
        } else if (!n)
            code = ah_fail(&x->error, AH_IO, "MCP server output closed");
        else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd fds[] = {{c->output, POLLIN, 0}, {c->diagnostic, POLLIN, 0}};
            if (poll(fds, 2, 10) < 0 && errno != EINTR)
                code = ah_fail(&x->error, AH_IO, "MCP pipe polling failed");
        } else
            code = ah_fail(&x->error, AH_IO, "MCP server read failed");
    }
    return code;
}

void mcp_stdio_close(mcp_client *c) {
    if (c->input >= 0)
        close(c->input);
    c->input = -1;
    if (c->pid > 0) {
        /* Terminate the group even if its leader already exited: descendants may own pipes. */
        kill(-c->pid, SIGTERM);
        uint64_t until = ah_now_ms() + 50;
        bool reaped = false;
        while (ah_now_ms() < until) {
            drain_diagnostics(c);
            if (!reaped) {
                pid_t result = waitpid(c->pid, NULL, WNOHANG);
                reaped = result == c->pid || (result < 0 && errno == ECHILD);
            }
            if (kill(-c->pid, 0) < 0 && errno == ESRCH)
                break;
            poll(NULL, 0, 5);
        }
        kill(-c->pid, SIGKILL);
        if (!reaped)
            while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {
            }
    }
    c->pid = 0;
    if (c->output >= 0)
        close(c->output);
    if (c->diagnostic >= 0)
        close(c->diagnostic);
    c->output = c->diagnostic = -1;
    mcp_buffer_free(&c->pending);
}
