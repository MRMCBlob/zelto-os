# How Zelto OS Runs Android Apps

Zelto OS runs unmodified Android apps (APKs) by bundling a full Android system inside a
container. This page explains the model and is explicit about what works and what does
not.

## The model: Waydroid

Zelto uses [Waydroid](https://waydro.id/), which runs a complete Android (LineageOS-based)
system image inside an **LXC container** on the same Linux kernel as the rest of Zelto OS.
Android renders its windows to **Wayland surfaces**, and Zelto's compositor (`zcomp`)
hosts those surfaces just like it hosts native Zelto app surfaces.

```
Native Zelto app  --\
                     >--  zcomp (Wayland compositor)  -->  display
Android app (APK) --/        ^
   |  ART/DEX                 |
   |  Android framework       |  Wayland surface
   +--  Waydroid container ---+
```

Because Android windows are just surfaces to `zcomp`, the **APK bridge** can decorate,
animate, and place them inside the Zelto shell.

## Kernel requirements

The container needs Android kernel features. The Zelto kernel config must enable:

- `CONFIG_ANDROID_BINDER_IPC` and `CONFIG_ANDROID_BINDERFS` — Binder IPC.
- `ashmem` or a `memfd`-based shim — anonymous shared memory.

postmarketOS devices that already support Waydroid satisfy these. See
[../contributing/porting-to-a-device.md](../contributing/porting-to-a-device.md).

## What works well

- Installing and launching standard APKs (sideload, or via an app store APK).
- GPU-accelerated apps and games (through the container's GLES/Vulkan path).
- Audio, networking, storage shared via configured mounts.
- Window theming/decoration applied by the Zelto APK bridge.

## What is limited

- **"Native feel" is best-effort.** APK UIs are Android underneath — Zelto themes the
  window and integrates lifecycle/notifications, but cannot restyle an app's internal
  Material UI into the Zelto design language.
- **Google services** require microG or GApps inside the container, with the usual
  caveats; some apps that hard-depend on Play Integrity may not work.
- **Hardware passthrough** (camera, sensors, GPS) depends on the device port and the
  Waydroid HAL bridges; availability varies by device.
- **Performance** carries container overhead vs. a stock Android device.

## How APKs integrate with the shell

The **APK bridge** (a system service) provides:

- **Permission mapping** — Android runtime permissions are surfaced through the Zelto
  permission broker (`zsysd`) so the user sees one consistent prompt style. See
  [../platform/apk-interop.md](../platform/apk-interop.md).
- **Lifecycle routing** — foreground/background and task-switch events are coordinated
  with the native app switcher.
- **Notifications** — Android notifications are forwarded to the Zelto notification shade.
- **Launch entries** — installed APKs appear in the launcher next to native apps.

## For app developers

You usually do not target the container directly — you build **native** Zelto apps. But
you can interoperate with installed Android apps (launch them, share to them) via the
platform APIs in [../platform/apk-interop.md](../platform/apk-interop.md).

## See also

- [architecture-overview.md](architecture-overview.md)
- [../platform/apk-interop.md](../platform/apk-interop.md)
- [../contributing/porting-to-a-device.md](../contributing/porting-to-a-device.md)
