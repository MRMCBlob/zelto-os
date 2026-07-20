// Native bindings: the sensors + location half of "zelto:native" (P38).
//
// Same shape as sys.c — thin C that unmarshals JS args, calls the libzelto
// z_sensor_* / z_loc_* client, and marshals results back. Streams (a sensor or a
// GPS watch) park a long-lived JS callback in the registry and fire it per sample
// from the app loop; a one-shot location read is synchronous. Rate presets and
// the value shape mirror what the JS wrapper (zelto/sensors) and the andemu
// bridge (android/hardware) expect.
#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "zelto/ui.h"

// Wire names indexed by ZSensorType (tracks the enum in <zelto/ui.h>).
static const char *const zs_sensor_names[Z_SENSOR_COUNT] = {
    "accelerometer", "gyroscope",  "magnetometer", "orientation",
    "gravity",       "linear_acceleration", "rotation_vector", "light",
    "proximity",     "pressure",   "step_counter",
};

static bool require_app(JSContext *ctx) {
    if (!zs_current()->app) {
        JS_ThrowTypeError(ctx, "the app is not running yet (call this from a "
                               "component, an effect, or a handler)");
        return false;
    }
    return true;
}

static int sensor_type_of(const char *name) {
    for (int i = 0; i < Z_SENSOR_COUNT; i++) {
        if (strcmp(zs_sensor_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

// handle -> parked callback index, so sensorClose/locationStop can release the
// registry slot when a stream ends (a polling app reuses one slot).
#define ZS_SENSOR_MAX 64
static struct {
    int handle;
    uint32_t cbidx;
} zs_sensor_cbs[ZS_SENSOR_MAX];

static void sensor_cb_track(int handle, uint32_t cbidx) {
    for (int i = 0; i < ZS_SENSOR_MAX; i++) {
        if (zs_sensor_cbs[i].handle == 0) {
            zs_sensor_cbs[i].handle = handle;
            zs_sensor_cbs[i].cbidx = cbidx;
            return;
        }
    }
}

// Release the callback slot for a handle (returns true if one was tracked).
static bool sensor_cb_release(int handle) {
    for (int i = 0; i < ZS_SENSOR_MAX; i++) {
        if (zs_sensor_cbs[i].handle == handle) {
            zs_cb_del(zs_current()->ctx, zs_sensor_cbs[i].cbidx);
            zs_sensor_cbs[i].handle = 0;
            return true;
        }
    }
    return false;
}

// --- sensors ---------------------------------------------------------------

static void sensor_sample_cb(ZApp *app, const ZSensorSample *s, void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);

    JSValue ev = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ev, "type",
                      JS_NewString(ctx, (s->type >= 0 && s->type < Z_SENSOR_COUNT)
                                            ? zs_sensor_names[s->type]
                                            : "unknown"));
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < s->n && i < 3; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewFloat64(ctx, s->v[i]));
    }
    JS_SetPropertyStr(ctx, ev, "values", arr);
    JS_SetPropertyStr(ctx, ev, "accuracy", JS_NewInt32(ctx, s->accuracy));
    JS_SetPropertyStr(ctx, ev, "t", JS_NewInt64(ctx, s->t));

    zs_call_handler(fn, 1, (JSValueConst *)&ev);
    JS_FreeValue(ctx, ev);
    JS_FreeValue(ctx, fn);
}

// sensorOpen(typeName, rateHz, fn) -> handle (0 on failure / unknown sensor)
static JSValue js_sensor_open(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 3 || !JS_IsFunction(ctx, argv[2])) {
        return JS_ThrowTypeError(ctx, "sensorOpen(type, rateHz, fn)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) { return JS_EXCEPTION; }
    int type = sensor_type_of(name);
    JS_FreeCString(ctx, name);
    if (type < 0) { return JS_NewInt32(ctx, 0); }   // unknown sensor: not present
    int32_t rate = Z_SENSOR_RATE_NORMAL;
    if (argc > 1) { JS_ToInt32(ctx, &rate, argv[1]); }

    uint32_t cbidx = zs_cb_add(ctx, argv[2]);
    int handle = z_sensor_open(zs_current()->app, (ZSensorType)type, rate,
                               sensor_sample_cb, (void *)(uintptr_t)cbidx);
    if (handle == 0) {
        zs_cb_del(ctx, cbidx);
        return JS_NewInt32(ctx, 0);
    }
    sensor_cb_track(handle, cbidx);
    return JS_NewInt32(ctx, handle);
}

static JSValue js_sensor_set_rate(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) { return JS_UNDEFINED; }
    int32_t handle = 0, rate = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    JS_ToInt32(ctx, &rate, argv[1]);
    z_sensor_set_rate(handle, rate);
    return JS_UNDEFINED;
}

static JSValue js_sensor_close(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_UNDEFINED; }
    int32_t handle = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    z_sensor_close(handle);
    sensor_cb_release(handle);
    return JS_UNDEFINED;
}

// sensorPresent(typeName) -> boolean
static JSValue js_sensor_present(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_FALSE; }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) { return JS_EXCEPTION; }
    int type = sensor_type_of(name);
    JS_FreeCString(ctx, name);
    if (type < 0) { return JS_FALSE; }
    ZSensorCaps caps = z_sensor_info((ZSensorType)type);
    return JS_NewBool(ctx, caps.present);
}

// --- location --------------------------------------------------------------

static JSValue location_to_js(JSContext *ctx, const ZLocation *loc) {
    if (!loc->ok) { return JS_NULL; }
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "lat", JS_NewFloat64(ctx, loc->lat));
    JS_SetPropertyStr(ctx, o, "lng", JS_NewFloat64(ctx, loc->lng));
    JS_SetPropertyStr(ctx, o, "accuracy", JS_NewFloat64(ctx, loc->accuracy));
    JS_SetPropertyStr(ctx, o, "altitude", JS_NewFloat64(ctx, loc->altitude));
    JS_SetPropertyStr(ctx, o, "speed", JS_NewFloat64(ctx, loc->speed));
    JS_SetPropertyStr(ctx, o, "bearing", JS_NewFloat64(ctx, loc->bearing));
    JS_SetPropertyStr(ctx, o, "t", JS_NewInt64(ctx, loc->t));
    return o;
}

// locationGet() -> { lat, lng, ... } | null   (synchronous fast read)
static JSValue js_location_get(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    ZLocation loc = z_loc_get(zs_current()->app);
    return location_to_js(ctx, &loc);
}

static void location_watch_cb(ZApp *app, const ZLocation *loc, void *ud) {
    (void)app;
    JSContext *ctx = zs_current()->ctx;
    JSValue fn = zs_cb_get(ctx, (uint32_t)(uintptr_t)ud);
    JSValue ev = location_to_js(ctx, loc);
    zs_call_handler(fn, 1, (JSValueConst *)&ev);
    JS_FreeValue(ctx, ev);
    JS_FreeValue(ctx, fn);
}

// locationWatch(rateHz, fn) -> handle (0 on failure)
static JSValue js_location_watch(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "locationWatch(rateHz, fn)");
    }
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    int32_t rate = Z_SENSOR_RATE_MIN;
    JS_ToInt32(ctx, &rate, argv[0]);

    uint32_t cbidx = zs_cb_add(ctx, argv[1]);
    int handle = z_loc_watch(zs_current()->app, rate, location_watch_cb,
                             (void *)(uintptr_t)cbidx);
    if (handle == 0) {
        zs_cb_del(ctx, cbidx);
        return JS_NewInt32(ctx, 0);
    }
    sensor_cb_track(handle, cbidx);
    return JS_NewInt32(ctx, handle);
}

static JSValue js_location_stop(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_UNDEFINED; }
    int32_t handle = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    z_loc_stop(handle);
    sensor_cb_release(handle);
    return JS_UNDEFINED;
}

// --- module table ----------------------------------------------------------

const JSCFunctionListEntry zs_sensor_funcs[] = {
    JS_CFUNC_DEF("sensorOpen", 3, js_sensor_open),
    JS_CFUNC_DEF("sensorSetRate", 2, js_sensor_set_rate),
    JS_CFUNC_DEF("sensorClose", 1, js_sensor_close),
    JS_CFUNC_DEF("sensorPresent", 1, js_sensor_present),
    JS_CFUNC_DEF("locationGet", 0, js_location_get),
    JS_CFUNC_DEF("locationWatch", 2, js_location_watch),
    JS_CFUNC_DEF("locationStop", 1, js_location_stop),
};

const int zs_sensor_funcs_count =
    (int)(sizeof(zs_sensor_funcs) / sizeof(zs_sensor_funcs[0]));
