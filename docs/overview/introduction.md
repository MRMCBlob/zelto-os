# Introduction

## What Zelto OS is

Zelto OS is a mobile operating system with two goals that usually conflict:

1. **Run unmodified Android apps.** You can install and use real APKs.
2. **Be a fast, modern OS with its own identity** — written largely in C, with a
   declarative UI toolkit, its own design language, and first-class developer tooling.

It reconciles them with a layered design: a Linux base provides the kernel and drivers,
an Android container (Waydroid) provides APK compatibility, and a **C graphics + UI
layer** built by Zelto provides the compositor, the native app SDK, the System UI, and
the bridge that themes Android apps into the Zelto shell.

## Design principles

- **Reuse the hard parts, own the experience.** Zelto does not write a kernel or Android
  runtime — those are solved and enormous. Zelto owns what defines the product: how it
  looks, how apps are written, how it feels to use and to build for.
- **Performance by construction.** The compositor and UI toolkit are C, GPU-composited,
  with damage tracking and vsync. Animations target 60/120 fps.
- **Two languages, one model.** App developers use **Zelto Script** (JS-like) for speed
  of development and drop to **C** for performance-critical modules — both drive the same
  declarative UI tree.
- **Honest about Android.** APKs run as Android underneath. Zelto themes and integrates
  them as far as the platform allows, and the docs are explicit about the limits.

## Who it is for

- **App developers** who want a modern, declarative SDK with a fast simulator and a
  clean CLI — without learning a huge framework.
- **Device tinkerers** who want a daily-driver phone OS that still runs the Android apps
  they depend on.
- **Contributors** interested in compositors, UI runtimes, and Linux-on-phone.

## How it differs

| | Stock Android | Zelto OS |
|---|---|---|
| Kernel | Linux (Android) | Linux (mainline / postmarketOS) |
| APK support | Native | Via Android container (Waydroid) |
| Native UI | Android framework (Java/Kotlin) | `libzelto` (C) + Zelto Script |
| UI model | Imperative + Compose | Declarative (SwiftUI-style) |
| Look | Material You | Own design language |

## Next

- [architecture-overview.md](architecture-overview.md) — the full stack.
- [how-it-runs-apks.md](how-it-runs-apks.md) — the Android container model.
- [../getting-started/hello-world.md](../getting-started/hello-world.md) — build something.
