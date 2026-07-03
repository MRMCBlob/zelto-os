# Building Zelto OS

How to build the OS components from source — for contributors. App developers don't need
this (use the prebuilt SDK, [../getting-started/install-sdk.md](../getting-started/install-sdk.md)).

## Prerequisites

A Linux build host with:

- C17 toolchain (`clang`/`gcc`), `meson`, `ninja`, `pkg-config`.
- wlroots + Wayland dev packages, Mesa (EGL/GLES), libinput, libxkbcommon.
- FreeType + HarfBuzz, SQLite (`libsqlite3-dev` — the SDK/storage), libsodium (packaging).
- For the nested simulator's headless screenshots/input: `grim`, `wlrctl`, `wtype`.
- For device images: the postmarketOS toolchain (`pmbootstrap`) and an aarch64
  cross-toolchain.

```sh
sudo apt install build-essential clang meson ninja-build pkg-config \
  libwayland-dev wayland-protocols libwlroots-dev libegl1-mesa-dev \
  libgles2-mesa-dev libinput-dev libxkbcommon-dev libfreetype-dev libharfbuzz-dev \
  libsqlite3-dev libsodium-dev grim wlrctl wtype
```

## Configure & build (host)

From the repo root:

```sh
meson setup build
ninja -C build
```

This builds all components ([repo-layout.md](repo-layout.md)). Build a single component:

```sh
ninja -C build compositor
ninja -C build sdk
```

> Note: the concrete build dirs this repo uses are `build-host` (native) and `build-arm64`
> (cross); the `meta/run-sim.sh` and `meta/run-qemu.sh` scripts create/build them for you.

## Run in the nested simulator

Run the freshly built compositor + full System UI on your desktop, no device needed — the
fast inner loop. This builds `build-host/` (native x86_64) and launches `zcomp` nested:

```sh
meta/run-sim.sh                 # build + run in a window (WSLg / Wayland / X11)
SKIP_BUILD=1 meta/run-sim.sh    # run existing build-host/ artifacts, no rebuild
```

The intended CLI form (`zelto run --simulator --dev`) is a future front-end for the same
thing. Details, headless mode, and scripted input:
[../tooling/simulator.md](../tooling/simulator.md).

## Cross-compile for a device

```sh
meson setup build-arm64 --cross-file meta/cross/aarch64-linux-gnu.txt
ninja -C build-arm64
```

The cross file pins the aarch64 toolchain and sysroot. `meta/run-qemu.sh` builds this and
boots the full OS in QEMU (aarch64, emulated). Device images are assembled with
postmarketOS — see [porting-to-a-device.md](porting-to-a-device.md).

## Build a device image

```sh
pmbootstrap init           # select the device + Zelto UI
pmbootstrap install        # build the rootfs with zcomp/sdk/services
pmbootstrap flasher flash_rootfs
```

The Zelto packaging integrates `zcomp`, `libzelto`, `zsysd`, the System UI, and Waydroid
into the postmarketOS image.

## Tests

```sh
ninja -C build-host test                              # unit tests (meson test)
HEADLESS=1 SHOT=ci.png meta/run-sim.sh                # nested UI smoke (native, fast)
```

CI runs host unit tests + a headless UI smoke shot via `meta/run-sim.sh` (grim captures
the frame; `wlrctl`/`wtype` script taps + typing). See
[../tooling/simulator.md](../tooling/simulator.md).

## Common build issues

| Error | Fix |
|---|---|
| `wlroots` version mismatch | Install the pinned wlroots version (see `meta/`) |
| EGL/GLES not found | Install Mesa dev packages |
| cross sysroot missing | Run `pmbootstrap` to populate the sysroot |

## See also

- [porting-to-a-device.md](porting-to-a-device.md)
- [coding-standards.md](coding-standards.md)
