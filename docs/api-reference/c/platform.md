# C API: Platform (`<zelto/platform.h>`)

Lifecycle, permissions, background execution, IPC/intents, and device APIs for native
code. Conventions: [../conventions.md](../conventions.md).

## Lifecycle

Register callbacks for app state transitions
([../../platform/app-lifecycle.md](../../platform/app-lifecycle.md)):

```c
void z_on_lifecycle(ZApp *app, ZLifecycleCb cb, void *ud);

typedef void (*ZLifecycleCb)(ZApp *app, ZLifecycleEvent ev, void *ud);
// ev: Z_LC_LAUNCH, Z_LC_RESUME, Z_LC_PAUSE, Z_LC_STOP, Z_LC_TERMINATE
```

Save state on `Z_LC_PAUSE`; assume you may be terminated afterward.

## Permissions

```c
ZPermStatus z_perm_status(const char *name);     // "camera", "location", ...
void z_perm_request(const char *name, ZPermCallback cb, void *ud);

// ZPermStatus: Z_PERM_GRANTED, Z_PERM_DENIED, Z_PERM_PROMPT
```

Names match the manifest ([../../platform/permissions.md](../../platform/permissions.md)).

## Background execution

```c
ZFinishTask *z_finish_task_begin(const char *name);
void         z_finish_task_end(ZFinishTask *t);

void z_scheduler_register(const char *id, ZJobOpts *opts, ZJobCb cb);
// ZJobOpts{ .min_interval_sec, .requires_network, .requires_charging }
```

See [../../guides/background-tasks.md](../../guides/background-tasks.md).

## IPC & intents

Inter-app messaging, deep links, and share targets
([../../platform/ipc-and-intents.md](../../platform/ipc-and-intents.md)):

```c
void z_on_open_url(ZApp *app, ZUrlCb cb, void *ud);   // incoming deep link/intent
bool z_open_url(const char *url);                     // hand off to the system/another app
void z_share(ZShareItem *items, int count);           // present the share sheet
void z_on_share_target(ZApp *app, ZShareCb cb, void *ud); // receive shared content
```

## Device APIs

Each requires its permission and is async where it touches hardware.

```c
// Sensors
void z_sensor_subscribe(ZSensorType type, ZSensorCb cb, void *ud);  // accel, gyro, ...
void z_sensor_unsubscribe(ZSensorType type);

// Location  (permission: location)
void z_location_get(ZLocationCb cb, void *ud);
void z_location_watch(ZLocationCb cb, void *ud);

// Camera  (permission: camera)
void z_camera_capture(ZCameraOpts opts, ZCaptureCb cb, void *ud);

// Clipboard
void z_clipboard_set_text(const char *text);
char *z_clipboard_get_text(void);

// Haptics
void z_haptic(ZHapticStyle style);   // Z_HAPTIC_LIGHT/MEDIUM/HEAVY/SUCCESS/WARNING/ERROR

// Biometrics  (see system-apis/biometrics)
void z_biometric_authenticate(const char *reason, ZAuthCb cb, void *ud);
```

Higher-level guidance and Script equivalents are in
[../../system-apis/](../../system-apis/) and the `script/` reference.

## Device info

```c
ZDeviceInfo z_device_info(void);   // model, os_version, screen { w, h, scale }, safe_area
float       z_battery_level(void); // 0..1
ZNetType    z_network_type(void);  // wifi / cellular / none
```

## See also

- [ui.md](ui.md) · [system.md](system.md) · [gfx.md](gfx.md)
- [../../platform/](../../platform/) guides.
