/*
 * version.c - version and ABI version accessors.
 *
 * Part of libpuci. Public API declared in include/puci/version.h.
 */

#include "puci/puci.h"

static const char k_version_string[] = PUCI_VERSION_STRING;

const char *puci_version_string(void) {
    return k_version_string;
}

unsigned puci_version_hex(void) {
    return PUCI_VERSION_HEX;
}

int puci_abi_version(void) {
    return PUCI_ABI_VERSION;
}

const char *puci_event_name(puci_event event) {
    switch (event) {
    case PUCI_EVENT_TOKEN_DELTA:
        return "token_delta";
    case PUCI_EVENT_TOOL_CALL_START:
        return "tool_call_start";
    case PUCI_EVENT_TOOL_CALL_END:
        return "tool_call_end";
    case PUCI_EVENT_TURN_COMPLETE:
        return "turn_complete";
    case PUCI_EVENT_ERROR:
        return "error";
    case PUCI_EVENT_REASONING_DELTA:
        return "reasoning_delta";
    case PUCI_EVENT_TURN_CANCELLED:
        return "turn_cancelled";
    }
    return "unknown";
}