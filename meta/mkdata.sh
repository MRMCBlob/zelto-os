#!/usr/bin/env bash
# Create + format the persistent data disk for Zelto OS (P11).
#
# A raw virtio-blk image the guest mounts at /var/zelto (ext4). init formats
# nothing at runtime; the filesystem is laid down here, once, on the host. The
# image persists across QEMU runs, so prefs / files / SQLite written by apps
# survive a reboot. Idempotent: if the image already exists it is left alone
# (delete it to start clean).
#
# Usage:   meta/mkdata.sh [path] [size_mb]
# Env:     DATA_IMG (default device/qemu-virt/out/data.img)  SIZE_MB (default 64)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

DATA_IMG="${1:-${DATA_IMG:-$REPO_ROOT/device/qemu-virt/out/data.img}}"
SIZE_MB="${2:-${SIZE_MB:-64}}"

if [ -f "$DATA_IMG" ]; then
    echo "==> data image already exists: $DATA_IMG ($(du -h "$DATA_IMG" | cut -f1))"
    exit 0
fi

mkdir -p "$(dirname "$DATA_IMG")"
echo "==> creating ${SIZE_MB}MiB raw image: $DATA_IMG"
dd if=/dev/zero of="$DATA_IMG" bs=1M count="$SIZE_MB" status=none

echo "==> formatting ext4 (no journal recovery surprises under TCG)"
# -F: force on a non-block file. -q: quiet. -L: label for easy identification.
# -O ^has_journal keeps it ext2-simple (the guest kernel mounts it as ext4 all
# the same) so there is no journal replay on the second boot.
mkfs.ext4 -F -q -L zelto-data -O ^has_journal "$DATA_IMG"

echo "==> done: $DATA_IMG"
