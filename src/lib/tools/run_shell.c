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

/* Terminates the child's process group after cancellation: SIGTERM, wait up to
 * grace_ms for it to exit, then SIGKILL, reaping so no zombie remains. All
 * reaps handle EINTR. */
static void terminate_group(pid_t pid, long grace_ms) {
    (void)killpg(pid, SIGTERM);
    long long deadline = now_ms() + grace_ms;
    int ws;
    for (;;) {
        if (now_ms() >= deadline) {
            break;
        }
        pid_t r = waitpid(pid, &ws, WNOHANG);
        if (r == pid) {
            return; /* reaped during the grace period */
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                return; /* already gone */
            }
            break;
        }
        /* Still running; give it a few ms and recheck the deadline. */
        struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
        (void)nanosleep(&ts, NULL);
    }
    (void)killpg(pid, SIGKILL);
    for (;;) {
        pid_t r = waitpid(pid, &ws, 0);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            break;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0) {
            break;
        }
    }
}

static int invoke_shell(void *userdata, const puci_cancel_token *cancel,
                        const char *args_json, char **out_result) {
    (void)userdata;
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
    /* Cancel signalling: the token may be NULL if the engine did not provide
     * one; on ISIGless/raw hosts cancel only surfaces via the fd. */
    const volatile sig_atomic_t *cancel_flag =
        cancel ? cancel->flag : NULL;
    int cancel_fd = (cancel && cancel->fd >= 0) ? cancel->fd : -1;

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

    /* Give the child its own process group from the parent side too, closing
     * the window before the child's own setpgid() runs. */
    (void)setpgid(pid, pid);

    set_nonblocking(out_fds[0]);
    set_nonblocking(err_fds[0]);

    puci_strbuf out;
    puci_strbuf err;
    puci_strbuf_init(&out, NULL);
    puci_strbuf_init(&err, NULL);

    struct pollfd fds[3];
    int status = -1;
    int timed_out = 0;
    int was_cancelled = 0;
    int out_eof = 0;
    int err_eof = 0;
    long long deadline = timeout_ms > 0 ? now_ms() + (long long)timeout_ms : -1;

    while (1) {
        /* Cancellation may be requested from another thread / signal path at
         * any moment, including mid-run. */
        if (cancel_requested(cancel_flag)) {
            was_cancelled = 1;
            break;
        }
        /* Reap the child non-blocking (EINTR-safe). */
        int ws;
        pid_t r = waitpid(pid, &ws, WNOHANG);
        if (r == pid) {
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : (WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1);
            break;
        }
        if (r < 0 && errno != EINTR) {
            break;
        }

        int nfds = 0;
        if (!out_eof) {
            fds[nfds++] = (struct pollfd){out_fds[0], POLLIN, 0};
        }
        if (!err_eof) {
            fds[nfds++] = (struct pollfd){err_fds[0], POLLIN, 0};
        }
        fds[nfds++] = (struct pollfd){cancel_fd, POLLIN, 0};
        if (nfds == 0) {
            /* Everything EOF: wait for child. */
            waitpid(pid, &ws, 0);
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : (WIFSIGNALED(ws) ? 128 + WTERMSIG(ws) : 1);
            break;
        }

        int to = -1;
        if (timeout_ms > 0) {
            long long rem = deadline - now_ms();
            if (rem <= 0) {
                to = 0;
            } else {
                to = rem > 0x7FFFFFFFLL ? 0x7FFFFFFF : (int)rem;
            }
        }
        int pr = poll(fds, (nfds_t)nfds, to);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pr == 0) {
            /* Timeout. */
            kill(pid, SIGKILL);
            waitpid(pid, &ws, 0);
            status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 137;
            timed_out = 1;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (fd == cancel_fd && cancel_fd >= 0) {
                    was_cancelled = 1;
                    break;
                }
                int is_out = (fd == out_fds[0]);
                puci_strbuf *target = is_out ? &out : &err;
                char buf[8192];
                for (;;) {
                    ssize_t nr = read(fd, buf, sizeof(buf));
                    if (nr > 0) {
                        bounded_append(target, buf, (size_t)nr);
                    } else if (nr == 0) {
                        close(fd);
                        if (is_out) {
                            out_eof = 1;
                        } else {
                            err_eof = 1;
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                }
            }
        }
        if (was_cancelled) {
            break;
        }
    }

    /* On cancellation, terminate the child's process group and reap it so no
     * zombie (or surviving grandchild) is left behind. */
    if (was_cancelled) {
        terminate_group(pid, cancel_grace_ms);
        status = 130; /* 128 + SIGINT, by convention */
    }

    /* Drain any leftover output after child exit. */
    {
        char buf[8192];
        ssize_t nr;
        if (!out_eof) {
            while ((nr = read(out_fds[0], buf, sizeof(buf))) > 0) {
                bounded_append(&out, buf, (size_t)nr);
            }
            close(out_fds[0]);
        }
        if (!err_eof) {
            while ((nr = read(err_fds[0], buf, sizeof(buf))) > 0) {
                bounded_append(&err, buf, (size_t)nr);
            }
            close(err_fds[0]);
        }
    }

    puci_strbuf res;
    puci_strbuf_init(&res, NULL);
    puci_strbuf_append_fmt(&res,
                           "{\"ok\":%s,\"exit_code\":%d,\"timed_out\":%s,"
                           "\"cancelled\":%s,\"stdout\":",
                           was_cancelled ? "false" : "true",
                           status, timed_out ? "true" : "false",
                           was_cancelled ? "true" : "false");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&out));
    puci_strbuf_appendz(&res, ",\"stderr\":");
    puci_strbuf_append_json_string(&res, puci_strbuf_cstr(&err));
    puci_strbuf_appendz(&res, "}");

    *out_result = strdup(puci_strbuf_cstr(&res));

    puci_strbuf_destroy(&res);
    puci_strbuf_destroy(&out);
    puci_strbuf_destroy(&err);
    puci_arena_destroy(&arena);
    return 0;
}

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
    return puci_tool_register(ctx, &spec, invoke_shell, NULL);
}