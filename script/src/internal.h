// Internals shared by the Zelto Script host (host.c) and its native bindings
// (ui.c, sys.c). Not installed — apps see <zelto/script.h> and, from JS, the
// `zelto/*` modules.
#ifndef ZSCRIPT_INTERNAL_H
#define ZSCRIPT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "quickjs.h"
#include "zelto/ui.h"

// One runtime per process (an app is one script, one app loop). The bindings
// need the live ZApp and the current build generation, and a C callback fired
// from the app loop has no user-data channel back to the context, so the host
// keeps a single instance and hands it out with zs_current().
typedef struct ZsTimer ZsTimer;
typedef struct ZsField ZsField;

typedef struct ZScript {
    JSRuntime *rt;
    JSContext *ctx;

    ZApp *app;               // live app handle (set for the whole run)
    JSValue root;            // the script's default export (the root component)
    JSValue render;          // core module's __render(): runs hooks + component
    JSValue screen_render;   // core module's __screen(i): renders one nav screen

    // Tap closures. A ZTapAction carries a void* data, not a JSValue, so a handler
    // is registered in this array during a build and passed by INDEX (cast to
    // void*). The array is cleared at the top of every build, which is safe for a
    // TAP: a tap is dispatched by hit-testing the CURRENT tree, so the node and
    // the array its index refers to always come from the same build.
    JSValue handlers;
    uint32_t n_handlers;

    // Gesture closures (onPan / onLongPress) — deliberately NOT the per-build
    // array above. A gesture's data pointer is latched when the gesture STARTS
    // (the slop-cross for a pan, the press for a long-press) and is used for the
    // rest of the gesture, across any rebuild that happens mid-drag. An index into
    // a per-build array would therefore go stale exactly the way P17's cached
    // pan_target did. So a gesture closure is keyed by its CALL SITE: the Nth
    // onPan of a build always occupies slot N, and every rebuild overwrites that
    // slot with the same site's fresh closure — so a latched index keeps resolving
    // to the right site. This is the JS mirror of the C toolkit caching a plain
    // function pointer at the slop-cross. (A tree that reorders its gesture sites
    // mid-drag misroutes, exactly as the C side does: keep them structurally
    // stable, which a component naturally is.)
    JSValue pan_sites;
    uint32_t n_pan_sites;
    JSValue lp_sites;
    uint32_t n_lp_sites;

    // Long-lived closures: net/permission promise settlers, the notification
    // action handler, the settings observer, the intent handlers, text-field
    // on-change. These outlive the build that registered them (a fetch resolves
    // many builds later), so they live in a registry that is never cleared at
    // build time; a one-shot entry is released by index when it fires, and its
    // slot is reused, so a polling app does not grow the array without bound.
    JSValue cbs;
    uint32_t n_cbs;

    // Text fields. A ZTextField is mutable state the toolkit writes into between
    // builds, so it cannot live in the arena: these are heap-allocated, owned
    // here, and handed to JS as an index.
    ZsField **fields;
    uint32_t n_fields;

    // Build generation: bumped per body() call. A ZView is arena memory thrown
    // away at the next rebuild, so a JS wrapper that outlives its build (an app
    // stashing a view in state) must be rejected rather than dereferenced.
    uint64_t gen;
    bool building;

    ZsTimer *timers;         // setTimeout/setInterval queue, soonest first
    int64_t next_timer_id;
} ZScript;

ZScript *zs_current(void);

// Wall clock in ms (monotonic), the timer queue's time base.
int64_t zs_now_ms(void);

// Print a pending JS exception (message + stack) to stderr.
void zs_dump_error(JSContext *ctx);

// Drain the microtask/promise job queue. Called after every entry into JS.
void zs_run_jobs(ZScript *zs);

// Call a JS function from a C callback on the app loop: reports a throw, drains
// the job queue, and repaints (a callback exists to change something). Frees
// nothing — `fn` and `argv` stay the caller's.
void zs_call_handler(JSValue fn, int argc, JSValueConst *argv);

// The long-lived closure registry (ZScript.cbs). zs_cb_add parks `fn` and returns
// its index; zs_cb_get borrows it (free the result); zs_cb_del releases a one-shot
// entry so its slot can be reused.
uint32_t zs_cb_add(JSContext *ctx, JSValueConst fn);
JSValue zs_cb_get(JSContext *ctx, uint32_t idx);
void zs_cb_del(JSContext *ctx, uint32_t idx);

// --- ui.c -----------------------------------------------------------------

// Install the native binding module ("zelto:native") into the context. The
// user-facing `zelto/*` modules are JS wrappers around it (builtins.c).
void zs_init_native_module(JSContext *ctx);

// Wrap/unwrap a ZView. zs_view_of throws and returns NULL on a stale (previous
// build) or non-view value.
JSValue zs_view_new(JSContext *ctx, ZView v);
ZView zs_view_of(JSContext *ctx, JSValueConst val);

// Fired from the app loop when a text field's buffer changes: finds which
// field(s) moved and calls their JS on-change. (ZTextField.on_change carries no
// user data, so the field is identified by diffing against a cached copy.)
void zs_fields_changed(ZApp *app, void *state);

// --- sys.c ----------------------------------------------------------------

// The system-API half of "zelto:native" (permissions, net, notifications,
// settings, intents), registered into the same module by ui.c.
extern const JSCFunctionListEntry zs_sys_funcs[];
extern const int zs_sys_funcs_count;

// The sensors/location half of "zelto:native" (sensors.c), registered into the
// same module by ui.c.
extern const JSCFunctionListEntry zs_sensor_funcs[];
extern const int zs_sensor_funcs_count;

// The JS source of the built-in `zelto/*` modules (builtins.c). Returns NULL if
// `name` is not a built-in.
const char *zs_builtin_module(const char *name);

#endif  // ZSCRIPT_INTERNAL_H
