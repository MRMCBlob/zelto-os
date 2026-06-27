#!/usr/bin/env bash
# Fetch + cross-build a mainline aarch64 Linux kernel for the QEMU 'virt' target,
# with the Zelto config fragment merged on top of the arm64 defconfig.
#
# Zelto does NOT write its own kernel (APK compatibility needs mainline Linux +
# Android binder); this only configures and builds upstream.
#
# Output: $OUT/Image           (uncompressed kernel for `qemu -kernel`)
#         $OUT/.config         (final resolved config)
#
# Env overrides:
#   KERNEL_VERSION   mainline stable to fetch (default below)
#   CROSS_COMPILE    cross prefix (default aarch64-linux-gnu-)
#   JOBS             parallel make jobs (default: nproc)
#   BUILD_DIR        where to download/extract/build (default: meta/build/kernel)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

KERNEL_VERSION="${KERNEL_VERSION:-6.12.31}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/meta/build/kernel}"

FRAGMENT="$HERE/zelto.config"
SERIES="${KERNEL_VERSION%%.*}.x"
TARBALL="linux-${KERNEL_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${SERIES}/${TARBALL}"
SRC="$BUILD_DIR/linux-${KERNEL_VERSION}"
OUT="$HERE/out"

export ARCH=arm64
export CROSS_COMPILE

echo "==> Zelto kernel build"
echo "    version : $KERNEL_VERSION"
echo "    cross   : $CROSS_COMPILE"
echo "    jobs    : $JOBS"
echo "    src     : $SRC"
echo "    out     : $OUT"

mkdir -p "$BUILD_DIR" "$OUT"

# 1. Fetch + extract.
if [ ! -d "$SRC" ]; then
    if [ ! -f "$BUILD_DIR/$TARBALL" ]; then
        echo "==> downloading $URL"
        curl -fSL --retry 3 -o "$BUILD_DIR/$TARBALL" "$URL"
    fi
    echo "==> extracting $TARBALL"
    tar -C "$BUILD_DIR" -xf "$BUILD_DIR/$TARBALL"
fi

# 2. Base config = arm64 defconfig, then merge the Zelto fragment.
echo "==> make defconfig"
make -C "$SRC" O="$SRC/.build" defconfig

echo "==> merging Zelto fragment: $FRAGMENT"
"$SRC/scripts/kconfig/merge_config.sh" -m -O "$SRC/.build" \
    "$SRC/.build/.config" "$FRAGMENT"
make -C "$SRC" O="$SRC/.build" olddefconfig

# 3. Verify our key symbols actually landed (fail loud if a rename/drop happened).
echo "==> verifying required symbols"
required=(
    CONFIG_DRM_VIRTIO_GPU
    CONFIG_VIRTIO_INPUT
    CONFIG_ANDROID_BINDER_IPC
    CONFIG_ANDROID_BINDERFS
)
missing=0
for sym in "${required[@]}"; do
    if grep -q "^${sym}=y" "$SRC/.build/.config"; then
        echo "    ok   $sym=y"
    else
        echo "    MISS $sym (not =y)"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || { echo "ERROR: required kernel symbols missing"; exit 1; }

# 4. Build the kernel image.
echo "==> building Image (-j$JOBS)"
make -C "$SRC" O="$SRC/.build" -j"$JOBS" Image

cp "$SRC/.build/arch/arm64/boot/Image" "$OUT/Image"
cp "$SRC/.build/.config" "$OUT/.config"
echo "==> done: $OUT/Image"
