#!/usr/bin/env bash
# Build a tiny aarch64 initramfs that boots to zcomp.
#
# Contents:
#   - BusyBox (static, arm64) providing /bin/sh + coreutils.
#   - /init  (meta/initramfs/init): mounts pseudo-fs + binderfs, execs zcomp.
#   - /usr/bin/zcomp: the cross-built compositor (from build-arm64/).
#   - The shared-library closure zcomp needs, IF it is dynamically linked
#     (wlroots/Mesa/EGL/...), including the Mesa swrast DRI driver which is
#     dlopen'd and therefore not in the ELF NEEDED list.
#
# A statically linked zcomp (the stub) needs none of that, so the closure step
# is skipped automatically.
#
# Output: device/qemu-virt/out/initramfs.cpio.gz
#
# Env overrides:
#   ZCOMP        path to the zcomp binary (default: build-arm64/compositor/zcomp)
#   ARM64_LIBDIR arm64 multiarch lib dir   (default: /usr/lib/aarch64-linux-gnu)
#   BUILD_DIR    work dir (default: meta/build/initramfs)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

ZCOMP="${ZCOMP:-$REPO_ROOT/build-arm64/compositor/zcomp}"
ARM64_LIBDIR="${ARM64_LIBDIR:-/usr/lib/aarch64-linux-gnu}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/meta/build/initramfs}"
OUT_DIR="$REPO_ROOT/device/qemu-virt/out"
ROOT="$BUILD_DIR/root"

echo "==> Zelto initramfs build"
echo "    zcomp : $ZCOMP"

[ -x "$ZCOMP" ] || { echo "ERROR: zcomp not found/executable at $ZCOMP"; exit 1; }

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,tmp,run,etc} "$OUT_DIR" "$BUILD_DIR"

# --- BusyBox (static arm64) -------------------------------------------------
BB="$BUILD_DIR/busybox"
if [ ! -x "$BB" ]; then
    echo "==> fetching busybox-static:arm64 via apt"
    pushd "$BUILD_DIR" >/dev/null
    rm -f ./*.deb
    # Requires: dpkg --add-architecture arm64 && apt-get update
    apt-get download busybox-static:arm64
    dpkg-deb -x ./busybox-static*.deb bbroot
    # Debian ships the binary at /bin/busybox or /usr/bin/busybox.
    cp "$(find bbroot -name busybox -type f | head -1)" "$BB"
    popd >/dev/null
fi
cp "$BB" "$ROOT/bin/busybox"
chmod +x "$ROOT/bin/busybox"

# Install busybox applet symlinks.
for app in sh ls mount umount mkdir cat echo ln cp mv rm ps dmesg sleep \
           switch_root mknod chmod insmod modprobe find head tail; do
    ln -sf busybox "$ROOT/bin/$app"
done

# --- init -------------------------------------------------------------------
cp "$HERE/init" "$ROOT/init"
chmod +x "$ROOT/init"

# --- zcomp ------------------------------------------------------------------
cp "$ZCOMP" "$ROOT/usr/bin/zcomp"
chmod +x "$ROOT/usr/bin/zcomp"

# --- shared-library closure (only if dynamically linked) --------------------
is_dynamic() { aarch64-linux-gnu-readelf -d "$1" >/dev/null 2>&1 \
               && aarch64-linux-gnu-readelf -d "$1" | grep -q NEEDED; }

if is_dynamic "$ZCOMP"; then
    echo "==> zcomp is dynamic: bundling arm64 library closure"
    mkdir -p "$ROOT/lib" "$ROOT/usr/lib"

    # The dynamic loader.
    LOADER="$(find "$ARM64_LIBDIR/.." -maxdepth 2 -name 'ld-linux-aarch64.so.1' 2>/dev/null | head -1)"
    [ -n "$LOADER" ] && cp -L "$LOADER" "$ROOT/lib/"

    declare -A seen
    queue=()
    enqueue() { for n in "$@"; do [ -z "${seen[$n]:-}" ] && { seen[$n]=1; queue+=("$n"); }; done; }

    needed_of() { aarch64-linux-gnu-readelf -d "$1" 2>/dev/null \
                  | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }

    find_lib() { find "$ARM64_LIBDIR" /lib/aarch64-linux-gnu -name "$1" 2>/dev/null | head -1; }

    enqueue $(needed_of "$ZCOMP")
    # Mesa swrast/virtio DRI drivers are dlopen'd, not NEEDED -> add explicitly.
    for drv in swrast_dri.so kms_swrast_dri.so virtio_gpu_dri.so libgallium-*.so; do
        for f in $(find "$ARM64_LIBDIR" -name "$drv" 2>/dev/null); do
            cp -L "$f" "$ROOT$ARM64_LIBDIR/" 2>/dev/null || {
                mkdir -p "$ROOT$ARM64_LIBDIR"; cp -L "$f" "$ROOT$ARM64_LIBDIR/"; }
            enqueue $(needed_of "$f")
        done
    done

    i=0
    while [ $i -lt ${#queue[@]} ]; do
        name="${queue[$i]}"; i=$((i+1))
        path="$(find_lib "$name")"
        if [ -n "$path" ]; then
            mkdir -p "$ROOT$(dirname "$path")"
            cp -Ln "$path" "$ROOT$path" 2>/dev/null || true
            enqueue $(needed_of "$path")
        else
            echo "    WARN: missing $name"
        fi
    done
    echo "==> bundled $((${#seen[@]})) libraries"
else
    echo "==> zcomp is static: no library closure needed"
fi

# --- pack -------------------------------------------------------------------
echo "==> packing cpio"
( cd "$ROOT" && find . | cpio -o -H newc --owner root:root 2>/dev/null ) \
    | gzip -9 > "$OUT_DIR/initramfs.cpio.gz"

echo "==> done: $OUT_DIR/initramfs.cpio.gz ($(du -h "$OUT_DIR/initramfs.cpio.gz" | cut -f1))"
