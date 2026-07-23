// Zelto demo app — "Sensors" (the P38 sensor/location showcase).
//
// Exercises the libzelto sensor + location client from the app side. It declares
// `permissions=sensors,location`; on launch it requests them through the zsysd
// consent flow, then opens streams for the accelerometer, gyroscope, orientation,
// light and step counter, and watches location. The readouts update live as zsysd
// pushes samples onto the app loop. A denied permission simply leaves the readout
// blank — nothing crashes, matching the graceful contract the andemu bridge relies
// on. The "level" dot is driven straight off the accelerometer's x/y.
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

typedef struct SensorsState {
    ZApp *app;            // captured each build so the stream callbacks can repaint
    bool started;         // one-time init guard
    bool sensors_ok;      // the `sensors` grant landed
    bool location_ok;     // the `location` grant landed

    float accel[3];
    float gyro[3];
    float orient[3];      // azimuth, pitch, roll (degrees)
    float light;
    long steps;
    int samples;          // total sensor samples received (proves the stream lives)

    ZLocation loc;
} SensorsState;

// One stream callback for every sensor: route by type into the matching field.
static void on_sensor(ZApp *app, const ZSensorSample *s, void *ud) {
    SensorsState *st = ud;
    st->samples++;
    switch (s->type) {
        case Z_SENSOR_ACCELEROMETER:
            st->accel[0] = s->v[0]; st->accel[1] = s->v[1]; st->accel[2] = s->v[2];
            break;
        case Z_SENSOR_GYROSCOPE:
            st->gyro[0] = s->v[0]; st->gyro[1] = s->v[1]; st->gyro[2] = s->v[2];
            break;
        case Z_SENSOR_ORIENTATION:
            st->orient[0] = s->v[0]; st->orient[1] = s->v[1]; st->orient[2] = s->v[2];
            break;
        case Z_SENSOR_LIGHT:
            st->light = s->v[0];
            break;
        case Z_SENSOR_STEP_COUNTER:
            st->steps = (long)s->v[0];
            break;
        default:
            break;
    }
    if (app) {
        z_invalidate(app);
    }
}

static void on_location(ZApp *app, const ZLocation *loc, void *ud) {
    SensorsState *st = ud;
    st->loc = *loc;
    if (app) {
        z_invalidate(app);
    }
}

// The `location` grant is decided (chained after `sensors` so only one consent
// request is outstanding at a time — the loop parks a single perm socket).
static void on_location_perm(ZApp *app, ZPermStatus status, void *ud) {
    SensorsState *st = ud;
    (void)app;
    st->location_ok = (status == Z_PERM_GRANTED);
    if (st->location_ok) {
        z_loc_watch(st->app, Z_SENSOR_RATE_UI, on_location, st);
    }
    z_invalidate(st->app);
}

// The `sensors` grant is decided: open every sensor stream, then move on to ask
// for location.
static void on_sensors_perm(ZApp *app, ZPermStatus status, void *ud) {
    SensorsState *st = ud;
    (void)app;
    st->sensors_ok = (status == Z_PERM_GRANTED);
    if (st->sensors_ok) {
        z_sensor_open(st->app, Z_SENSOR_ACCELEROMETER, Z_SENSOR_RATE_UI, on_sensor, st);
        z_sensor_open(st->app, Z_SENSOR_GYROSCOPE, Z_SENSOR_RATE_GAME, on_sensor, st);
        z_sensor_open(st->app, Z_SENSOR_ORIENTATION, Z_SENSOR_RATE_UI, on_sensor, st);
        z_sensor_open(st->app, Z_SENSOR_LIGHT, Z_SENSOR_RATE_NORMAL, on_sensor, st);
        z_sensor_open(st->app, Z_SENSOR_STEP_COUNTER, Z_SENSOR_RATE_NORMAL, on_sensor, st);
    }
    z_perm_request("location", on_location_perm, st);
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static ZView reading(const char *label, const char *value) {
    return HStack(
        Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION, Text("%s", label))),
        Spacer(),
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_BODY, Text("%s", value))),
        .align = Z_ALIGN_CENTER);
}

static ZView sensors_body(ZApp *app, SensorsState *st) {
    st->app = app;
    if (!st->started) {
        st->started = true;
        z_perm_request("sensors", on_sensors_perm, st);
    }

    char accel[64], gyro[64], orient[64], light[32], steps[32], loc[96];
    snprintf(accel, sizeof(accel), "%.2f  %.2f  %.2f", st->accel[0], st->accel[1], st->accel[2]);
    snprintf(gyro, sizeof(gyro), "%.2f  %.2f  %.2f", st->gyro[0], st->gyro[1], st->gyro[2]);
    snprintf(orient, sizeof(orient), "%.0f°  %.0f°  %.0f°", st->orient[0], st->orient[1], st->orient[2]);
    snprintf(light, sizeof(light), "%.0f lx", st->light);
    snprintf(steps, sizeof(steps), "%ld", st->steps);
    if (st->location_ok && st->loc.ok) {
        snprintf(loc, sizeof(loc), "%.4f, %.4f  ±%.0fm", st->loc.lat, st->loc.lng,
                 st->loc.accuracy);
    } else {
        snprintf(loc, sizeof(loc), st->location_ok ? "acquiring…" : "no permission");
    }

    // Level dot: the accelerometer's x/y tilt, mapped to a ±60px offset.
    float dx = clampf(st->accel[0] * 6.0f, -60.0f, 60.0f);
    float dy = clampf(st->accel[1] * 6.0f, -60.0f, 60.0f);

    const char *hint = st->sensors_ok ? "streaming" : "grant sensors to stream";
    char sub[64];
    snprintf(sub, sizeof(sub), "%s   ·   %d samples", hint, st->samples);

    return Background(Z_COLOR_BG,
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_TITLE, Text("Sensors"))),
            Foreground(Z_COLOR_TEXT_MUTED, Font(Z_FONT_CAPTION, Text("%s", sub))),
            Spacer(),
            Background(Z_COLOR_SURFACE_2,
                CornerRadius(18,
                    Frame(520.0f, 300.0f,
                        VStack(
                            reading("Accelerometer", accel),
                            reading("Gyroscope", gyro),
                            reading("Orientation", orient),
                            reading("Light", light),
                            reading("Steps", steps),
                            reading("Location", loc),
                            .spacing = Z_SPACE_S, .padding = Z_SPACE_L, .align = Z_ALIGN_LEADING)))),
            Spacer(),
            // The level: a dot inside a ring, nudged by device tilt.
            Background(Z_COLOR_SURFACE,
                CornerRadius(80,
                    Frame(160.0f, 160.0f,
                        ZStack(
                            OffsetXY(dx, dy,
                                Background(Z_COLOR_PRIMARY,
                                    CornerRadius(14, Frame(28.0f, 28.0f, Spacer())))),
                            .align = Z_ALIGN_CENTER)))),
            Spacer(),
            .padding = Z_SPACE_L, .spacing = Z_SPACE_M, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(SensorsState, sensors_body, "os.zelto.sensors")
