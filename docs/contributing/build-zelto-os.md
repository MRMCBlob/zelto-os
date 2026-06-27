# Building Zelto OS

How to build the OS components from source — for contributors. App developers don't need
this (use the prebuilt SDK, [../getting-started/install-sdk.md](../getting-started/install-sdk.md)).

## Prerequisites

A Linux build host with:

- C17 toolchain (`clang`/`gcc`), `meson`, `ninja`, `pkg-config`.
- wlroots + Wayland dev packages, Mesa (EGL/GLES), libinput, libxkbcommon.
- FreeType + HarfBuzz.
- For device images: the postmarketOS toolchain (`pmbootstrap`) and an aarch64
  cross-toolchain.

```sh
sudo apt install build-essential clang meson ninja-build pkg-config \
  libwayland-dev wayland-protocols libwlroots-dev libegl1-mesa-dev \
  libgles2-mesa-dev libinput-dev libxkbcommon-dev libfreetype-dev libharfbuzz-dev
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

## Run in the nested simulator

Run the freshly built compositor + SDK on your desktop, no device needed:

```sh
ninja -C build && ./build/cli/zelto run --simulator --dev
```

`--dev` uses the local build instead of an installed SDK
([../tooling/simulator.md](../tooling/simulator.md)).

## Cross-compile for a device

```sh
meson setup build-arm64 --cross-file device/cross/aarch64.txt
ninja -C build-arm64
```

The cross file pins the aarch64 toolchain and sysroot. Device images are assembled with
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
ninja -C build test        # unit tests (meson test)
./build/cli/zelto run --simulator --headless --screenshot ci.png   # UI smoke
```

CI runs host unit tests + headless UI smoke tests
([../tooling/simulator.md](../tooling/simulator.md)).

## Common build issues

| Error | Fix |
|---|---|
| `wlroots` version mismatch | Install the pinned wlroots version (see `meta/`) |
| EGL/GLES not found | Install Mesa dev packages |
| cross sysroot missing | Run `pmbootstrap` to populate the sysroot |

## See also

- [porting-to-a-device.md](porting-to-a-device.md)
- [coding-standards.md](coding-standards.md)
