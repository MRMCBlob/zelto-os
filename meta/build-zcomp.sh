set -e
sudo hwclock -s 2>/dev/null || true
cd /mnt/c/Users/mrblo/Documents/projects/zelto-os
if [ ! -d build-arm64 ]; then
  meson setup build-arm64 . --cross-file meta/cross/aarch64-linux-gnu.txt
fi
ninja -C build-arm64 compositor/zcomp 2>&1 | tail -40
