#ifndef PUCI_VERSION_H
#define PUCI_VERSION_H

/*
 * puci/version.h - version and ABI version macros.
 *
 * Compatibility policy:
 *  - PUCI_ABI_VERSION is the source of truth for the shared library soname.
 *  - Any change that alters the layout or semantics of public types, or
 *    removes/depends-on a symbol, is an ABI break and MUST bump
 *    PUCI_ABI_VERSION. Source-compatible additions (new functions, new enum
 *    members) should not bump the ABI version.
 *  - The release version (MAJOR.MINOR.PATCH) may advance independently of the
 *    ABI version.
 */

#define PUCI_VERSION_MAJOR 0
#define PUCI_VERSION_MINOR 2
#define PUCI_VERSION_PATCH 0

#define PUCI_VERSION_STRING "0.2.0"

 /* ABI version, drives the shared library soname (e.g. libpuci.so.2).
 * Version 2 is deliberate for v0.1.0: the public tool callback gained a cancel
 * token and puci_event gained its permanent payload union. v0.2.0 adds source-
 * compatible APIs (MCP client, Anthropic provider, Lua bindings) and therefore
 * keeps ABI version 2 unchanged. */
#define PUCI_ABI_VERSION 2

#define PUCI_VERSION_HEX \
    ((PUCI_VERSION_MAJOR << 16) | (PUCI_VERSION_MINOR << 8) | (PUCI_VERSION_PATCH))

/* Returns the library release version as a static, NUL-terminated string.
 * The returned pointer is owned by the library and must not be freed.
 * Thread-safe. */
const char *puci_version_string(void);

/* Returns the numeric release version as a packed HEX value. Thread-safe. */
unsigned puci_version_hex(void);

/* Returns the ABI version. Thread-safe. */
int puci_abi_version(void);

#endif /* PUCI_VERSION_H */
