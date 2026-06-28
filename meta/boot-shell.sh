set -e
sudo hwclock -s 2>/dev/null || true
cd /mnt/c/Users/mrblo/Documents/projects/zelto-os
export HEADLESS=1 INJECT=1 SHOT_DELAY=75
bash meta/run-qemu.sh 2>&1 | tail -80
echo "=== frames ==="
ls -la device/qemu-virt/out/*.png 2>&1
