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

In the simulator, `zcomp` runs as a **nested Wayland client** of the host instead of
driving DRM/KMS — same code, different backend
([../tooling/simulator.md](../tooling/simulator.md)).

## See also

- [sdk-internals.md](sdk-internals.md) · [services-and-ipc.md](services-and-ipc.md)
- [../api-reference/c/gfx.md](../api-reference/c/gfx.md)
