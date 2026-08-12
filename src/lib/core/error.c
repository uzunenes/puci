/*
 * core/error.c - status string lookups for the public error API.
 */

#include "puci/puci.h"

static const char *k_tokens[] = {
    [PUCI_OK] = "ok",
    [PUCI_ERR_UNKNOWN] = "unknown",
    [PUCI_ERR_NOMEM] = "nomem",
    [PUCI_ERR_INVALID_ARG] = "invalid_arg",
    [PUCI_ERR_NOT_FOUND] = "not_found",
    [PUCI_ERR_IO] = "io",
    [PUCI_ERR_NETWORK] = "network",
    [PUCI_ERR_PROTOCOL] = "protocol",
    [PUCI_ERR_UNAUTHORIZED] = "unauthorized",
    [PUCI_ERR_TIMEOUT] = "timeout",
    [PUCI_ERR_TOOL_FAILED] = "tool_failed",
    [PUCI_ERR_OUT_OF_STATE] = "out_of_state",
    [PUCI_ERR_CANCELLED] = "cancelled",
};

const char *puci_status_string(puci_status status) {
    switch (status) {
    case PUCI_OK:
        return "success";
    case PUCI_ERR_UNKNOWN:
        return "unknown error";
    case PUCI_ERR_NOMEM:
        return "out of memory";
    case PUCI_ERR_INVALID_ARG:
        return "invalid argument";
    case PUCI_ERR_NOT_FOUND:
        return "not found";
    case PUCI_ERR_IO:
        return "I/O error";
    case PUCI_ERR_NETWORK:
        return "network error";
    case PUCI_ERR_PROTOCOL:
        return "protocol error";
    case PUCI_ERR_UNAUTHORIZED:
        return "authentication failed";
    case PUCI_ERR_TIMEOUT:
        return "operation timed out";
    case PUCI_ERR_TOOL_FAILED:
        return "tool failed";
    case PUCI_ERR_OUT_OF_STATE:
        return "invalid operation state";
    case PUCI_ERR_CANCELLED:
        return "cancelled";
    }
    return "unknown error";
}

const char *puci_status_token(puci_status status) {
    if (status >= 0 &&
        status < (int)(sizeof(k_tokens) / sizeof(k_tokens[0]))) {
        return k_tokens[status];
    }
    return "unknown";
}