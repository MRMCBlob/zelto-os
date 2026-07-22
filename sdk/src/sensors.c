// libzelto sensor + location client (P38). Streams device sensors (accelerometer,
// gyroscope, magnetometer, ...) and GPS from the zsysd sensor source over a single
// persistent unix socket, driven from the app's poll loop (app.c) exactly like the
// net.c HTTP/WebSocket sockets — a stream never blocks the render loop.
//
// One connection carries every stream this app opens. Each z_sensor_open /
// z_loc_watch registers a callback here and sends a *_subscribe line; zsysd pushes
// a sensor_sample / location_update line per subscription at its (clamped) rate,
// which z_sensor_handle_ready parses and hands to the matching callback. A one-shot
// z_loc_get is a transient synchronous read, like z_setting_get_str.
//
// Permission is the broker's: streaming a sensor needs the `sensors` grant and
// location the `location` grant (zsysd checks the cached decision on every tick).
// The app requests it with z_perm_request first; a denied stream simply never
// delivers, mirroring z_net_send's quiet failure. Numeric wire fields travel as
// quoted strings, re-parsed here — the same shape the settings/notify lines use.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "internal.h"
#include "zelto/ui.h"

#define Z_SENSOR_REG_MAX 32
#define Z_SENSOR_BUF 1024

typedef struct SensorReg {
    bool used;
    int handle;
    int kind;              // 0 = sensor, 1 = location
    ZSensorType type;      // kind 0 only
    int rate;              // clamped Hz — kept so a resume can replay the subscribe
    ZSensorCb sensor_cb;   // kind 0
    ZLocationCb loc_cb;    // kind 1
    ZApp *app;
    void *ud;
} SensorReg;

static SensorReg g_regs[Z_SENSOR_REG_MAX];
static int g_next_handle = 1;
static int g_fd = -1;
static char g_buf[Z_SENSOR_BUF];
static size_t g_len;
// While the app is backgrounded its streams are paused at the broker (below): the
// subscriptions stay registered here but no sensor_sample / location_update is
// asked for, so a paused app burns no battery and — the privacy half — cannot keep
// reading the accelerometer or GPS after the user switched away. This is the
// "when-in-use" location semantics the docs promise (foreground-only); `always`
// background location would be a separate, capability-gated opt-out. app.c flips it
// from the xdg lifecycle (z_sensor_set_paused).
static bool g_paused;

// Canonical wire names, indexed by ZSensorType (must track the enum order).
static const char *const g_type_names[Z_SENSOR_COUNT] = {
    "accelerometer", "gyroscope",  "magnetometer", "orientation",
    "gravity",       "linear_acceleration", "rotation_vector", "light",
    "proximity",     "pressure",   "step_counter",
};

static const char *type_name(ZSensorType t) {
    return (t >= 0 && t < Z_SENSOR_COUNT) ? g_type_names[t] : NULL;
}

static ZSensorType type_from_name(const char *name) {
    for (int i = 0; i < Z_SENSOR_COUNT; i++) {
        if (strcmp(g_type_names[i], name) == 0) {
            return (ZSensorType)i;
        }
    }
    return Z_SENSOR_COUNT;   // unknown
}

// Extract a quoted string value for "key" (our lines only carry quoted values).
static bool json_get(const char *buf, const char *key, char *out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Connect a fresh socket to the broker (a transient one for z_loc_get, or the
// persistent stream socket). Same path as app.c's zsysd_connect.
static int sensor_connect(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Ensure the persistent stream socket is up; returns false if the broker is
// unreachable (streams then silently do nothing, like a denied permission).
static bool ensure_stream(void) {
    if (g_fd >= 0) {
        return true;
    }
    g_fd = sensor_connect();
    g_len = 0;
    return g_fd >= 0;
}

static void send_line(const char *msg) {
    if (g_fd < 0) {
        return;
    }
    ssize_t w = write(g_fd, msg, strlen(msg));
    (void)w;
}

static int clamp_rate(int hz) {
    if (hz < Z_SENSOR_RATE_MIN) {
        return Z_SENSOR_RATE_MIN;
    }
    if (hz > Z_SENSOR_RATE_MAX) {
        return Z_SENSOR_RATE_MAX;
    }
    return hz;
}

// Send the subscribe line for one registration at its stored (clamped) rate. Used
// both on the initial open and on a resume that replays every live subscription.
static void send_subscribe(const SensorReg *r) {
    char msg[192];
    if (r->kind == 0) {
        const char *name = type_name(r->type);
        if (!name) {
            return;
        }
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"sensor_subscribe\",\"app_id\":\"%s\",\"type\":\"%s\","
                 "\"rate\":\"%d\"}\n",
                 z_active_app_id() ? z_active_app_id() : "", name, r->rate);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"op\":\"location_subscribe\",\"app_id\":\"%s\",\"rate\":\"%d\"}\n",
                 z_active_app_id() ? z_active_app_id() : "", r->rate);
    }
    send_line(msg);
}

static SensorReg *reg_alloc(void) {
    for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
        if (!g_regs[i].used) {
            return &g_regs[i];
        }
    }
    return NULL;
}

static SensorReg *reg_by_handle(int handle) {
    for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
        if (g_regs[i].used && g_regs[i].handle == handle) {
            return &g_regs[i];
        }
    }
    return NULL;
}

// Any remaining sensor registration of this type (used to decide whether closing
// one handle should also tell the broker to stop that type's stream).
static bool type_still_wanted(ZSensorType t, int except_handle) {
    for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
        if (g_regs[i].used && g_regs[i].kind == 0 && g_regs[i].type == t &&
            g_regs[i].handle != except_handle) {
            return true;
        }
    }
    return false;
}

static bool loc_still_wanted(int except_handle) {
    for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
        if (g_regs[i].used && g_regs[i].kind == 1 &&
            g_regs[i].handle != except_handle) {
            return true;
        }
    }
    return false;
}

// --- public API ------------------------------------------------------------

ZSensorCaps z_sensor_info(ZSensorType type) {
    ZSensorCaps caps = {false, Z_SENSOR_RATE_MIN, Z_SENSOR_RATE_MAX};
    caps.present = (type >= 0 && type < Z_SENSOR_COUNT);
    return caps;
}

int z_sensor_open(ZApp *app, ZSensorType type, int rate_hz, ZSensorCb cb,
                  void *ud) {
    const char *name = type_name(type);
    if (!app || !cb || !name || !ensure_stream()) {
        return 0;
    }
    SensorReg *r = reg_alloc();
    if (!r) {
        return 0;
    }
    r->used = true;
    r->handle = g_next_handle++;
    r->kind = 0;
    r->type = type;
    r->rate = clamp_rate(rate_hz);
    r->sensor_cb = cb;
    r->loc_cb = NULL;
    r->app = app;
    r->ud = ud;

    // Backgrounded: register but do not stream — resume replays this subscribe.
    if (!g_paused) {
        send_subscribe(r);
    }
    return r->handle;
}

void z_sensor_set_rate(int handle, int rate_hz) {
    SensorReg *r = reg_by_handle(handle);
    if (!r || r->kind != 0) {
        return;
    }
    r->rate = clamp_rate(rate_hz);
    // Re-subscribing the same fd+type updates the rate in place (broker keys on
    // fd+type). While paused just record it — the resume subscribe carries it.
    if (!g_paused) {
        send_subscribe(r);
    }
}

void z_sensor_close(int handle) {
    SensorReg *r = reg_by_handle(handle);
    if (!r || r->kind != 0) {
        return;
    }
    ZSensorType type = r->type;
    r->used = false;
    if (!type_still_wanted(type, handle)) {
        const char *name = type_name(type);
        if (name) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "{\"op\":\"sensor_unsubscribe\",\"type\":\"%s\"}\n", name);
            send_line(msg);
        }
    }
}

ZLocation z_loc_get(ZApp *app) {
    ZLocation loc = {0};
    (void)app;
    int fd = sensor_connect();
    if (fd < 0) {
        return loc;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"op\":\"location_get\",\"app_id\":\"%s\"}\n",
             z_active_app_id() ? z_active_app_id() : "");
    if (write(fd, msg, strlen(msg)) > 0) {
        char line[320];
        size_t len = 0;
        while (len + 1 < sizeof(line)) {
            char c;
            ssize_t rd = read(fd, &c, 1);
            if (rd <= 0 || c == '\n') {
                break;
            }
            line[len++] = c;
        }
        line[len] = '\0';
        char val[64];
        if (json_get(line, "ok", val, sizeof(val)) && atoi(val) == 1) {
            loc.ok = true;
            if (json_get(line, "lat", val, sizeof(val))) { loc.lat = strtod(val, NULL); }
            if (json_get(line, "lng", val, sizeof(val))) { loc.lng = strtod(val, NULL); }
            if (json_get(line, "accuracy", val, sizeof(val))) { loc.accuracy = (float)strtod(val, NULL); }
            if (json_get(line, "altitude", val, sizeof(val))) { loc.altitude = (float)strtod(val, NULL); }
            if (json_get(line, "speed", val, sizeof(val))) { loc.speed = (float)strtod(val, NULL); }
            if (json_get(line, "bearing", val, sizeof(val))) { loc.bearing = (float)strtod(val, NULL); }
            if (json_get(line, "t", val, sizeof(val))) { loc.t = (int64_t)strtoll(val, NULL, 10); }
        }
    }
    close(fd);
    return loc;
}

int z_loc_watch(ZApp *app, int rate_hz, ZLocationCb cb, void *ud) {
    if (!app || !cb || !ensure_stream()) {
        return 0;
    }
    SensorReg *r = reg_alloc();
    if (!r) {
        return 0;
    }
    r->used = true;
    r->handle = g_next_handle++;
    r->kind = 1;
    r->rate = clamp_rate(rate_hz);
    r->sensor_cb = NULL;
    r->loc_cb = cb;
    r->app = app;
    r->ud = ud;

    // Backgrounded: register but do not stream — resume replays this subscribe.
    if (!g_paused) {
        send_subscribe(r);
    }
    return r->handle;
}

void z_loc_stop(int handle) {
    SensorReg *r = reg_by_handle(handle);
    if (!r || r->kind != 1) {
        return;
    }
    r->used = false;
    if (!loc_still_wanted(handle)) {
        send_line("{\"op\":\"location_unsubscribe\"}\n");
    }
}

// --- dispatch (one pushed stream line) -------------------------------------

static void dispatch_line(const char *line) {
    char op[24] = {0};
    if (!json_get(line, "op", op, sizeof(op))) {
        return;
    }
    if (strcmp(op, "sensor_sample") == 0) {
        char type[24] = {0}, val[32];
        if (!json_get(line, "type", type, sizeof(type))) {
            return;
        }
        ZSensorType t = type_from_name(type);
        if (t >= Z_SENSOR_COUNT) {
            return;
        }
        ZSensorSample s = {0};
        s.type = t;
        s.n = json_get(line, "n", val, sizeof(val)) ? atoi(val) : 3;
        s.accuracy = json_get(line, "accuracy", val, sizeof(val)) ? atoi(val) : 3;
        s.t = json_get(line, "t", val, sizeof(val)) ? (int64_t)strtoll(val, NULL, 10) : 0;
        if (json_get(line, "v0", val, sizeof(val))) { s.v[0] = (float)strtod(val, NULL); }
        if (json_get(line, "v1", val, sizeof(val))) { s.v[1] = (float)strtod(val, NULL); }
        if (json_get(line, "v2", val, sizeof(val))) { s.v[2] = (float)strtod(val, NULL); }
        for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
            SensorReg *r = &g_regs[i];
            if (r->used && r->kind == 0 && r->type == t && r->sensor_cb) {
                r->sensor_cb(r->app, &s, r->ud);
            }
        }
        return;
    }
    if (strcmp(op, "location_update") == 0) {
        char val[64];
        ZLocation loc = {0};
        loc.ok = true;
        if (json_get(line, "lat", val, sizeof(val))) { loc.lat = strtod(val, NULL); }
        if (json_get(line, "lng", val, sizeof(val))) { loc.lng = strtod(val, NULL); }
        if (json_get(line, "accuracy", val, sizeof(val))) { loc.accuracy = (float)strtod(val, NULL); }
        if (json_get(line, "altitude", val, sizeof(val))) { loc.altitude = (float)strtod(val, NULL); }
        if (json_get(line, "speed", val, sizeof(val))) { loc.speed = (float)strtod(val, NULL); }
        if (json_get(line, "bearing", val, sizeof(val))) { loc.bearing = (float)strtod(val, NULL); }
        if (json_get(line, "t", val, sizeof(val))) { loc.t = (int64_t)strtoll(val, NULL, 10); }
        for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
            SensorReg *r = &g_regs[i];
            if (r->used && r->kind == 1 && r->loc_cb) {
                r->loc_cb(r->app, &loc, r->ud);
            }
        }
    }
}

// --- background pause/resume (app.c lifecycle) -----------------------------

// Foreground state changed. Backgrounding tells the broker to stop every stream
// (so a paused app draws no power and cannot read sensors/GPS behind the user's
// back); foregrounding replays each live subscription at its stored rate. The
// registrations themselves are untouched, so an app's z_sensor_open handles stay
// valid across a pause. Called from app.c on the xdg activated-state transition.
void z_sensor_set_paused(bool paused) {
    if (paused == g_paused) {
        return;
    }
    g_paused = paused;
    if (g_fd < 0) {
        return;   // no stream yet: the flag alone gates future opens
    }
    if (paused) {
        // Stop each distinct sensor type once, plus location if any watch is live.
        bool loc_stopped = false;
        for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
            SensorReg *r = &g_regs[i];
            if (!r->used) {
                continue;
            }
            if (r->kind == 0) {
                // Skip if an earlier reg already unsubscribed this type.
                bool done = false;
                for (int j = 0; j < i; j++) {
                    if (g_regs[j].used && g_regs[j].kind == 0 &&
                        g_regs[j].type == r->type) {
                        done = true;
                        break;
                    }
                }
                if (done) {
                    continue;
                }
                const char *name = type_name(r->type);
                if (name) {
                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "{\"op\":\"sensor_unsubscribe\",\"type\":\"%s\"}\n",
                             name);
                    send_line(msg);
                }
            } else if (!loc_stopped) {
                loc_stopped = true;
                send_line("{\"op\":\"location_unsubscribe\"}\n");
            }
        }
    } else {
        // Resume: replay every live subscription at its stored rate.
        for (int i = 0; i < Z_SENSOR_REG_MAX; i++) {
            if (g_regs[i].used) {
                send_subscribe(&g_regs[i]);
            }
        }
    }
}

// --- app-loop integration (app.c) ------------------------------------------

int z_sensor_poll_fd(void) {
    return g_fd;
}

void z_sensor_handle_ready(void) {
    if (g_fd < 0) {
        return;
    }
    if (g_len >= sizeof(g_buf) - 1) {
        g_len = 0;   // overlong line: drop (our messages are small)
    }
    ssize_t r = read(g_fd, g_buf + g_len, sizeof(g_buf) - 1 - g_len);
    if (r <= 0) {
        close(g_fd);
        g_fd = -1;
        g_len = 0;
        return;
    }
    g_len += (size_t)r;
    g_buf[g_len] = '\0';
    char *start = g_buf;
    char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        dispatch_line(start);
        start = nl + 1;
    }
    size_t rem = g_len - (size_t)(start - g_buf);
    memmove(g_buf, start, rem);
    g_len = rem;
}
