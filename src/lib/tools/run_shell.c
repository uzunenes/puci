/*
 * tools/run_shell.c - built-in run_shell tool.
 *
 * Executes a command in a child process with an explicit argv array using
 * fork + execvp. `system()` is deliberately never used (shell injection
 * vector). stdout and stderr are captured through pipes, bounded, and returned
 * with the exit code. A timeout kills the child.
 */

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

static int invoke_shell(void *userdata, const char *args_json,
                        char **out_result) {
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
        /* Child. */
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

    set_nonblocking(out_fds[0]);
    set_nonblocking(err_fds[0]);

    puci_strbuf out;
    puci_strbuf err;
    puci_strbuf_init(&out, NULL);
    puci_strbuf_init(&err, NULL);

    struct pollfd fds[2] = {
        {out_fds[0], POLLIN, 0},
        {err_fds[0], POLLIN, 0},
    };
    int status = -1;
    int timed_out = 0;
    int out_eof = 0;
    int err_eof = 0;
    long long deadline = timeout_ms > 0 ? now_ms() + (long long)timeout_ms : -1;

    while (1) {
        /* Reap the child non-blocking. */
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
            int which = !out_eof ? 0 : 1;
            int fd = fds[i].fd;
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[8192];
                for (;;) {
                    ssize_t nr = read(fd, buf, sizeof(buf));
                    if (nr > 0) {
                        puci_strbuf *target = (which == 0) ? &out : &err;
                        bounded_append(target, buf, (size_t)nr);
                    } else if (nr == 0) {
                        close(fd);
                        if (which == 0) {
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
                           "{\"ok\":true,\"exit_code\":%d,\"timed_out\":%s,"
                           "\"stdout\":",
                           status, timed_out ? "true" : "false");
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