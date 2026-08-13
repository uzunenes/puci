/*
 * tools/run_shell.c - built-in run_shell tool.
 *
 * Executes a command in a child process with an explicit argv array using
 * fork + execvp. `system()` is deliberately never used (shell injection
 * vector). stdout and stderr are captured through pipes, bounded, and returned
 * with the exit code. A timeout or cancellation kills the child's process group.
 */

/* killpg() needs _XOPEN_SOURCE, which the global -D_POSIX_C_SOURCE=200809L does
 * not enable. Must precede any system header. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "puci_internal.h"
#include "tools/tools_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json/json.h"
#include "util/arena.h"
#include "util/strbuf.h"

#define CAPTURE_CAP ((size_t)1024 * 1024)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Appends to buf, capping total at CAPTURE_CAP. Returns 1 if capped. */
static int bounded_append(puci_strbuf *buf, const char *data, size_t len) {
    size_t space = len;
    if (buf->len + space > CAPTURE_CAP) {
        space = CAPTURE_CAP - buf->len;
    }
    if (space > 0) {
        puci_strbuf_append(buf, data, space);
    }
    return buf->len >= CAPTURE_CAP;
}

static int cancel_requested(const volatile sig_atomic_t *flag) {
    return flag && *flag;
}

/* Non-blocking, bounded drain of whatever is currently readable on *fd into
 * target. Closes and clears *fd on EOF or an unrecoverable read error so
 * subsequent calls (and get_fds) know it is done. Never blocks. */
static void drain_fd(int *fd, puci_strbuf *target) {
    if (*fd < 0) {
        return;
    }
    char buf[8192];
    for (;;) {
        ssize_t nr = read(*fd, buf, sizeof(buf));
        if (nr > 0) {
            bounded_append(target, buf, (size_t)nr);
        } else if (nr == 0) {
            close(*fd);
            *fd = -1;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            close(*fd);
            *fd = -1;
            break;
        }
    }
}

/* run_shell's async tool state: one child process, its two output pipes, and
 * the phase of a possible in-progress termination. Driven entirely by the
 * host's poll loop via the puci_tool_async_vtable below; nothing here blocks. */
typedef enum shell_phase {
    SHELL_RUNNING,   /* child alive; draining pipes, watching for exit/timeout/cancel */
    SHELL_TERM_WAIT, /* cancelled: SIGTERM sent, waiting up to grace before SIGKILL */
    SHELL_KILL_WAIT, /* SIGKILL sent (grace expired or hard timeout); waiting for reap */
} shell_phase;

typedef struct shell_async_state {
    pid_t pid;
    int out_fd, err_fd; /* -1 once EOF/closed */
    puci_strbuf out, err;
    shell_phase phase;
    int status;
    int timed_out;
    int was_cancelled;
    long long deadline;      /* command timeout_ms deadline (monotonic ms), -1 = none */
    long long term_deadline; /* SIGTERM->SIGKILL grace deadline while cancelling */
    long cancel_grace_ms;
    const volatile sig_atomic_t *cancel_flag;
} shell_async_state;

/* Builds the JSON result, releases the child's fds/buffers, and frees state.
 * Shared terminal path for every way the invocation can end. */
static int shell_finish(shell_async_state *st, char **out_result,
                        int *out_is_error) {
    /* One last non-blocking drain: data may have arrived between the reap and
     * this call. */
    drain_fd(&st->out_fd, &st->out);
    drain_fd(&st->err_fd, &st->err);
    if (st->out_fd >= 0) {
        close(st->out_fd);
    }
    if (st->err_fd >= 0) {
        close(st->err_fd);
    }

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res,
                           "{\"ok\":%s,\"exit_code\":%d,\"timed_out\":%s,"
                           "\"cancelled\":%s,\"stdout\":",
                           st->was_cancelled ? "false" : "true", st->status,
                           st->timed_out ? "true" : "false",
                           st->was_cancelled ? "true" : "false");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&st->out));
    puci_strbuf_appendz(&res, ",\"stderr\":");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&st->err));
    puci_strbuf_appendz(&res, "}");

    *out_result = strdup(puci_strbuf_cstr(&res));
    /* Mirrors the previous synchronous tool: failures (nonzero exit, timeout,
     * cancellation) are encoded in the JSON body's "ok" field, not is_error;
     * is_error is reserved for the invocation never having produced a result
     * at all (handled in begin()). */
    *out_is_error = 0;

    puci_strbuf_destroy(&res);
    puci_strbuf_destroy(&st->out);
    puci_strbuf_destroy(&st->err);
    free(st);
    return 1;
}

/* Starts the command: parses arguments, forks with non-blocking output pipes,
 * and returns immediately. Never blocks past the fork() call itself. */
static int shell_async_begin(void *userdata, const puci_cancel_token *cancel,
                             const char *args_json,
                             puci_tool_async_handle *out_handle,
                             char **out_result) {
    (void)userdata;
    *out_handle = NULL;
    *out_result = NULL;

    puci_arena arena;
    puci_arena_init(&arena, NULL, 0);
    pj_val *root = NULL;
    if (pj_parse(&arena, args_json, strlen(args_json), &root) != 0) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"invalid arguments\"}");
        return 1;
    }
    pj_val *cmdv = pj_get(root, "command");
    const char *command = cmdv ? pj_str(cmdv) : NULL;
    if (!command) {
        puci_arena_destroy(&arena);
        *out_result =
            strdup("{\"ok\":false,\"error\":\"missing required 'command'\"}");
        return 1;
    }

    /* Resolve argv from the optional args array. */
    pj_val *argsv = pj_get(root, "args");
    size_t nargs = argsv && argsv->type == PJ_ARR ? argsv->u.arr.len : 0;
    char **argv = (char **)calloc(nargs + 2, sizeof(char *));
    if (!argv) {
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"out of memory\"}");
        return 1;
    }
    argv[0] = (char *)command;
    for (size_t i = 0; i < nargs; i++) {
        pj_val *a = argsv->u.arr.items[i];
        if (a && a->type == PJ_STR) {
            argv[i + 1] = a->u.str.s;
        } else {
            argv[i + 1] = (char *)"";
        }
    }

    pj_val *cwv = pj_get(root, "cwd");
    const char *cwd = cwv ? pj_str(cwv) : NULL;

    /* Timeout. */
    pj_val *tov = pj_get(root, "timeout_ms");
    int timeout_ms = tov && tov->type == PJ_NUM ? (int)tov->u.num : 0;
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    /* Grace period (ms) between SIGTERM and SIGKILL when the tool is
     * cancelled. Default 2000. */
    pj_val *gv = pj_get(root, "cancel_grace_ms");
    long cancel_grace_ms = gv && gv->type == PJ_NUM ? (long)gv->u.num : 2000;
    if (cancel_grace_ms < 0) {
        cancel_grace_ms = 0;
    }

    int out_fds[2] = {-1, -1};
    int err_fds[2] = {-1, -1};
    if (pipe(out_fds) != 0 || pipe(err_fds) != 0) {
        free(argv);
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"pipe failed\"}");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_fds[0]);
        close(out_fds[1]);
        close(err_fds[0]);
        close(err_fds[1]);
        free(argv);
        puci_arena_destroy(&arena);
        *out_result = strdup("{\"ok\":false,\"error\":\"fork failed\"}");
        return 1;
    }

    if (pid == 0) {
        /* Child. Put the child in its own process group so running grandchildren
         * can be signalled together on cancel (killpg). */
        (void)setpgid(0, 0);
        close(out_fds[0]);
        close(err_fds[0]);
        if (dup2(out_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        if (dup2(err_fds[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(out_fds[1]);
        close(err_fds[1]);
        if (cwd && chdir(cwd) != 0) {
            _exit(126);
        }
        execvp(command, argv);
        _exit(127);
    }

    /* Parent. */
    close(out_fds[1]);
    close(err_fds[1]);
    free(argv);
    puci_arena_destroy(&arena);

    /* Give the child its own process group from the parent side too, closing
     * the window before the child's own setpgid() runs. */
    (void)setpgid(pid, pid);

    set_nonblocking(out_fds[0]);
    set_nonblocking(err_fds[0]);

    shell_async_state *st = (shell_async_state *)calloc(1, sizeof(*st));
    if (!st) {
        (void)killpg(pid, SIGKILL);
        int ws;
        while (waitpid(pid, &ws, 0) < 0 && errno == EINTR) {
        }
        close(out_fds[0]);
        close(err_fds[0]);
        *out_result = strdup("{\"ok\":false,\"error\":\"out of memory\"}");
        return 1;
    }
    st->pid = pid;
    st->out_fd = out_fds[0];
    st->err_fd = err_fds[0];
    puci_strbuf_init(&st->out, NULL);
    puci_strbuf_init(&st->err, NULL);
    st->phase = SHELL_RUNNING;
    st->status = -1;
    st->deadline = timeout_ms > 0 ? now_ms() + (long long)timeout_ms : -1;
    st->term_deadline = -1;
    st->cancel_grace_ms = cancel_grace_ms;
    /* Cancel signalling: the token may be NULL if the engine did not provide
     * one; on flagless hosts cancel only surfaces via the fd, which the
     * engine already folds into its own poll set (see puci_agent_get_fds). */
    st->cancel_flag = cancel ? cancel->flag : NULL;

    *out_handle = st;
    return 0;
}

static void shell_async_get_fds(void *userdata, puci_tool_async_handle handle,
                                struct pollfd *fds, size_t cap,
                                size_t *out_count) {
    (void)userdata;
    shell_async_state *st = (shell_async_state *)handle;
    size_t n = 0;
    if (st->out_fd >= 0) {
        if (n < cap) {
            fds[n].fd = st->out_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
        }
        n++;
    }
    if (st->err_fd >= 0) {
        if (n < cap) {
            fds[n].fd = st->err_fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
        }
        n++;
    }
    *out_count = n;
}

static int shell_async_next_timeout_ms(void *userdata,
                                       puci_tool_async_handle handle) {
    (void)userdata;
    shell_async_state *st = (shell_async_state *)handle;
    long long now = now_ms();
    if (st->phase == SHELL_TERM_WAIT) {
        long long rem = st->term_deadline - now;
        return rem <= 0 ? 0 : (rem > 0x7FFFFFFFLL ? 0x7FFFFFFF : (int)rem);
    }
    if (st->phase == SHELL_KILL_WAIT) {
        return 50; /* short poll until the reap succeeds */
    }
    if (st->out_fd < 0 && st->err_fd < 0) {
        /* Both pipes are at EOF (the child has exited or closed them); only
         * waitpid reaping remains. Poll in short slices to reap promptly. */
        return 50;
    }
    if (st->deadline > 0) {
        long long rem = st->deadline - now;
        return rem <= 0 ? 0 : (rem > 0x7FFFFFFFLL ? 0x7FFFFFFF : (int)rem);
    }
    /* Nothing to do until a pipe fd is ready or cancellation is observed (the
     * session's cancel fd, polled alongside these, wakes the host either way). */
    return -1;
}

static int shell_async_step(void *userdata, puci_tool_async_handle handle,
                            char **out_result, int *out_is_error) {
    (void)userdata;
    shell_async_state *st = (shell_async_state *)handle;
    *out_is_error = 0;

    if (st->phase == SHELL_RUNNING && !st->was_cancelled &&
        cancel_requested(st->cancel_flag)) {
        st->was_cancelled = 1;
        (void)killpg(st->pid, SIGTERM);
        st->term_deadline = now_ms() + st->cancel_grace_ms;
        st->phase = SHELL_TERM_WAIT;
    }

    drain_fd(&st->out_fd, &st->out);
    drain_fd(&st->err_fd, &st->err);

    int ws;
    pid_t r = waitpid(st->pid, &ws, WNOHANG);
    if (r == st->pid) {
        if (st->timed_out) {
            st->status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 137;
        } else if (st->was_cancelled) {
            st->status = 130; /* 128 + SIGINT, by convention */
        } else {
            st->status = WIFEXITED(ws) ? WEXITSTATUS(ws)
                                       : (WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1);
        }
        return shell_finish(st, out_result, out_is_error);
    }
    if (r < 0 && errno != EINTR && errno != ECHILD) {
        /* Unexpected reap failure; treat as a tool error rather than spin. */
        st->status = 1;
        return shell_finish(st, out_result, out_is_error);
    }

    long long now = now_ms();
    if (st->phase == SHELL_RUNNING && st->deadline > 0 && now >= st->deadline) {
        /* Hard timeout: kill just the direct child, matching the documented
         * timeout_ms contract (cancellation is what signals the whole group). */
        (void)kill(st->pid, SIGKILL);
        st->timed_out = 1;
        st->phase = SHELL_KILL_WAIT;
    } else if (st->phase == SHELL_TERM_WAIT && now >= st->term_deadline) {
        (void)killpg(st->pid, SIGKILL);
        st->phase = SHELL_KILL_WAIT;
    }

    return 0; /* not done yet */
}

/* Forced teardown (session/context destroyed mid-flight): kill the whole
 * process group and reap. This is the one place a bounded blocking wait is
 * acceptable, per puci_tool_async_abort_cb's contract. */
static void shell_async_abort(void *userdata, puci_tool_async_handle handle) {
    (void)userdata;
    shell_async_state *st = (shell_async_state *)handle;
    if (!st) {
        return;
    }
    (void)killpg(st->pid, SIGKILL);
    int ws;
    for (;;) {
        pid_t r = waitpid(st->pid, &ws, 0);
        if (r == st->pid || (r < 0 && errno == ECHILD)) {
            break;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            break;
        }
    }
    if (st->out_fd >= 0) {
        close(st->out_fd);
    }
    if (st->err_fd >= 0) {
        close(st->err_fd);
    }
    puci_strbuf_destroy(&st->out);
    puci_strbuf_destroy(&st->err);
    free(st);
}

static const puci_tool_async_vtable puci_run_shell_async_vtable = {
    .begin = shell_async_begin,
    .get_fds = shell_async_get_fds,
    .next_timeout_ms = shell_async_next_timeout_ms,
    .step = shell_async_step,
    .abort = shell_async_abort,
};

puci_status puci_register_tool_run_shell(puci_context *ctx) {
    puci_tool_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "run_shell";
    spec.description =
        "Run a command in a subprocess with an explicit argument array (no "
        "shell interpretation). Provide the absolute path of a binary in "
        "'command' and its arguments in 'args'. Optionally set 'cwd' and a "
        "'timeout_ms' (0 = no timeout). Returns the exit code plus captured "
        "stdout and stderr.";
    spec.input_schema =
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\","
        "\"description\":\"Executable path\"},\"args\":{\"type\":\"array\","
        "\"items\":{\"type\":\"string\"}},\"cwd\":{\"type\":\"string\"},"
        "\"timeout_ms\":{\"type\":\"integer\"}},\"required\":[\"command\"]}";
    return puci_tool_register_async(ctx, &spec, &puci_run_shell_async_vtable,
                                    NULL);
}
