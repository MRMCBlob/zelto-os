// test_sdk_sensor_rate — the public sensor rate contract in <zelto/ui.h>.
//
// libzelto's z_sensor_open/z_sensor_set_rate and the zsysd sensor source both
// clamp a requested refresh rate to [Z_SENSOR_RATE_MIN, Z_SENSOR_RATE_MAX] and the
// andemu bridge (android/hardware.js delayToRate) maps Android's SENSOR_DELAY_*
// onto the named presets. That whole chain is only correct if the public constants
// hold their contract: bounds are 1..60, every preset lies inside the bounds and
// they strictly increase from NORMAL to FASTEST, FASTEST == MAX, and the sensor
// enum has exactly the count the wire-name table in sensors.c is sized for.
//
// This is the pure, compositor-free half. The runtime clamp() that enforces these
// bounds (a static in sdk/src/sensors.c reached only through a live broker socket)
// is exercised by the run-sim integration recipe in memory zelto-os-p39-status
// (SIM_APP=zelto-sensors); here we lock the contract those bounds depend on so a
// regression in the header is caught without a boot.
#include "zelto/ui.h"

#include "framework/ztest.h"

int main(void) {
    // Bounds are the documented [1, 60] Hz window.
    ASSERT_EQ_INT(1, Z_SENSOR_RATE_MIN);
    ASSERT_EQ_INT(60, Z_SENSOR_RATE_MAX);

    // Every preset is inside the clamp window.
    EXPECT_TRUE(Z_SENSOR_RATE_NORMAL >= Z_SENSOR_RATE_MIN &&
                Z_SENSOR_RATE_NORMAL <= Z_SENSOR_RATE_MAX);
    EXPECT_TRUE(Z_SENSOR_RATE_UI >= Z_SENSOR_RATE_MIN &&
                Z_SENSOR_RATE_UI <= Z_SENSOR_RATE_MAX);
    EXPECT_TRUE(Z_SENSOR_RATE_GAME >= Z_SENSOR_RATE_MIN &&
                Z_SENSOR_RATE_GAME <= Z_SENSOR_RATE_MAX);
    EXPECT_TRUE(Z_SENSOR_RATE_FASTEST >= Z_SENSOR_RATE_MIN &&
                Z_SENSOR_RATE_FASTEST <= Z_SENSOR_RATE_MAX);

    // Presets strictly increase (NORMAL < UI < GAME < FASTEST) — the ordering the
    // Android SENSOR_DELAY_* names imply.
    EXPECT_TRUE(Z_SENSOR_RATE_NORMAL < Z_SENSOR_RATE_UI);
    EXPECT_TRUE(Z_SENSOR_RATE_UI < Z_SENSOR_RATE_GAME);
    EXPECT_TRUE(Z_SENSOR_RATE_GAME < Z_SENSOR_RATE_FASTEST);

    // FASTEST is the cap (Android's "device-dependent fastest" -> our max).
    ASSERT_EQ_INT(Z_SENSOR_RATE_MAX, Z_SENSOR_RATE_FASTEST);

    // The enum count must match the 11 canonical wire names sensors.c indexes by
    // ZSensorType (g_type_names[Z_SENSOR_COUNT]); a mismatch reads past the table.
    ASSERT_EQ_INT(11, Z_SENSOR_COUNT);

    return zt_result();
}
