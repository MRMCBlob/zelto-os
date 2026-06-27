# Architecture Overview

Zelto OS is a layered system. Components in **bold** are written and owned by Zelto
(mostly C); the rest are reused upstream projects that Zelto configures and integrates.

## The stack

```
+-----------------------------------------------------------+
|  Apps                                                     |
|   - Zelto native apps (.zap)     - Android apps (.apk)    |
+-----------------------------------------------------------+
|  App runtimes                                             |
|   - **Zelto UI SDK (libzelto, C)**                        |
|   - **Zelto Script VM (QuickJS embed)**                   |
|   - Android: ART (inside Waydroid container)              |
+-----------------------------------------------------------+
|  **System UI / Shell  (wlr-layer-shell clients)**         |
|   status bar, notif shade, launcher, app switcher,        |
|   lock screen, quick settings  (built with libzelto)      |
+-----------------------------------------------------------+
|  **zcomp - Wayland compositor (wlroots, C)**              |
|   surface mgmt, GPU compositing, animation, input,        |
|   damage tracking, vsync, APK surface hosting             |
+-----------------------------------------------------------+
|  System services (daemons; IPC over D-Bus / custom)       |
|   **zsysd** lifecycle/permissions/packages,               |
|   power, network, notifications, settings,                |
|   **APK bridge** (Waydroid control + permission mapping)  |
+-----------------------------------------------------------+
|  Graphics + input: DRM/KMS, Mesa (GLES/Vulkan), libinput  |
+-----------------------------------------------------------+
|  Userspace base: postmarketOS (Alpine + musl), init       |
+-----------------------------------------------------------+
|  Linux kernel (mainline + ANDROID_BINDER_IPC, ashmem/     |
|  memfd) - required for the Android container              |
+-----------------------------------------------------------+
|  Android container: Waydroid (LineageOS image, LXC)       |
|   ART runs DEX; renders to Wayland surfaces zcomp hosts   |
+-----------------------------------------------------------+
```

## Components

| Component | Language | Responsibility |
|---|---|---|
| **zcomp** | C | Wayland compositor on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots). Owns the display (DRM/KMS), GPU compositing (EGL/GLES; Vulkan later), input (libinput), the animation engine, vsync, and damage tracking. Hosts both Zelto app surfaces and Android (Waydroid) surfaces. |
| **libzelto** | C | The native UI SDK. Declarative view tree, layout engine (stacks + flex), retained GPU scene graph, spring animation, gesture recognition, text shaping (HarfBuzz + FreeType), theming. Native apps are Wayland clients that link this. |
| **Zelto Script VM** | C + JS | An embedded [QuickJS](https://bellard.org/quickjs/) engine that exposes `libzelto` and the system APIs to a JS-like language. Performance-critical code drops to C modules. |
| **System UI** | Zelto Script / C | The shell: status bar, notification shade, launcher/home, app switcher, lock screen, quick settings. Implemented as privileged `wlr-layer-shell` clients that dogfood `libzelto`. |
| **zsysd** | C | Core system daemon: app lifecycle coordination, the permission broker, and the package manager (install/verify `.zap`). |
| **APK bridge** | C | Controls the Waydroid container; maps Zelto permissions to/from Android permissions; decorates and themes Android windows; routes lifecycle and notifications between the two worlds. |
| Services | reuse | NetworkManager/iwd (network), logind/UPower (power), a notification daemon, and a settings store. |

## Why this base

- **wlroots** is the realistic way to write a phone compositor in C without
  reimplementing display drivers — it wraps DRM/KMS, libinput, and GPU compositing. It
  is proven on phones (sway, Phosh).
- **postmarketOS** provides real device support and mainline-leaning kernels for many
  phones, which is what a daily-driver goal needs.
- **Waydroid** is the proven way to run unmodified APKs on a non-Android Linux: it
  renders Android surfaces into a Wayland compositor, so Android windows are simply
  surfaces that `zcomp` manages — which is what makes shell integration possible.

## Data + control flow (launching an app)

1. User taps an icon in the **launcher** (System UI).
2. The launcher asks **zsysd** to start the app by package id.
3. zsysd resolves a native `.zap` (spawns a sandboxed `libzelto`/Script process) or an
   APK (asks the **APK bridge** to start the Android activity in Waydroid).
4. The new app creates a Wayland surface; **zcomp** maps, animates, and composites it.
5. Permission requests flow through the **zsysd** broker, which prompts via System UI.

## Process & isolation model

- Each native app runs in its own process, sandboxed with Linux namespaces + seccomp,
  with a private data directory.
- The Android container is isolated as a whole; per-APK isolation is Android's own.
- `zcomp` and the System UI are privileged; apps talk to them only through Wayland
  protocols and zsysd IPC.

## See also

- [how-it-runs-apks.md](how-it-runs-apks.md)
- [../contributing/compositor-internals.md](../contributing/compositor-internals.md)
- [../contributing/sdk-internals.md](../contributing/sdk-internals.md)
- [../platform/permissions.md](../platform/permissions.md) — security & permission model
