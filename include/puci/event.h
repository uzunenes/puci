#ifndef PUCI_EVENT_H
#define PUCI_EVENT_H

/*
 * puci/event.h - event/callback API for streaming.
 *
 * Events are delivered through the callback the consumer registered at session
 * creation (see puci_session_options in session.h). Event payloads are
 * documented in puci_event (types.h); each event carries a ctx pointer valid
 * only for the duration of the callback.
 *
 * This header exists as a dedicated place for event-related helpers and
 * documentation so downstream bindings can map events cleanly.
 */

#include "puci/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Converts an event enum value to a stable human-readable name.
 *  Returns a static string owned by the library. Never NULL. Thread-safe. */
const char *puci_event_name(puci_event event);

#ifdef __cplusplus
}
#endif

#endif /* PUCI_EVENT_H */