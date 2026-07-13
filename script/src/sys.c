// Native bindings: the system half of the "zelto:native" module — permissions,
// networking, notifications, settings, and app-to-app intents.
//
// The split from ui.c is by subject, not by mechanism: this file is what a
// script talks to when it talks to the REST OF THE SYSTEM (through zsysd),
// while ui.c is what it talks to when it builds a frame. Both are registered
// into the same module (ui.c owns the module definition).
//
// Two rules shape everything here:
//
//   1. Nothing blocks the app loop. Every call whose answer arrives later —
//      a permission the user has to grant, an HTTP response — returns a PROMISE.
//      The C side hands libzelto a callback and returns immediately; the callback
//      fires from the app loop and settles the promise, so `await` in a script
//      suspends that script, not the frame loop.
//   2. Consent is the system's, not the app's. A script never draws a permission
//      dialog: it awaits z_perm_request, which is brokered by zsysd and shows the
//      same consent modal a C app gets. The JS wrappers (zelto/net, etc.) await
//      the grant BEFORE calling the capability, so a denied permission is a
//      rejected promise rather than a silently dead call.
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "zelto/ui.h"

// --- small helpers ---------------------------------------------------------

static bool require_app(JSContext *ctx) {
    if (!zs_current()->app) {
        JS_ThrowTypeError(ctx, "the app is not running yet (call this from a "
                               "component, an effect, or a handler)");
        return false;
    }
    return true;
}

// Read `obj.name` as a string, or NULL when absent. Caller frees with
// JS_FreeCString.
static const char *prop_str(JSContext *ctx, JSValueConst obj, const char *name) {
    if (!JS_IsObject(obj)) { return NULL; }
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

// A promise whose settlers are parked in the long-lived registry: the C callback
// that eventually fires gets nothing but a void*, so the resolve/reject pair is
// stashed by index. Returns the promise (to hand back to JS) and writes the
// registry index the callback should be given.
static JSValue promise_park(JSContext *ctx, uint32_t *idx_out) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { return promise; }

    JSValue pair = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pair, 0, funcs[0]);   // takes the references
    JS_SetPropertyUint32(ctx, pair, 1, funcs[1]);
    *idx_out = zs_cb_add(ctx, pair);
    JS_FreeValue(ctx, pair);
    return promise;
}

// Settle a parked promise and release its slot. `ok` picks resolve vs reject;
// `value` is consumed.
static void promise_settle(uint32_t idx, bool ok, JSValue value) {
    ZScript *zs = zs_current();
    JSContext *ctx = zs->ctx;

    JSValue pair = zs_cb_get(ctx, idx);
    if (JS_IsUndefined(pair)) {          // already settled / never parked
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, pair);
        return;
    }
    JSValue fn = JS_GetPropertyUint32(ctx, pair, ok ? 0 : 1);
    zs_cb_del(ctx, idx);                 // before the call: it may re-enter

    zs_call_handler(fn, 1, (JSValueConst *)&value);

    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, pair);
}

// --- permissions -----------------------------------------------------------

// permStatus(name) -> 0 granted / 1 denied / 2 prompt
static JSValue js_perm_status(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "permStatus(name)"); }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) { return JS_EXCEPTION; }
    ZPermStatus st = z_perm_status(name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, (int32_t)st);
}

static void perm_cb(ZApp *app, ZPermStatus status, void *ud) {
    (void)app;
    promise_settle((uint32_t)(uintptr_t)ud, true,
                   JS_NewBool(zs_current()->ctx, status == Z_PERM_GRANTED));
}

// permRequest(name) -> Promise<boolean>. The consent modal (if any) is zsysd's;
// the script simply awaits the answer.
static JSValue js_perm_request(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "permRequest(name)"); }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) { return JS_EXCEPTION; }

    uint32_t idx = 0;
    JSValue promise = promise_park(ctx, &idx);
    if (JS_IsException(promise)) {
        JS_FreeCString(ctx, name);
        return promise;
    }
    z_perm_request(name, perm_cb, (void *)(uintptr_t)idx);
    JS_FreeCString(ctx, name);
    return promise;
}

// --- networking ------------------------------------------------------------

// Collect the response headers into a plain JS object as they are walked.
static void header_cb(const char *name, const char *value, void *ud) {
    JSContext *ctx = zs_current()->ctx;
    JSValue obj = *(JSValue *)ud;
    // Lower-cased keys, like the web platform's Headers: a script should not have
    // to guess whether the server wrote "Content-Type" or "content-type".
    char lower[128];
    size_t n = strlen(name);
    if (n >= sizeof(lower)) { n = sizeof(lower) - 1; }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[n] = '\0';
    JS_SetPropertyStr(ctx, obj, lower, JS_NewString(ctx, value));
}

static void net_cb(ZNetResponse *res, void *ud) {
    uint32_t idx = (uint32_t)(uintptr_t)ud;
    JSContext *ctx = zs_current()->ctx;

    // status 0 is a TRANSPORT error (no connection, no response) — the request
    // never reached HTTP. That is a rejected promise, like fetch(); an HTTP error
    // status (404, 500) is a perfectly good response and resolves with ok=false.
    if (!res || res->status == 0) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message",
                          JS_NewString(ctx, "network request failed"));
        promise_settle(idx, false, err);
        return;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, res->status));
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, res->ok));

    ZBytes body = z_net_body(res);
    JS_SetPropertyStr(ctx, obj, "body",
                      body.ok && body.data
                          ? JS_NewStringLen(ctx, (const char *)body.data, body.len)
                          : JS_NewString(ctx, ""));

    JSValue headers = JS_NewObject(ctx);
    z_net_headers(res, header_cb, &headers);
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    promise_settle(idx, true, obj);
}

// netSend(method, url, headers, body) -> Promise<{status, ok, body, headers}>
//
// The `network` permission is checked by z_net_send itself, but the JS wrapper
// awaits the grant first (see zelto/net), so by the time we get here the answer
// is cached and the send is immediate.
static JSValue js_net_send(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "netSend(method, url, headers, body)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }

    const char *method = JS_ToCString(ctx, argv[0]);
    if (!method) { return JS_EXCEPTION; }
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!url) {
        JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
    }

    ZNetRequest *r = z_net_request(method, url);
    JS_FreeCString(ctx, method);
    if (!r) {
        JSValue e = JS_ThrowTypeError(ctx, "malformed URL: %s", url);
        JS_FreeCString(ctx, url);
        return e;
    }
    JS_FreeCString(ctx, url);

    // headers: a plain object { "content-type": "application/json", ... }
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSPropertyEnum *props = NULL;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, argv[2],
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; i++) {
                JSValue kv = JS_GetProperty(ctx, argv[2], props[i].atom);
                const char *key = JS_AtomToCString(ctx, props[i].atom);
                const char *val = JS_ToCString(ctx, kv);
                if (key && val) { z_net_set_header(r, key, val); }
                if (key) { JS_FreeCString(ctx, key); }
                if (val) { JS_FreeCString(ctx, val); }
                JS_FreeValue(ctx, kv);
            }
            for (uint32_t i = 0; i < count; i++) {
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }

    if (argc > 3 && JS_IsString(argv[3])) {
        size_t len = 0;
        const char *body = JS_ToCStringLen(ctx, &len, argv[3]);
        if (body) {
            z_net_set_body(r, body, len);
            JS_FreeCString(ctx, body);
        }
    }

    uint32_t idx = 0;
    JSValue promise = promise_park(ctx, &idx);
    if (JS_IsException(promise)) {
        z_net_cancel(r);
        return promise;
    }
    z_net_send(r, net_cb, (void *)(uintptr_t)idx);
    return promise;
}

// --- notifications ---------------------------------------------------------

static JSValue js_notify_channel(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "notifyChannel(id, name, importance)");
    }
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    int32_t imp = Z_IMPORTANCE_DEFAULT;
    if (argc > 2) { JS_ToInt32(ctx, &imp, argv[2]); }
    if (id && name) { z_notify_define_channel(id, name, (ZImportance)imp); }
    if (id) { JS_FreeCString(ctx, id); }
    if (name) { JS_FreeCString(ctx, name); }
    return JS_UNDEFINED;
}

// notifyPost(title, body, opts) -> id (or -1 if denied/unreachable)
//
// z_notify_post is a synchronous round-trip to the broker. That is fine here
// BECAUSE the JS wrapper has already awaited the `notifications` grant: the
// consent modal has been answered, so this call only carries the payload and
// comes straight back. (Posting without awaiting would still work — libzelto
// pumps Wayland during consent — but it would stall the frame loop, which is
// exactly what the promise-first wrapper exists to avoid.)
static JSValue js_notify_post(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "notifyPost(title, body, opts)");
    }
    const char *title = JS_ToCString(ctx, argv[0]);
    if (!title) { return JS_EXCEPTION; }
    const char *body = JS_ToCString(ctx, argv[1]);
    if (!body) {
        JS_FreeCString(ctx, title);
        return JS_EXCEPTION;
    }

    ZNotification *n = z_notify_new(title, body);
    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, body);
    if (!n) { return JS_NewInt64(ctx, -1); }

    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    const char *channel = prop_str(ctx, o, "channel");
    if (channel) {
        z_notify_set_channel(n, channel);
        JS_FreeCString(ctx, channel);
    }
    const char *route = prop_str(ctx, o, "tapRoute");
    if (route) {
        z_notify_set_tap_route(n, route);
        JS_FreeCString(ctx, route);
    }
    const char *aid = prop_str(ctx, o, "actionId");
    const char *atitle = prop_str(ctx, o, "actionTitle");
    if (aid && atitle) { z_notify_add_action(n, aid, atitle); }
    if (aid) { JS_FreeCString(ctx, aid); }
    if (atitle) { JS_FreeCString(ctx, atitle); }

    return JS_NewInt64(ctx, z_notify_post(n));
}

static JSValue js_notify_cancel(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_UNDEFINED; }
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) { return JS_EXCEPTION; }
    z_notify_cancel(id);
    return JS_UNDEFINED;
}

static JSValue js_notify_badge(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    int32_t count = 0;
    if (argc > 0 && JS_ToInt32(ctx, &count, argv[0]) < 0) { return JS_EXCEPTION; }
    z_notify_set_badge(count);
    return JS_UNDEFINED;
}

static void notify_action_cb(ZApp *app, const ZNotifyActionEvent *e, void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);

    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "id", JS_NewInt64(ctx, e->notification_id));
    // An empty action_id means the BODY was tapped (the tap_route, if any, has
    // already been routed through z_open_url) — surface it as null, not "".
    JS_SetPropertyStr(ctx, ev, "action",
                      (e->action_id && e->action_id[0])
                          ? JS_NewString(ctx, e->action_id)
                          : JS_NULL);

    zs_call_handler(fn, 1, (JSValueConst *)&ev);
    JS_FreeValue(ctx, ev);
    JS_FreeValue(ctx, fn);
}

// notifyOnAction(fn): one handler per app; a queued action fires on registration.
static JSValue js_notify_on_action(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "notifyOnAction(fn)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    uint32_t idx = zs_cb_add(ctx, argv[0]);
    z_on_notification_action(zs_current()->app, notify_action_cb,
                             (void *)(uintptr_t)idx);
    return JS_UNDEFINED;
}

// --- settings --------------------------------------------------------------

static JSValue js_setting_get(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "settingGet(key)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    const char *val = z_setting_get_str(key, NULL);
    JS_FreeCString(ctx, key);
    return val ? JS_NewString(ctx, val) : JS_NULL;
}

static JSValue js_setting_get_int(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "settingGetInt(key, fallback)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    int64_t fallback = 0;
    if (argc > 1) { JS_ToInt64(ctx, &fallback, argv[1]); }
    int64_t v = z_setting_get_int(key, fallback);
    JS_FreeCString(ctx, key);
    return JS_NewInt64(ctx, v);
}

static JSValue js_setting_set(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) { return JS_ThrowTypeError(ctx, "settingSet(key, value)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    z_setting_set_str(key, val);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

static void settings_cb(ZApp *app, const char *key, const char *value, void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);
    JSValueConst args[2] = {
        JS_NewString(ctx, key ? key : ""),
        JS_NewString(ctx, value ? value : ""),
    };
    zs_call_handler(fn, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, fn);
}

// settingsObserve(fn | null): fires on EVERY change, including this app's own —
// so a script must apply changes idempotently (it never loops, but it will see
// its own write echoed back).
static JSValue js_settings_observe(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    (void)this_val;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        z_settings_observe(zs_current()->app, NULL, NULL);
        return JS_UNDEFINED;
    }
    uint32_t idx = zs_cb_add(ctx, argv[0]);
    z_settings_observe(zs_current()->app, settings_cb, (void *)(uintptr_t)idx);
    return JS_UNDEFINED;
}

// --- intents ---------------------------------------------------------------

static JSValue js_open_url(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "openUrl(url)"); }
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) { return JS_EXCEPTION; }
    z_open_url(url);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

#define ZS_MAX_SHARE_ITEMS 8

// share([{ mime, text }, ...]) — the system chooser picks the target app.
static JSValue js_share(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "share(items)"); }

    uint32_t len = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, argv[0], "length");
    if (JS_ToUint32(ctx, &len, lenv) < 0) { len = 0; }
    JS_FreeValue(ctx, lenv);
    if (len > ZS_MAX_SHARE_ITEMS) { len = ZS_MAX_SHARE_ITEMS; }

    ZShareItem items[ZS_MAX_SHARE_ITEMS] = {0};
    const char *mimes[ZS_MAX_SHARE_ITEMS] = {0};
    const char *texts[ZS_MAX_SHARE_ITEMS] = {0};
    int n = 0;

    for (uint32_t i = 0; i < len; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, argv[0], i);
        mimes[n] = prop_str(ctx, item, "mime");
        texts[n] = prop_str(ctx, item, "text");
        JS_FreeValue(ctx, item);
        if (!texts[n]) {                       // nothing to hand over
            if (mimes[n]) { JS_FreeCString(ctx, mimes[n]); }
            mimes[n] = NULL;
            continue;
        }
        items[n].mime = mimes[n] ? mimes[n] : "text/plain";
        items[n].text = texts[n];
        n++;
    }

    if (n > 0) { z_share(items, n); }

    for (int i = 0; i < n; i++) {
        if (mimes[i]) { JS_FreeCString(ctx, mimes[i]); }
        if (texts[i]) { JS_FreeCString(ctx, texts[i]); }
    }
    return JS_NewBool(ctx, n > 0);
}

static void url_cb(ZApp *app, const char *url, void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);
    JSValue arg = JS_NewString(ctx, url ? url : "");
    zs_call_handler(fn, 1, (JSValueConst *)&arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, fn);
}

static void share_target_cb(ZApp *app, const ZShareItem *items, int count,
                            void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);

    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < count; i++) {
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "mime",
                          JS_NewString(ctx, items[i].mime ? items[i].mime : ""));
        JS_SetPropertyStr(ctx, item, "text",
                          JS_NewString(ctx, items[i].text ? items[i].text : ""));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, item);
    }

    zs_call_handler(fn, 1, (JSValueConst *)&arr);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, fn);
}

// Both registrations replay an intent that arrived BEFORE the handler was set —
// an app launched to handle a share never misses its payload.
static JSValue js_on_open_url(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "onOpenUrl(fn)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    uint32_t idx = zs_cb_add(ctx, argv[0]);
    z_on_open_url(zs_current()->app, url_cb, (void *)(uintptr_t)idx);
    return JS_UNDEFINED;
}

static JSValue js_on_share_target(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "onShareTarget(fn)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    uint32_t idx = zs_cb_add(ctx, argv[0]);
    z_on_share_target(zs_current()->app, share_target_cb, (void *)(uintptr_t)idx);
    return JS_UNDEFINED;
}

// --- module table ----------------------------------------------------------

const JSCFunctionListEntry zs_sys_funcs[] = {
    // permissions
    JS_CFUNC_DEF("permStatus", 1, js_perm_status),
    JS_CFUNC_DEF("permRequest", 1, js_perm_request),
    // net
    JS_CFUNC_DEF("netSend", 4, js_net_send),
    // notifications
    JS_CFUNC_DEF("notifyChannel", 3, js_notify_channel),
    JS_CFUNC_DEF("notifyPost", 3, js_notify_post),
    JS_CFUNC_DEF("notifyCancel", 1, js_notify_cancel),
    JS_CFUNC_DEF("notifyBadge", 1, js_notify_badge),
    JS_CFUNC_DEF("notifyOnAction", 1, js_notify_on_action),
    // settings
    JS_CFUNC_DEF("settingGet", 1, js_setting_get),
    JS_CFUNC_DEF("settingGetInt", 2, js_setting_get_int),
    JS_CFUNC_DEF("settingSet", 2, js_setting_set),
    JS_CFUNC_DEF("settingsObserve", 1, js_settings_observe),
    // intents
    JS_CFUNC_DEF("openUrl", 1, js_open_url),
    JS_CFUNC_DEF("share", 1, js_share),
    JS_CFUNC_DEF("onOpenUrl", 1, js_on_open_url),
    JS_CFUNC_DEF("onShareTarget", 1, js_on_share_target),
};

const int zs_sys_funcs_count =
    (int)(sizeof(zs_sys_funcs) / sizeof(zs_sys_funcs[0]));
