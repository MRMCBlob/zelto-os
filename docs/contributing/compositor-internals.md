# Compositor Internals (`zcomp`)

`zcomp` is the Wayland compositor — the heart of Zelto's graphics. It owns the display,
composites all surfaces (native apps, System UI, and Android windows), and runs the
animation/vsync loop. Built in C on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots).

## Responsibilities

- **Display:** DRM/KMS output management (modes, vsync, multiple outputs).
- **Surfaces:** map/stack/transform Wayland surfaces; manage the scene graph.
- **Input:** libinput → pointer/touch/keyboard → routed to focused surface + gestures.
- **Compositing:** GPU compositing via EGL/GLES (Vulkan planned), with damage tracking.
- **Animation:** the spring/transition engine that animates surface transforms/opacity.
- **Shell protocols:** `wlr-layer-shell` for System UI; custom protocols for app↔shell.

## Architecture

```
DRM/KMS  ─┐
libinput ─┤   wlroots backends
GPU/EGL  ─┘
            │
        ┌───▼────────────────────────────────┐
        │ zcomp                               │
        │  ├─ output manager (modes, vsync)   │
        │  ├─ scene graph (layers, surfaces)  │
        │  ├─ damage tracker                  │
        │  ├─ animation engine (springs)      │
        │  ├─ input router + gesture engine   │
        │  └─ protocol handlers               │
        └───┬────────────────────────────────┘
            │ Wayland
   ┌────────┼─────────────┬──────────────┐
 native app  System UI    Waydroid (Android surfaces)
```

## Render loop

1. **Vsync tick** from the output.
2. Advance **animations** (springs update surface transforms/opacity).
3. Compute **damage** (changed regions) across the scene graph.
4. **Composite** only damaged regions on the GPU.
5. **Present** the frame, synced to the display.

Damage tracking + partial composite keep power low and frame rate high — only what
changed is redrawn ([../tooling/profiling.md](../tooling/profiling.md)).

## Surfaces & layers

- **App layer:** native and Android app windows; the navigator/animator move them.
- **Overlay/shell layers (`wlr-layer-shell`):** status bar, shade, lock screen, app
  switcher — anchored, always-on-top of apps as configured.
- **Android surfaces** from Waydroid are ordinary surfaces; the **APK bridge** assigns
  them decoration and placement ([services-and-ipc.md](services-and-ipc.md)).

### Safe areas are a cross-process contract

Three surfaces publish an **exclusive zone** and `zcomp` shrinks every app window by the
sum: the status bar at the top, the home indicator at the bottom, and the on-screen
keyboard while it is up. Four more surfaces position themselves against those numbers (the
Control/Notification Center, the dim scrim and the volume HUD float below the bar; the
launcher starts its grid under it and reserves the keyboard's strip while searching).

So the heights are not a shared constant, they are an agreement between five processes,
and they live in ONE place — `system/common/safe_areas.h`, pinned by
`test/test_safe_areas_shared.sh`. They are written in HIG **points** through `Z_PT()`, for
the same reason the type scale is: they came off Apple's spec tables, and spending a point
value as a pixel gets you a strip at 54% of the size it was specified at. Nothing fails
when a copy drifts — an overlay just lands on top of the clock — which is why it is a lint
and not a code-review note.

## Input & gestures

libinput events are routed to the focused surface. System-level gestures (edge-back, home
swipe, app-switch) are recognized in `zcomp`/System UI before app delivery; app-level
gestures are handled by `libzelto` ([sdk-internals.md](sdk-internals.md),
[../guides/gestures.md](../guides/gestures.md)).

## Relationship to the SDK

`libzelto` builds an app's scene graph and submits surface updates; `zcomp` composites
them. The animation engine spans both: `libzelto` declares animations; `zcomp` advances
composited transforms each vsync ([../guides/animation.md](../guides/animation.md)).

## The simulator

In the simulator, `zcomp` runs as a **nested Wayland client** of the host (or the
**headless** backend) instead of driving DRM/KMS — same code, different backend
([../tooling/simulator.md](../tooling/simulator.md)).

To make the nested/headless compositor testable without a device or QEMU's QMP channel,
`zcomp` advertises a few extra globals (`compositor/src/server.c`, `seat.c`):

- **wlr-screencopy** — lets `grim` capture a frame (the screenshot path).
- **wlr-virtual-pointer** + **virtual-keyboard** — let `wlrctl`/`wtype` inject taps and
  keystrokes; a virtual device is wired into the seat's `wlr_cursor`/keyboard exactly like
  a real one, so injected events are indistinguishable from hardware.
- **xdg-output-manager** — reports output geometry so screenshot tools don't guess a 0×0
  region.

These are harmless on a real device (the seat only sees a virtual device if a tool creates
one) and are the desktop analog of `run-qemu.sh`'s QMP screendump + `input-send-event`.

## Window capture (the App Switcher's thumbnails)

`wlr-screencopy` captures an **output**, and a backgrounded window is by definition what
is *not* on the output — so a switcher that captures on demand photographs itself. Instead
`zcomp` snapshots each window at the **active → inactive edge** in
`zcomp_update_activation()` (`toplevel.c`): the last instant the window's contents are
what the user was looking at. Every Zelto app is a single `wl_surface`, so the snapshot is
that surface's committed buffer, box-halved. The **compositor** owns the pixels — the
client showing the card is a different, shorter-lived process, and the app may well have
exited, which is the case the card exists for. Delivery is a sealed memfd over
`zelto-toplevel-capture-v1`, keyed off the `zwlr_foreign_toplevel_handle_v1` the switcher
already holds. See `compositor/src/capture.c`.

**Two guards stop a picture being taken at all** (rather than taken and withheld — there
is then no copy anywhere for a later bug to leak):

- **The screen is held by a modal layer surface.** `server->focused_layer` is set exactly
  while a layer surface holds EXCLUSIVE keyboard interactivity, which is what `zelto-lock`
  does when it locks. Backgrounding an app under a lock screen must not mint a fresh
  picture of its contents; the previous snapshot, taken while the user was actually
  looking at it, stands.
- **The app declared `no_snapshot=1`** in its manifest, resolved through `zsysd` once at
  map and cached on the toplevel ([../packaging/manifest.md](../packaging/manifest.md)).

Both are exercised by `test/test_capture_lock_suppression.sh`, which boots the simulator
three times. Each run carries a **positive control from the same boot** — a window that
*must* be captured — because "no snapshot for X" passes trivially if the boot never
reached the state under test.

## See also

- [sdk-internals.md](sdk-internals.md) · [services-and-ipc.md](services-and-ipc.md)
- [../api-reference/c/gfx.md](../api-reference/c/gfx.md)
