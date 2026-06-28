#!/usr/bin/env bash
# Build (if needed) and launch Zelto OS in QEMU (aarch64 'virt').
#
# Boots the mainline kernel + tiny initramfs, attaches a virtio-gpu display and
# virtio input, and runs zcomp -> a rendered frame. This is the success path for
# the bootstrap task.
#
# Usage:
#   meta/run-qemu.sh              # build everything, show a GTK window (WSLg)
#   HEADLESS=1 meta/run-qemu.sh   # no window; dump a frame to out/frame.ppm
#   SKIP_BUILD=1 meta/run-qemu.sh # just launch with existing artifacts
#
# Env:
#   MEM (default 2048)   SMP (default 4)   CPU (default cortex-a72)
#   SHOT_DELAY (default 8) seconds to wait before the headless screendump
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
OUT="$REPO_ROOT/device/qemu-virt/out"

MEM="${MEM:-4096}"
SMP="${SMP:-4}"
CPU="${CPU:-cortex-a72}"
SHOT_DELAY="${SHOT_DELAY:-16}"
# There is no aarch64 KVM on an x86 host, so QEMU emulates via TCG. Multi-threaded
# TCG (one host thread per vCPU) plus a larger translation-block cache is the
# biggest lever we have on the lag (the "[libinput] your system is too slow"
# warning is the guest clock outrunning emulation). Override with ACCEL= if a
# host ever offers KVM.
ACCEL="${ACCEL:-tcg,thread=multi,tb-size=1024}"

KERNEL="$OUT/Image"
INITRD="$OUT/initramfs.cpio.gz"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> [1/3] cross-build zcomp (aarch64)"
    if [ ! -d "$REPO_ROOT/build-arm64" ]; then
        meson setup "$REPO_ROOT/build-arm64" "$REPO_ROOT" \
            --cross-file "$REPO_ROOT/meta/cross/aarch64-linux-gnu.txt"
    fi
    ninja -C "$REPO_ROOT/build-arm64"

    echo "==> [2/3] kernel"
    [ -f "$KERNEL" ] || "$REPO_ROOT/device/qemu-virt/build-kernel.sh"

    echo "==> [3/3] initramfs"
    "$REPO_ROOT/meta/initramfs/build-initramfs.sh"
fi

[ -f "$KERNEL" ] || { echo "ERROR: kernel missing ($KERNEL); run device/qemu-virt/build-kernel.sh"; exit 1; }
[ -f "$INITRD" ] || { echo "ERROR: initramfs missing ($INITRD)"; exit 1; }

# QEMU's kernel loader cannot mmap files on a WSL 9p/drvfs mount (/mnt/c/...).
# If the artifacts live there, stage them to a native tmp dir before launching.
case "$KERNEL" in
    /mnt/*)
        STAGE="$(mktemp -d "${TMPDIR:-/tmp}/zelto-run.XXXXXX")"
        echo "==> staging kernel+initramfs to native fs ($STAGE) for QEMU"
        cp -f "$KERNEL" "$STAGE/Image"
        cp -f "$INITRD" "$STAGE/initramfs.cpio.gz"
        KERNEL="$STAGE/Image"
        INITRD="$STAGE/initramfs.cpio.gz"
        trap 'rm -rf "$STAGE"' EXIT
        ;;
esac

# Common QEMU arguments.
common=(
    -M virt
    -accel "$ACCEL"
    -cpu "$CPU"
    -smp "$SMP"
    -m "$MEM"
    -kernel "$KERNEL"
    -initrd "$INITRD"
    # virtio-gpu (DRM/KMS in guest) + virtio input.
    -device virtio-gpu-pci
    -device virtio-keyboard-pci
    -device virtio-tablet-pci
    -no-reboot
)

KCMD="console=ttyAMA0 rdinit=/init loglevel=7"

if [ "${HEADLESS:-0}" = "1" ]; then
    echo "==> launching QEMU headless; frame -> $OUT/frame.ppm after ${SHOT_DELAY}s"
    rm -f "$OUT/frame.ppm" "$OUT/frame-after.ppm"
    # The QMP unix socket must live on a native fs (9p/drvfs can't bind sockets).
    QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
    rm -f "$QMP_SOCK"
    qemu-system-aarch64 "${common[@]}" \
        -append "$KCMD" \
        -display none \
        -serial mon:stdio \
        -qmp "unix:$QMP_SOCK,server,nowait" &
    QPID=$!

    have_socat=0
    command -v socat >/dev/null 2>&1 && have_socat=1
    [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

    # Send one QMP command (JSON, sans the capabilities handshake) to the socket.
    qmp() {
        [ "$have_socat" = "1" ] || return 0
        printf '%s\n' '{"execute":"qmp_capabilities"}' "$1" \
            | socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
    }
    # PPM -> PNG (netpbm / ImageMagick / pure-python fallback).
    to_png() {
        [ -f "$1" ] || return 0
        echo "==> wrote $1"
        if command -v pnmtopng >/dev/null 2>&1; then
            pnmtopng "$1" > "$2" 2>/dev/null && echo "==> wrote $2"
        elif command -v convert >/dev/null 2>&1; then
            convert "$1" "$2" && echo "==> wrote $2"
        elif command -v python3 >/dev/null 2>&1; then
            python3 "$REPO_ROOT/meta/ppm2png.py" "$1" "$2" && echo "==> wrote $2"
        fi
    }

    # Wait for the GPU + compositor to paint, then screendump the "before" frame.
    sleep "$SHOT_DELAY"
    qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/frame.ppm\"}}"
    sleep 1
    to_png "$OUT/frame.ppm" "$OUT/frame.png"

    # Optional: drive the P5 list/navigation sample over QMP input-send-event and
    # capture a *sequence* of frames proving each stage. The flow is one path (no
    # fork): wheel-scroll the list, pan-drag it (multi-step, releases into a
    # fling), tap a row to push the detail screen (slide transition), then press
    # Escape (system back) to pop it. Each stage dumps a PNG. Coordinates are
    # on-screen pixels mapped into the 0..32767 absolute input range.
    if [ "${INJECT:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        CX="${CX:-640}"                   # horizontal centre (over the list)
        ROW_Y="${ROW_Y:-150}"             # y of a row to tap (just below navbar)
        BACK_KEY="${BACK_KEY:-esc}"       # system-back key (Escape / backspace)
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        # Move the absolute pointer to (x,y) px.
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {  # btn down|up
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        wheel() {  # wheel-down|wheel-up
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"$1\"}}]}}"
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"$1\"}}]}}"
        }
        keypress() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
        }
        shot() {  # shot NAME : screendump <out>/NAME.ppm -> NAME.png
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }

        echo "==> [inject 1/4] wheel-scroll the list"
        move "$CX" 400
        for _ in 1 2 3 4 5; do wheel wheel-down; sleep 0.2; done
        sleep 1; shot frame-scroll

        echo "==> [inject 2/4] pan-drag (multi-step) -> fling"
        move "$CX" 540; btn down
        for yy in 480 420 360 300 240 180; do move "$CX" "$yy"; sleep 0.12; done
        btn up
        sleep 2; shot frame-drag       # captures mid-fling / settled scroll

        echo "==> [inject 3/4] tap a row -> push detail (slide transition)"
        move "$CX" "$ROW_Y"; btn down; btn up
        sleep 1; shot frame-detail-mid # mid slide-in
        sleep 2; shot frame-detail     # settled detail screen

        echo "==> [inject 4/4] system back ('$BACK_KEY') -> pop"
        keypress "$BACK_KEY"
        sleep 1; shot frame-back-mid   # mid slide-out
        sleep 2; shot frame-after      # back on the list (settled)
    fi

    kill "$QPID" 2>/dev/null || true
else
    echo "==> launching QEMU with GTK display (WSLg)"
    exec qemu-system-aarch64 "${common[@]}" \
        -append "$KCMD" \
        -display gtk,gl=off \
        -serial mon:stdio
fi
