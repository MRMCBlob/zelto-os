// test_android_hardware_listener — SensorManager.registerListener stream bookkeeping.
//
// Android lets ONE SensorEventListener register against several sensors, and a
// single unregisterListener(listener) then detaches all of them. android/
// hardware.js must therefore keep every per-sensor unsubscribe fn for a listener,
// not just the last — otherwise the earlier sensor keeps streaming after the app
// unregisters, a battery/privacy leak that quietly undoes the P39 when-in-use
// pause. This drives the REAL hardware.js under QuickJS with a zelto/sensors stub
// whose stop() bumps a counter, and asserts BOTH streams stop.
#include "framework/qjs_harness.h"
#include "framework/ztest.h"

// zelto/sensors stub: has() says every sensor exists; open() returns a stop fn
// that records it was called, so the test can count how many streams were torn
// down. Rate.* is read at hardware.js import time.
static const char *SENSORS_STUB =
    "export const Rate = { min:1, normal:5, ui:16, game:50, fastest:60, max:60 };"
    "export function has(t){ return true; }"
    "export function open(type, cb, opts){"
    "  return () => { globalThis.__stops = (globalThis.__stops || 0) + 1; }; }"
    "export const location = { lastKnown(){ return null; },"
    "  watch(cb, opts){ return () => {}; } };";

static const char *DRIVER =
    "import { SensorManager } from 'android/hardware';\n"
    "globalThis.__stops = 0;\n"
    "const sm = new SensorManager();\n"
    "const accel = sm.getDefaultSensor('accelerometer');\n"
    "const gyro  = sm.getDefaultSensor('gyroscope');\n"
    "const listener = { onSensorChanged(){}, onAccuracyChanged(){} };\n"
    "globalThis.__reg1 = sm.registerListener(listener, accel, 5) ? 1 : 0;\n"
    "globalThis.__reg2 = sm.registerListener(listener, gyro, 5) ? 1 : 0;\n"
    "sm.unregisterListener(listener);\n"
    "globalThis.__stops_after = globalThis.__stops;\n"
    // registerListener with a null sensor/listener must be a no-op returning false.
    "globalThis.__reg_bad = sm.registerListener(listener, null, 5) ? 1 : 0;\n";

int main(void) {
    zt_qjs_add_module("zelto/sensors", SENSORS_STUB);
    zt_qjs_add_file_module("android/compat", "script/android/compat.js");
    zt_qjs_add_file_module("android/hardware", "script/android/hardware.js");

    JSContext *ctx = zt_qjs_new();
    ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);

    // Both registrations succeed (the sensors exist per the stub).
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__reg1"));
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__reg2"));
    // The headline: unregistering the shared listener stops BOTH streams (2), not
    // just the most recent (which the pre-fix single-stop map would have done).
    EXPECT_EQ_INT(2, (int)zt_qjs_global_num(ctx, "__stops_after"));
    // A bogus registration is a harmless false.
    EXPECT_EQ_INT(0, (int)zt_qjs_global_num(ctx, "__reg_bad"));

    return zt_result();
}
