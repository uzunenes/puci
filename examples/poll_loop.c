/*
 * examples/poll_loop.c - libpuci driven from a host-owned event loop.
 *
 * Demonstrates the embeddable thesis: the public non-blocking API
 * (begin_turn / get_fds / next_timeout_ms / process / next_event)
 * allows the agent to be driven from a host-owned poll() loop.
 *
 * Limitations (current Phase 3.5):
 * - Non-blocking next_event() returns only the event enum; context payloads
 *   (like token text) are freed after delivery and unavailable to the caller.
 *   The blocking API (send) provides payloads via callbacks.
 * - This example counts and acknowledges events but cannot display token text
 *   without moving to the blocking send() API or implementing a separate
 *   callback delivery channel.
 * - Tool execution blocks (Stage A; async tools = Issue #2, Stage B).
 *
 * This example demonstrates:
 * - Proper poll timeout flooring (curl_multi_timeout==0 is upper bound)
 * - Correct event loop integration: get_fds / next_timeout_ms / process / next_event
 * - Bounded non-blocking work (process never blocks or loops until done)
 *
 * Configuration (all optional):
 *   PUCI_BASE_URL   provider base URL  (default https://api.openai.com/v1)
 *   PUCI_API_KEY    API key            (required for hosted providers)
 *   PUCI_MODEL      model id           (default gpt-4o-mini)
 *
 * Usage: ./poll_loop
 *   Reads prompts from stdin (one per line). Press Ctrl+D to exit.
 */

#include <puci/puci.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Print event summaries. Note: in the non-blocking API, event payloads
 * (like token text) are freed after next_event() returns and are not accessible
 * to the caller. For full text access use the blocking send() API. */
static void handle_event(puci_event ev) {
    switch (ev) {
    case PUCI_EVENT_TOKEN_DELTA:
        fprintf(stderr, ".");
        fflush(stderr);
        break;
    case PUCI_EVENT_TOOL_CALL_START:
        fprintf(stderr, "\n[tool starting]\n");
        break;
    case PUCI_EVENT_TURN_COMPLETE:
        fprintf(stderr, "\n[turn complete]\n");
        break;
    default:
        break;
    }
}

/* Read one line from stdin. Returns allocated string or NULL on EOF. */
static char *read_line(void) {
    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len >= cap - 1) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }
        buf[len++] = (char)c;
    }

    if (c == EOF && len == 0) {
        free(buf);
        return NULL; /* EOF on empty line = exit */
    }

    buf[len] = '\0';
    return buf;
}

int main(void) {
    puci_context *ctx = NULL;
    puci_session *sess = NULL;
    int ret = 0;

    /* Create context. */
    puci_context_options copts = {0};
    if (puci_context_create(&copts, &ctx) != PUCI_OK) {
        fprintf(stderr, "context creation failed\n");
        return 1;
    }

    /* Configure provider from environment. */
    const char *base = getenv("PUCI_BASE_URL");
    const char *key = getenv("PUCI_API_KEY");
    const char *model = getenv("PUCI_MODEL");
    if (!model) {
        model = "gpt-4o-mini";
    }

    if (base && key) {
        puci_provider_config pcfg = {
            .name = "openai",
            .base_url = base,
            .api_key = key,
            .model = model,
            .timeout_ms = 60000,
        };
        puci_context_set_provider(ctx, &pcfg);
    } else {
        fprintf(stderr,
                "Note: PUCI_BASE_URL and PUCI_API_KEY not set. Using mock "
                "provider.\n");
        fprintf(stderr, "Set them to use a real provider.\n\n");
    }

    /* Create session. */
    puci_session_options sopts = {0};
    if (puci_session_create(ctx, &sopts, &sess) != PUCI_OK) {
        fprintf(stderr, "session creation failed\n");
        puci_context_destroy(ctx);
        return 1;
    }

    fprintf(stderr, "Poll-loop agent ready. Type prompts (one per line, "
                    "Ctrl+D to exit).\n\n");

    /* Main event loop: read prompts, drive turns via poll(). */
    while (1) {
        fprintf(stderr, "> ");
        fflush(stderr);

        char *prompt = read_line();
        if (!prompt) {
            break; /* EOF */
        }

        /* Start a new turn. */
        if (puci_session_begin_turn(sess, prompt) != PUCI_OK) {
            fprintf(stderr, "begin_turn failed\n");
            free(prompt);
            break;
        }
        free(prompt);

        /* Drive turn to completion via non-blocking API + poll loop. */
        int turn_done = 0;
        while (!turn_done) {
            /* Query file descriptors and timeout. */
            size_t fd_count = 0;
            if (puci_session_get_fds(sess, NULL, 0, &fd_count) != PUCI_OK) {
                fprintf(stderr, "get_fds(sizing) failed\n");
                ret = 1;
                turn_done = 1;
                break;
            }

            struct pollfd *fds = NULL;
            if (fd_count > 0) {
                fds = malloc(fd_count * sizeof(struct pollfd));
                if (!fds) {
                    fprintf(stderr, "malloc failed\n");
                    ret = 1;
                    turn_done = 1;
                    break;
                }

                if (puci_session_get_fds(sess, fds, fd_count, NULL) !=
                    PUCI_OK) {
                    fprintf(stderr, "get_fds failed\n");
                    free(fds);
                    ret = 1;
                    turn_done = 1;
                    break;
                }
            }

            /* Get timeout from session. Floor to 1000ms to avoid busy-spin
             * (curl_multi_timeout==0 is upper bound, not a force to poll
             * immediately). */
            int timeout_ms = 0;
            if (puci_session_next_timeout_ms(sess, &timeout_ms) != PUCI_OK) {
                fprintf(stderr, "next_timeout_ms failed\n");
                free(fds);
                ret = 1;
                turn_done = 1;
                break;
            }
            if (timeout_ms == 0 && fd_count > 0) {
                timeout_ms = 1000; /* Floor timeout when fds are ready */
            }

            /* Poll for activity. */
            int poll_ret = poll(fds, (nfds_t)fd_count, timeout_ms);
            if (poll_ret < 0) {
                if (errno == EINTR) {
                    free(fds);
                    continue; /* Retry on signal */
                }
                fprintf(stderr, "poll failed: %s\n", strerror(errno));
                free(fds);
                ret = 1;
                turn_done = 1;
                break;
            }

            /* Process ready fds. */
            if (puci_session_process(sess, fds, fd_count) != PUCI_OK) {
                fprintf(stderr, "process failed\n");
                free(fds);
                ret = 1;
                turn_done = 1;
                break;
            }

            free(fds);

            /* Drain queued events. In the non-blocking API, event payloads
             * are not accessible; they are freed after next_event() returns. */
            puci_event ev;
            while (puci_session_next_event(sess, &ev)) {
                handle_event(ev);

                if (ev == PUCI_EVENT_TURN_COMPLETE) {
                    turn_done = 1;
                }
            }
        }

        if (ret != 0) {
            break;
        }
    }

    puci_session_destroy(sess);
    puci_context_destroy(ctx);

    return ret;
}
