/* error.c - status string helpers. */
#include "puci/puci.h"

const char *puci_status_string(puci_status status) {
    switch (status) {
    case PUCI_OK: return "success";
    case PUCI_ERR_UNKNOWN: return "unknown error";
    default: return "error";
    }
}
const char *puci_status_token(puci_status status) {
    switch (status) {
    case PUCI_OK: return "ok";
    default: return "error";
    }
}
const char *puci_context_error(const puci_context *ctx) { (void)ctx; return NULL; }
