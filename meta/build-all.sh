set -e
sudo hwclock -s 2>/dev/null || true
cd /mnt/c/Users/mrblo/Documents/projects/zelto-os
ninja -C build-arm64 2>&1 | tail -50
echo "=== binaries ==="
ls -la build-arm64/system/bar/zelto-bar build-arm64/system/launcher/zelto-launcher build-arm64/system/apps/cards/zelto-cards 2>&1
