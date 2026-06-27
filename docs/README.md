# Zelto OS Documentation

Zelto OS is a mobile operating system that runs your own **native apps** alongside
unmodified **Android apps (APKs)**. It is built on a Linux base (postmarketOS-class
devices), runs Android in a container (Waydroid) for APK compatibility, and adds a
C-written graphics + UI layer with its own design language and a great developer
experience.

- **Native apps** are written in **Zelto Script** (a JS-like language) or in **C**,
  using a declarative, SwiftUI-style UI toolkit (`libzelto`).
- **Android apps** run via the bundled Android container and are themed into the
  Zelto shell as much as the platform allows.

> Status: this documentation describes the target design of Zelto OS. Components are
> delivered in phases (see [Roadmap](#roadmap)). Pages note when a feature is planned
> vs. available.

## Start here

| If you want to… | Read |
|---|---|
| Understand what Zelto OS is | [overview/introduction.md](overview/introduction.md) |
| See the whole system stack | [overview/architecture-overview.md](overview/architecture-overview.md) |
| Know how APKs run | [overview/how-it-runs-apks.md](overview/how-it-runs-apks.md) |
| Build your first app | [getting-started/hello-world.md](getting-started/hello-world.md) |
| Look up an API | [api-reference/](api-reference/) |
| Work on Zelto OS itself | [contributing/repo-layout.md](contributing/repo-layout.md) |

## Documentation map

- **overview/** — what Zelto OS is, the architecture, how APKs run, the design language.
- **getting-started/** — install the SDK, write hello world, use the simulator, run on a device.
- **guides/** — task-focused how-tos (UI, state, layout, animation, networking, storage…).
- **ui/** — the component catalog (every built-in view).
- **api-reference/** — exact API for C (`libzelto`) and Zelto Script.
- **zelto-script/** — the app language: syntax, stdlib, runtime.
- **system-apis/** — device capabilities (camera, sensors, location, media…).
- **platform/** — app lifecycle, permissions, IPC/intents, Android interop.
- **packaging/** — the `.zap` package, manifest, signing, publishing.
- **tooling/** — the `zelto` CLI, simulator, debugging, profiling.
- **contributing/** — how Zelto OS is built and how to port it to a device.

## Roadmap

Zelto OS ships in phases; docs are ordered to match.

1. **Foundation** — Linux base boots on device; Waydroid runs an APK.
2. **Compositor** — `zcomp` brings up display + input.
3. **SDK MVP** — `libzelto` renders a static view tree.
4. **Interactivity** — state, gestures, animation, navigation.
5. **Zelto Script** — JS-like language bound to the SDK.
6. **System UI** — status bar, launcher, switcher, lock screen.
7. **Services + APK integration** — lifecycle, permissions, Android bridge.
8. **Packaging + store** — `.zap`, signing, install, store.

See [overview/architecture-overview.md](overview/architecture-overview.md) for the full
component breakdown.
