set -e
sudo hwclock -s 2>/dev/null || true
cd /mnt/c/Users/mrblo/Documents/projects/zelto-os
ninja -C build-arm64 sdk/libzelto.a samples/hello/zelto-hello 2>&1 | tail -40
