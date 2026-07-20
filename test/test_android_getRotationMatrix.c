// test_android_getRotationMatrix — the andemu SensorManager orientation math.
//
// android/hardware.js ships SensorManager.getRotationMatrix / getOrientation as
// the stock Android algorithms verbatim, so an emulated Android app fuses
// accelerometer + magnetometer into azimuth/pitch/roll exactly as on a device.
// This runs the REAL hardware.js under QuickJS (stubbing only the zelto/sensors
// import it does not touch on this path) and checks it against a hand-computed
// reference:
//
//   A perfectly flat, face-up device (gravity = [0,0,9.81]) in a field whose
//   horizontal component points along +x after the cross products, i.e.
//   geomagnetic = [0,20,-40], yields R = the 3x3 identity, hence orientation
//   [0,0,0]. Free fall (|g|^2 < 1% of g^2) and a magnetic field parallel to
//   gravity both return false, as Android documents.
#include "framework/qjs_harness.h"
#include "framework/ztest.h"

// Minimal zelto/sensors stub: hardware.js reads sensors.Rate.* at import time and
// would call sensors.has/open for streaming, which this math test never does.
static const char *SENSORS_STUB =
    "export const Rate = { min:1, normal:5, ui:16, game:50, fastest:60, max:60 };"
    "export function has(t){ return true; }"
    "export function open(t, cb, opts){ return () => {}; }"
    "export const location = { lastKnown(){ return null; },"
    "  watch(cb, opts){ return () => {}; } };";

static const char *DRIVER =
    "import { SensorManager } from 'android/hardware';\n"
    "{\n"
    "  const R = [0,0,0, 0,0,0, 0,0,0];\n"
    "  globalThis.__ok9 = SensorManager.getRotationMatrix(R, null, [0,0,9.81], [0,20,-40]);\n"
    "  globalThis.__R9 = R;\n"
    "  const o = [0,0,0];\n"
    "  SensorManager.getOrientation(R, o);\n"
    "  globalThis.__o = o;\n"
    "}\n"
    "{\n"
    "  const R = new Array(16).fill(0);\n"
    "  globalThis.__ok16 = SensorManager.getRotationMatrix(R, null, [0,0,9.81], [0,20,-40]);\n"
    "  globalThis.__R16 = R;\n"
    "}\n"
    "globalThis.__freefall = SensorManager.getRotationMatrix([0,0,0,0,0,0,0,0,0], null, [0,0,0.1], [0,20,-40]);\n"
    "globalThis.__parallel = SensorManager.getRotationMatrix([0,0,0,0,0,0,0,0,0], null, [0,0,9.81], [0,0,-40]);\n";

int main(void) {
    zt_qjs_add_module("zelto/sensors", SENSORS_STUB);
    zt_qjs_add_file_module("android/compat", "script/android/compat.js");
    zt_qjs_add_file_module("android/hardware", "script/android/hardware.js");

    JSContext *ctx = zt_qjs_new();
    ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);

    // Flat, face-up: valid fix, R == identity.
    ASSERT_TRUE(zt_qjs_global_bool(ctx, "__ok9"));
    const double id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (uint32_t i = 0; i < 9; i++) {
        EXPECT_NEAR(id[i], zt_qjs_global_arr(ctx, "__R9", i), 1e-6);
    }

    // Orientation of the identity matrix is azimuth/pitch/roll all zero.
    EXPECT_NEAR(0.0, zt_qjs_global_arr(ctx, "__o", 0), 1e-6);
    EXPECT_NEAR(0.0, zt_qjs_global_arr(ctx, "__o", 1), 1e-6);
    EXPECT_NEAR(0.0, zt_qjs_global_arr(ctx, "__o", 2), 1e-6);

    // 16-length matrix: same rotation in the top-left 3x3, homogeneous row/col.
    ASSERT_TRUE(zt_qjs_global_bool(ctx, "__ok16"));
    EXPECT_NEAR(0.0, zt_qjs_global_arr(ctx, "__R16", 8), 1e-6);   // A row -> [0,0,1]
    EXPECT_NEAR(0.0, zt_qjs_global_arr(ctx, "__R16", 9), 1e-6);
    EXPECT_NEAR(1.0, zt_qjs_global_arr(ctx, "__R16", 10), 1e-6);
    EXPECT_NEAR(1.0, zt_qjs_global_arr(ctx, "__R16", 15), 1e-6); // homogeneous 1

    // Degenerate inputs return false (no crash, no garbage matrix).
    EXPECT_FALSE(zt_qjs_global_bool(ctx, "__freefall"));
    EXPECT_FALSE(zt_qjs_global_bool(ctx, "__parallel"));

    return zt_result();
}
