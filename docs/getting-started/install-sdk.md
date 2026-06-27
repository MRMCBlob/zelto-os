# Install the SDK

The Zelto SDK gives you the `zelto` CLI, the UI toolkit (`libzelto`), the Zelto Script
runtime, and the desktop **simulator** so you can build and run apps without a phone.

> Supported hosts: Linux (x86_64/aarch64) is first-class. macOS and Windows (WSL2) are
> supported for development against the simulator.

## 1. Prerequisites

- A C17 toolchain (`clang` or `gcc`), `meson`, `ninja`, and `pkg-config`.
- For the simulator on Linux: a Wayland or X11 desktop session and Mesa drivers.
- `git`.

On a Debian/Ubuntu host:

```sh
sudo apt install build-essential clang meson ninja-build pkg-config \
                 libwayland-dev libegl1-mesa-dev libgles2-mesa-dev \
                 libinput-dev libxkbcommon-dev
```

## 2. Install the CLI

Install the prebuilt CLI (recommended):

```sh
curl -fsSL https://get.zelto.dev/install.sh | sh
```

This installs `zelto` to `~/.zelto/bin`. Add it to your `PATH`:

```sh
export PATH="$HOME/.zelto/bin:$PATH"
```

Or build from source — see [../contributing/build-zelto-os.md](../contributing/build-zelto-os.md).

## 3. Verify the toolchain

```sh
zelto doctor
```

`zelto doctor` checks the compiler, the simulator dependencies, and (if a device is
connected) the device link. Fix anything it reports before continuing.

## 4. What you got

| Piece | Purpose |
|---|---|
| `zelto` CLI | scaffold, build, run, sign, publish — see [../tooling/cli.md](../tooling/cli.md) |
| `libzelto` | the C UI toolkit headers + library |
| Zelto Script runtime | the JS-like app runtime ([../zelto-script/runtime.md](../zelto-script/runtime.md)) |
| Simulator | run apps on your desktop ([simulator.md](simulator.md)) |

## Next

Build your first app: [hello-world.md](hello-world.md).
