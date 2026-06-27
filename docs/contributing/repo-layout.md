# Repository Layout

How the Zelto OS source is organized. This is for people building Zelto OS itself, not app
developers (see [../getting-started/](../getting-started/) for apps).

## Mono-repo with per-component dirs

Zelto OS lives in one repository so cross-component changes (a Wayland protocol shared by
the compositor and the SDK, say) land atomically.

```
zelto-os/
├── docs/                    # this documentation
├── compositor/              # zcomp — Wayland compositor (C, wlroots)
│   ├── src/
│   └── protocols/           # custom Wayland protocols
├── sdk/                     # libzelto — native UI toolkit (C)
│   ├── include/zelto/       # public headers (ui.h, system.h, platform.h, gfx.h)
│   └── src/                 # scene graph, layout, render, text, gestures
├── script/                  # Zelto Script runtime (QuickJS embed + bindings)
│   ├── engine/              # QuickJS integration
│   └── bindings/            # zelto/* module implementations
├── system-ui/               # shell: status bar, launcher, switcher, lock screen
├── services/
│   ├── zsysd/               # lifecycle, permissions broker, package manager
│   └── apk-bridge/          # Waydroid control + integration
├── cli/                     # the `zelto` CLI + simulator launcher
├── packaging/               # .zap tooling, signing
├── device/                  # per-device ports, kernel configs (postmarketOS)
└── meta/                    # build system, CI, shared cmake/meson
```

## Component map → docs

| Dir | What it is | Internals doc |
|---|---|---|
| `compositor/` | zcomp | [compositor-internals.md](compositor-internals.md) |
| `sdk/` | libzelto | [sdk-internals.md](sdk-internals.md) |
| `script/` | Zelto Script runtime | [../zelto-script/runtime.md](../zelto-script/runtime.md) |
| `services/` | zsysd, APK bridge | [services-and-ipc.md](services-and-ipc.md) |
| `system-ui/` | the shell | [../overview/design-language.md](../overview/design-language.md) |
| `cli/` | `zelto` + simulator | [../tooling/cli.md](../tooling/cli.md) |
| `device/` | ports | [porting-to-a-device.md](porting-to-a-device.md) |

## Build system

Meson + Ninja across components; a top-level meson project ties them together. See
[build-zelto-os.md](build-zelto-os.md).

## Language split

- **C17** — compositor, SDK, services, CLI core.
- **Zelto Script** — System UI screens (dogfooding the SDK), sample apps.
- Build/dev scripts in shell + Python.

## Conventions

Coding standards (C style, memory rules, error handling) are in
[coding-standards.md](coding-standards.md). Public SDK headers under `sdk/include/zelto/`
are the stable contract documented in [../api-reference/c/](../api-reference/c/).
