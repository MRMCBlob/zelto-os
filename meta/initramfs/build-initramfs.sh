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
# We resolve the closure with the *real* arm64 dynamic loader run under QEMU
# user emulation (`ld-linux-aarch64.so.1 --list`), which is far more reliable
# than walking ELF NEEDED tags by hand: it follows the exact search/resolution
# the guest will perform. dlopen'd Mesa DRI drivers are not in any NEEDED list,
# so we add the whole dri/ directory and resolve each driver's closure too.
LD_AARCH64="$ARM64_LIBDIR/ld-linux-aarch64.so.1"
[ -e "$LD_AARCH64" ] || LD_AARCH64="/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

is_dynamic() { aarch64-linux-gnu-readelf -d "$1" 2>/dev/null | grep -q NEEDED; }

# Print absolute paths of every shared object an ELF pulls in (transitively).
resolve_closure() {
    LD_LIBRARY_PATH="$ARM64_LIBDIR:/lib/aarch64-linux-gnu" \
        qemu-aarch64-static "$LD_AARCH64" --list "$1" 2>/dev/null \
        | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i }' \
        | sort -u
}

# Copy a host file into the initramfs at the same absolute path.
copy_into_root() {
    local src="$1" dst="$ROOT$1"
    [ -e "$src" ] || return 0
    mkdir -p "$(dirname "$dst")"
    cp -Lf "$src" "$dst"
}

if is_dynamic "$ZCOMP"; then
    echo "==> zcomp is dynamic: bundling arm64 library closure"
    mkdir -p "$ROOT/lib" "$ROOT$ARM64_LIBDIR"

    if ! command -v qemu-aarch64-static >/dev/null 2>&1; then
        echo "ERROR: qemu-aarch64-static needed to resolve the arm64 closure"
        echo "       install it:  sudo apt-get install qemu-user-static"
        exit 1
    fi

    # The loader itself (resolve_closure lists it but copy explicitly to be safe).
    copy_into_root "$LD_AARCH64"
    # Some binaries reference /lib/ld-linux-aarch64.so.1 directly.
    mkdir -p "$ROOT/lib"
    cp -Lf "$LD_AARCH64" "$ROOT/lib/ld-linux-aarch64.so.1"

    count=0
    # Copy a file plus its full transitive .so closure.
    bundle_with_closure() {
        [ -e "$1" ] || return 0
        copy_into_root "$1"; count=$((count + 1))
        while IFS= read -r lib; do
            copy_into_root "$lib"; count=$((count + 1))
        done < <(resolve_closure "$1")
    }

    # 1. zcomp's own closure (pulls libwlroots, libwayland, libEGL/glvnd, ...).
    bundle_with_closure "$ZCOMP"

    # 2. Mesa userspace bits that are dlopen'd, so they are invisible to the ELF
    #    NEEDED walk and must be added explicitly:
    #    a) the glvnd EGL vendor driver + its ICD descriptor,
    #    b) the GLES/GBM vendor libs,
    #    c) the GBM DRI backend,
    #    d) the software + virtio DRI drivers (pull libgallium/libLLVM).
    for lib in \
        "$ARM64_LIBDIR/libEGL_mesa.so.0" \
        "$ARM64_LIBDIR/libGLESv2.so.2" \
        "$ARM64_LIBDIR/libGLESv1_CM.so.1" \
        "$ARM64_LIBDIR/libgbm.so.1" \
        "$ARM64_LIBDIR/libglapi.so.0"; do
        bundle_with_closure "$lib"
    done

    # glvnd EGL vendor ICD descriptors (point glvnd at libEGL_mesa.so.0).
    if [ -d /usr/share/glvnd/egl_vendor.d ]; then
        mkdir -p "$ROOT/usr/share/glvnd/egl_vendor.d"
        cp -Lf /usr/share/glvnd/egl_vendor.d/*.json \
            "$ROOT/usr/share/glvnd/egl_vendor.d/" 2>/dev/null || true
    fi

    # libinput device-quirks data (silences a warning; harmless if absent).
    if [ -d /usr/share/libinput ]; then
        mkdir -p "$ROOT/usr/share/libinput"
        cp -Lrf /usr/share/libinput/. "$ROOT/usr/share/libinput/" 2>/dev/null || true
    fi

    # GBM backends (dlopen'd by libgbm).
    for be in "$ARM64_LIBDIR"/gbm/*.so; do
        bundle_with_closure "$be"
    done

    # DRI drivers we actually need in QEMU: software (kms_swrast/swrast),
    # virtio-gpu, and zink. (Skipping the 50+ device-specific drivers.)
    for drv in kms_swrast swrast virtio_gpu zink; do
        bundle_with_closure "$ARM64_LIBDIR/dri/${drv}_dri.so"
    done

    echo "==> bundled ~$count library entries"
else
    echo "==> zcomp is static: no library closure needed"
fi

# --- pack -------------------------------------------------------------------
echo "==> packing cpio"
( cd "$ROOT" && find . | cpio -o -H newc --owner root:root 2>/dev/null ) \
    | gzip -9 > "$OUT_DIR/initramfs.cpio.gz"

echo "==> done: $OUT_DIR/initramfs.cpio.gz ($(du -h "$OUT_DIR/initramfs.cpio.gz" | cut -f1))"
