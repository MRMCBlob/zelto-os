# The Simulator

The simulator runs `zcomp` and your app **nested inside your desktop session**, so you
can develop Zelto apps without a phone. It is the primary inner-loop tool. It runs the
host (x86_64) build natively — no VM and no aarch64 emulation, so it is far faster than
booting the QEMU image ([../tooling/simulator.md](../tooling/simulator.md) for internals).

## Run the OS shell today (`meta/run-sim.sh`)

The `zelto run --simulator` CLI below is the intended front-end; the working entry point
right now is the repo script, which boots the whole System UI (bar, launcher, keyboard,
apps) in a phone-shaped window on your desktop (WSLg / any Wayland or X11 session):

```sh
meta/run-sim.sh                 # build the host binaries + run in a window
SKIP_BUILD=1 meta/run-sim.sh    # run the existing build-host/ artifacts (no rebuild)
SIM_APP=zelto-notepad meta/run-sim.sh   # also auto-launch an app
SIM_SIZE=1080x2340 meta/run-sim.sh      # phone resolution (default 720x1440 portrait)
```

The window is a portrait handset by default (720×1440); `SIM_SIZE=WxH` picks another
device size (or a landscape `1280x720`).

Headless (no window) + a screenshot, for CI or an agent:

```sh
HEADLESS=1 SHOT=out.png meta/run-sim.sh
```

The host build needs `libsqlite3-dev`; headless screenshots/input use `grim` + `wlrctl` +
`wtype` (`apt install grim wlrctl wtype`). Details, including scripting taps and typing:
[../tooling/simulator.md](../tooling/simulator.md).

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
