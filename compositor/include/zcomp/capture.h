// zcomp per-toplevel window capture: the compositor half of
// zelto-toplevel-capture-v1 (the App Switcher's window thumbnails).
// See docs/contributing/compositor-internals.md.
#ifndef ZCOMP_CAPTURE_H
#define ZCOMP_CAPTURE_H

#include <wayland-server-core.h>

#include "zcomp/server.h"

typedef struct ZcompToplevel ZcompToplevel;

// Advertise the zelto_toplevel_capture_manager_v1 global.
void zcomp_capture_init(ZcompServer *server);

// Snapshot a window, because it just stopped being the foreground window.
// No-op while the screen is held by a modal layer surface (the lock screen) —
// see the comment on the definition. Safe to call on a toplevel with no buffer.
void zcomp_capture_take(ZcompToplevel *toplevel);

// The window is going away: tell every client watching it and drop the stored
// image. Called from the toplevel's unmap/destroy path.
void zcomp_capture_toplevel_gone(ZcompToplevel *toplevel);

// Resolve this window's manifest `no_snapshot=` declaration (through zsysd, which
// owns the manifest table) and cache the answer on the toplevel. Call ONCE, from
// the map handler — that is where zcomp first has the app_id, and it keeps the
// query off the capture path. An unreachable broker means "allowed", matching
// every other zsysd fallback in the system.
void zcomp_capture_resolve_policy(ZcompToplevel *toplevel, const char *app_id);

#endif  // ZCOMP_CAPTURE_H
