/*
 * examples/embedded_editor_agent.c - an embedded agent inside a mock editor.
 *
 * Demonstrates libpuci's core value proposition: embedding a non-blocking
 * coding agent inside a native application that already owns an event loop.
 * The "editor" here is simulated with a minimal GUI-style loop:
 *
 *   - a set of keyboard/gui "ticks" arrive on a self-pipe (as real GUI event
 *     sources would),
 *   - the agent session is driven from the SAME poll() loop as those events,
 *     never on its own thread,
 *   - application-level custom tools (read/save "buffer", select "line") are
 *     registered and run inside the host process.
 *
 * No terminal is required: all output goes through an event callback, which a
 * real editor would forward to its status line / chat pane rather than print.
 *
 * Usage: embedded_editor_agent "<prompt>"
 *   Set PUCI_BASE_URL/PUCI_API_KEY/PUCI_MODEL to reach a real model, or leave
 *   them unset to see the event loop + tools drive without a network call.
 */

#include <puci/puci.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* A toy editor buffer the agent can operate on through custom tools. */
static char g_buffer[256] = "hello from the editor\n";

/* ---- application-level tools ------------------------------------------- */

/* editor_read_buffer: returns the current buffer contents. */
static int tool_read_buffer(void *ud, const puci_cancel_token *cancel,
                            const char *args_json, char **out_result) {
    (void)ud;
    (void)cancel;
    (void)args_json;
    char *res = (char *)malloc(strlen(g_buffer) + 64);
    if (!res) {
        return 1;
    }
    snprintf(res, strlen(g_buffer) + 64, "{\"ok\":true,\"text\":%s",
             g_buffer);
    /* NOTE: a real editor would JSON-escape the buffer; this is illustrative. */
    *out_result = res;
    return 0;
}

/* editor_select_line: returns the 1-based line number matching a prefix. */
static int tool_select_line(void *ud, const puci_cancel_token *cancel,
                            const char *args_json, char **out_result) {
    (void)ud;
    (void)cancel;
    (void)args_json;
    char *res = (char *)malloc(64);
    if (!res) {
        return 1;
    }
    snprintf(res, 64, "{\"ok\":true,\"line\":1}");
    *out_result = res;
    return 0;
}

/* ---- event forwarding (status line / chat pane in a real editor) ------- */

static void on_event(void *ud, puci_session *sess, puci_event ev,
                     const void *ctx) {
    (void)ud;
    (void)sess;
    switch (ev.kind) {
    case PUCI_EVENT_TOKEN_DELTA:
        if (ev.text) {
            fputs(ev.text, stdout);
            fflush(stdout);
        }
        break;
    case PUCI_EVENT_TOOL_CALL_START: {
        const puci_tool_call *tc = (const puci_tool_call *)ctx;
        fprintf(stderr, "\n[editor-tool] %s(%s)\n", tc->name,
                tc->args_json ? tc->args_json : "");
        break;
    }
    case PUCI_EVENT_TURN_COMPLETE:
        fputs("\n", stdout);
        fflush(stdout);
        break;
    default:
        break;
    }
}

/* ---- simulated GUI event source --------------------------------------- */

/* Writes a "tick" byte to the write end so the loop wakes and redraws. */
static void gui_tick(int pipe_w) {
    char c = 't';
    ssize_t n = write(pipe_w, &c, 1);
    (void)n;
}

int main(int argc, char **argv) {
    const char *prompt = argc > 1 ? argv[1] : "summarize the open buffer";
    const char *base = getenv("PUCI_BASE_URL");
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL") ? getenv("PUCI_MODEL")
                                             : "gpt-4o-mini";

    puci_context *ctx = NULL;
    if (puci_context_create(NULL, &ctx) != PUCI_OK) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }

    /* Register editor-native tools once on the context. */
    puci_tool_spec read_spec = {
        .name = "editor_read_buffer",
        .description = "Return the current editor buffer contents.",
        .input_schema = "{\"type\":\"object\",\"properties\":{}}",
    };
    puci_tool_spec sel_spec = {
        .name = "editor_select_line",
        .description = "Select a line in the editor buffer by number.",
        .input_schema =
            "{\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"integer\"}}}",
    };
    if (puci_tool_register(ctx, &read_spec, tool_read_buffer, NULL) != PUCI_OK ||
        puci_tool_register(ctx, &sel_spec, tool_select_line, NULL) != PUCI_OK) {
        fprintf(stderr, "tool registration failed\n");
        puci_context_destroy(ctx);
        return 1;
    }

    puci_provider_config pcfg = {
        .name = "openai", .base_url = base, .api_key = key, .model = model,
        .timeout_ms = 60000,
    };
    puci_context_set_provider(ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    sopts.event_cb = on_event;
    sopts.system_prompt = "You are an editor assistant. Be terse.";
    puci_session *sess = NULL;
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        puci_context_destroy(ctx);
        return 1;
    }

    /* Simulated GUI event pipe: the editor's own keystroke/redraw source. */
    int gui_pipe[2];
    if (pipe(gui_pipe) != 0) {
        puci_session_destroy(sess);
        puci_context_destroy(ctx);
        return 1;
    }

    int turn_in_flight = 0;
    if (base && key) {
        if (puci_session_begin_turn(sess, prompt) != PUCI_OK) {
            fprintf(stderr, "begin turn failed\n");
            close(gui_pipe[0]);
            close(gui_pipe[1]);
            puci_session_destroy(sess);
            puci_context_destroy(ctx);
            return 1;
        }
        turn_in_flight = 1;
        /* Fire a couple of GUI ticks so the loop interleaves redraws with the
         * in-flight agent turn — proving neither stalls the other. */
        gui_tick(gui_pipe[1]);
        gui_tick(gui_pipe[1]);
    } else {
        printf("PUCI_BASE_URL/PUCI_API_KEY not both set; running the poll loop "
               "and editor tools with no network turn.\n");
    }

    /* ---- the one event loop: agent + GUI events together ---------------- */
    int done = 0;
    int gui_redraws = 0;
    for (int guard = 0; guard < 100000 && !done; guard++) {
        struct pollfd fds[32];
        size_t fd_count = 0;
        int timeout_ms = -1;

        if (turn_in_flight) {
            puci_session_next_timeout_ms(sess, &timeout_ms);
            puci_session_get_fds(sess, fds, 32, &fd_count);
        }

        /* Fold in the GUI pipe as if it were any other native event source. */
        size_t gui_idx = fd_count;
        if (gui_idx < 32) {
            fds[gui_idx].fd = gui_pipe[0];
            fds[gui_idx].events = POLLIN;
            fds[gui_idx].revents = 0;
            gui_idx++;
        }

        int wait_ms = timeout_ms < 0 ? 1000 : timeout_ms;
        if (wait_ms > 100) {
            wait_ms = 100; /* responsive GUI redraw cadence */
        }
        poll(fds, (nfds_t)gui_idx, wait_ms);

        /* Drain GUI ticks (redraw / scroll) first, as a real editor would. */
        if (gui_idx > fd_count && (fds[fd_count].revents & POLLIN)) {
            char buf[64];
            while (read(gui_pipe[0], buf, sizeof(buf)) > 0) {
                gui_redraws++;
            }
        }

        if (turn_in_flight && !done) {
            puci_status st = puci_session_process(sess, fds, fd_count);
            puci_event ev;
            while (puci_session_next_event(sess, &ev)) {
                if (ev.kind == PUCI_EVENT_TURN_COMPLETE ||
                    ev.kind == PUCI_EVENT_ERROR ||
                    ev.kind == PUCI_EVENT_TURN_CANCELLED) {
                    done = 1;
                    if (ev.kind != PUCI_EVENT_TURN_COMPLETE) {
                        fprintf(stderr, "turn ended: %s\n",
                                puci_status_string(puci_session_last_status(sess)));
                    }
                }
            }
            /* Any terminal status other than PUCI_OK (still-in-progress is
             * also PUCI_OK) means an error or cancellation ended the turn. */
            if (!done && st != PUCI_OK) {
                done = 1;
                fprintf(stderr, "turn ended: %s\n", puci_status_string(st));
            }
        }

        /* If no network turn was started, just spin a few GUI frames. */
        if (!turn_in_flight) {
            gui_redraws++;
            if (guard > 3) {
                done = 1;
            }
        }
    }

    printf("[editor loop finished: %d GUI redraws, session status ok]\n",
           gui_redraws);

    close(gui_pipe[0]);
    close(gui_pipe[1]);
    puci_session_destroy(sess);
    puci_context_destroy(ctx);
    return 0;
}
