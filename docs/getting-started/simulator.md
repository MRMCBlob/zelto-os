# The Simulator

The simulator runs `zcomp` and your app **nested inside your desktop session**, so you
can develop Zelto apps without a phone. It is the primary inner-loop tool.

## Run an app

From an app directory:

```sh
zelto run --simulator
```

This builds the app, launches a nested compositor in a phone-sized window, and starts
your app inside it. Source edits **hot-reload** (Zelto Script) or trigger a rebuild +
restart (C).

## Choosing a device profile

The simulator emulates a screen size, density, and safe-area insets:

```sh
zelto run --simulator --profile pinephone     # default
zelto run --simulator --profile tall-1080
zelto run --simulator --profile small
```

List profiles with `zelto simulator profiles`. Profiles set resolution, scale factor,
notch/rounded-corner insets, and input type. Your layout should respect safe areas — see
[../guides/layout.md](../guides/layout.md).

## What the simulator includes

- A minimal **System UI** (status bar + home gesture) so lifecycle and navigation behave
  like a device.
- The **permission broker** — permission prompts appear as they would on device.
- **Notifications**, **clipboard**, and basic **sensor mocking**.

## Mocking device state

```sh
zelto simulator set battery 35
zelto simulator set network none
zelto simulator set location 52.52,13.40
zelto simulator rotate
```

Use these to test edge cases (offline, low battery, rotation) deterministically.

## Limitations

- The simulator runs your app on the host CPU/GPU — performance numbers are indicative,
  not final. Profile on-device for real timings ([../tooling/profiling.md](../tooling/profiling.md)).
- The Android container is **not** emulated; to test APK interop you need a device or a
  Waydroid-capable host. See [run-on-device.md](run-on-device.md).

## How it works

The simulator is the same `zcomp` compositor running as a nested Wayland client of your
desktop, rather than driving DRM/KMS directly. Details:
[../tooling/simulator.md](../tooling/simulator.md).
