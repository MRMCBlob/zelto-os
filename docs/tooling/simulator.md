# Simulator (internals & advanced use)

The user-facing guide is [../getting-started/simulator.md](../getting-started/simulator.md).
This page covers how the simulator works and advanced options.

## How it works

The simulator runs the real `zcomp` compositor as a **nested Wayland client** of your
desktop session, instead of driving DRM/KMS directly on a phone
([../contributing/compositor-internals.md](../contributing/compositor-internals.md)).

```
your desktop (Wayland/X11)
└── zcomp (nested) ── phone-sized window
    ├── minimal System UI (status bar, home gesture)
    ├── zsysd (permission broker, lifecycle)   [simulated]
    └── your app (libzelto / Zelto Script)
```

Because it's the same compositor and toolkit, layout, animation, gestures, navigation,
and permission prompts behave as on device. Only the kernel/HAL and the Android container
are absent.

## Device profiles

A profile defines resolution, scale factor, safe-area insets, and input type:

```sh
zelto simulator profiles
zelto run --simulator --profile tall-1080
```

Custom profile (project-local `zelto.sim.toml`):

```toml
[[profile]]
name = "my-device"
width = 1080
height = 2400
scale = 3
safeArea = { top = 96, bottom = 48 }
```

## Mocking device state

```sh
zelto simulator set battery 35
zelto simulator set network none      # offline
zelto simulator set location 52.52,13.40
zelto simulator rotate
```

Sensor and permission mocking let you test edge cases deterministically.

## Hot reload

Zelto Script edits reload in place, preserving navigation where possible. Native (C)
changes trigger a rebuild + app restart.

## What's not simulated

- The **Android container** — APK interop requires a device or Waydroid-capable host
  ([../getting-started/run-on-device.md](../getting-started/run-on-device.md)).
- Real **GPU/CPU performance** — profile on device for true numbers
  ([profiling.md](profiling.md)).
- Device-specific **HAL** behavior (camera/sensors quirks).

## Headless / CI mode

```sh
zelto run --simulator --headless --screenshot out.png
```

Renders without a visible window — useful for screenshot tests in CI.
