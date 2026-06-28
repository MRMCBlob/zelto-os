# Zelto OS

A mobile OS that runs native **Zelto** apps alongside unmodified **Android apps (APKs)**.
It is built on a mainline Linux base, runs Android in a container (Waydroid) for APK
compatibility, and adds a C graphics/UI layer — the `zcomp` Wayland compositor and the
`libzelto` SDK. See [`docs/`](docs/) for the full design.

## Start device:
```bash
wsl -d Ubuntu
SKIP_BUILD=1 meta/run-qemu.sh
```

> **This repo is at P0/P1 (Foundation + Compositor bring-up).** What exists today is the
> bootable foundation: a mainline aarch64 kernel, a tiny initramfs, and the `zcomp`
> compositor skeleton that brings up a display and paints a frame inside QEMU. The SDK,
> Zelto Script, System UI, services, and Waydroid are later phases (see the
> [roadmap](docs/README.md#roadmap)). Zelto does **not** write its own kernel or libc.

## Repository layout

```
zelto-os/
├── compositor/            # zcomp — Wayland compositor (C, wlroots)
│   ├── include/zcomp/     # public headers
│   ├── src/               # main, server, output manager, render loop
│   └── protocols/         # custom Wayland protocols (later)
├── device/qemu-virt/      # QEMU 'virt' target: kernel fragment + build script
│   ├── zelto.config       # kernel config fragment (virtio-gpu, binder, ...)
│   ├── build-kernel.sh    # fetch + cross-build mainline aarch64 kernel
│   └── out/               # build artifacts: Image, initramfs.cpio.gz
├── meta/                  # build system glue
│   ├── cross/             # Meson cross files (aarch64)
│   ├── initramfs/         # init + initramfs builder
│   └── run-qemu.sh        # build image + launch QEMU
├── docs/                  # design docs (source of truth)
└── meson.build            # top-level Meson project
```

See [`docs/contributing/repo-layout.md`](docs/contributing/repo-layout.md).

## Build host

A Linux build host is required (kernel build, aarch64 cross-compile, QEMU). On Windows,
use **WSL2 (Ubuntu 24.04)** — that is the validated host for this repo. All commands
below run inside that Linux environment.

### Prerequisites

```sh
# Host toolchain + Meson/Ninja
sudo apt install -y build-essential clang meson ninja-build pkg-config

# aarch64 cross toolchain + kernel build deps
sudo apt install -y gcc-aarch64-linux-gnu bc bison flex libssl-dev \
  libelf-dev cpio xz-utils curl qemu-system-arm socat

# arm64 runtime/dev libraries for cross-building zcomp (multiarch)
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install -y \
  libwlroots-dev:arm64 libwayland-dev:arm64 wayland-protocols \
  libxkbcommon-dev:arm64 libpixman-1-dev:arm64 libdrm-dev:arm64 \
  libegl1-mesa-dev:arm64 libgles2-mesa-dev:arm64 libinput-dev:arm64 \
  libgbm-dev:arm64 mesa-vulkan-drivers:arm64 libgl1-mesa-dri:arm64
```

## Build

### Host smoke build (stub compositor)

Builds without the graphics stack — `zcomp` falls back to a stub that prints and exits.
Useful to verify the Meson setup.

```sh
meson setup build
ninja -C build
./build/compositor/zcomp        # prints version, exits 0
```

### Cross-build zcomp for aarch64

```sh
meson setup build-arm64 --cross-file meta/cross/aarch64-linux-gnu.txt
ninja -C build-arm64
```

### Kernel

```sh
device/qemu-virt/build-kernel.sh        # -> device/qemu-virt/out/Image
```

Builds a mainline stable kernel (default `6.12.x`) with `defconfig` + the Zelto fragment
([`device/qemu-virt/zelto.config`](device/qemu-virt/zelto.config)): virtio-gpu DRM/KMS,
virtio input, and the Android-container prerequisites (`ANDROID_BINDER_IPC`,
`ANDROID_BINDERFS`, memfd). Override the version with `KERNEL_VERSION=6.x.y`.

## Run in QEMU

```sh
meta/run-qemu.sh                # builds everything, opens a GTK window (WSLg)
HEADLESS=1 meta/run-qemu.sh     # no window; dumps a frame to out/frame.ppm
```

You should see the kernel boot, `[zelto-init]` mount messages, then `zcomp` clear the
display to a solid color and draw one rectangle — the proof the GPU path works.

The launched QEMU command line is (roughly):

```sh
qemu-system-aarch64 -M virt -cpu cortex-a72 -smp 4 -m 2048 \
  -kernel device/qemu-virt/out/Image \
  -initrd device/qemu-virt/out/initramfs.cpio.gz \
  -append "console=ttyAMA0 rdinit=/init loglevel=7" \
  -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
  -serial mon:stdio -display gtk,gl=off
```

## Coding standards

C17, `-Wall -Wextra -Werror`, 4-space indent, `snake_case` functions, `Z`-prefixed
`PascalCase` types, `z_` public symbols. See
[`docs/contributing/coding-standards.md`](docs/contributing/coding-standards.md).
