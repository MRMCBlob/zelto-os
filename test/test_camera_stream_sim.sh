#!/usr/bin/env bash
# test_camera_stream_sim — the camera stream, and the four claims a SYNTHETIC
# source can still honestly make.
#
# THE PROBLEM THIS TEST EXISTS TO SOLVE. There is no camera in the simulator or
# in QEMU, so the frames are generated (the decision and its alternatives are
# written up at the top of sdk/src/camera.c). A synthetic source that only ever
# proves itself is the shape of a test that passes and means nothing — so the
# source encodes EVERY FRAME'S SEQUENCE NUMBER in its own top-left pixel, and
# this test decodes that out of the written PNG. That turns "the fake produced a
# fake" into a real assertion about the plumbing: the still is the frame that was
# LIVE when the shutter fired, not a stale buffer, not a blank one, and not a
# frame whose colour channels got reordered on the way to disk.
#
# WHAT IS ASSERTED HERE
#   1. A granted stream produces frames and the capture keeps the live one.
#   2. The broker records who is streaming — the fact a privacy indicator must
#      be built on, since the app being indicated does not draw it.
#   3. Backgrounding STOPS the stream, with a positive control from the same
#      boot (it was demonstrably running first).
#   4. A DENIED permission produces no stream at all.
#
# WHAT IS NOT ASSERTED, and cannot be here: that a physical sensor's pixels reach
# this API. That is the device port's job and camera_source_fill is where it
# attaches.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="${BUILD:-$REPO_ROOT/build-host}"

if [ ! -x "$BUILD/system/apps/camera/zelto-camera" ]; then
    echo "SKIP: $BUILD/system/apps/camera/zelto-camera not built"
    exit 0
fi
if ! command -v grim >/dev/null 2>&1; then
    echo "SKIP: grim not installed; the simulator cannot complete a run"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not installed; cannot decode a PNG pixel"
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-camera.XXXXXX")"
trap '[ "${ZT_FAILURES:-0}" -eq 0 ] && rm -rf "$TMP" \
      || echo "kept the simulator logs and captures: $TMP" >&2' EXIT

# $1 = tag, $2 = consent binary (/bin/true grants, /bin/false denies),
# $3 = run seconds, $4.. = extra env. Echoes the run dir.
run_boot() {
    local tag="$1" consent="$2" secs="$3"; shift 3
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/xdg" "$dir/lib"
    printf 'sys.lock_enabled\t0\nsys.brightness\t5\n' > "$dir/data/settings.conf"

    env HEADLESS=1 SKIP_BUILD=1 SIM_SIZE=720x1440 \
        SHOT="$dir/frame.png" SHOT_DELAY="$secs" \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PHOTOS_ROOT="$dir/lib" \
        ZELTO_CONSENT_BIN="$consent" \
        SIM_APP=zelto-camera "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir"
}

# The red channel of the top-left pixel — the frame's own sequence marker.
png_marker() {
    python3 - "$1" <<'PY'
import struct, sys, zlib

with open(sys.argv[1], 'rb') as f:
    data = f.read()
pos, w, h, ctype, idat = 8, 0, 0, 0, b''
while pos < len(data):
    ln = struct.unpack('>I', data[pos:pos + 4])[0]
    tag = data[pos + 4:pos + 8]
    body = data[pos + 8:pos + 8 + ln]
    if tag == b'IHDR':
        w, h, _, ctype = struct.unpack('>IIBB', body[:10])
    elif tag == b'IDAT':
        idat += body
    pos += 12 + ln
# Channels per pixel: 3 = RGB (what an opaque capture writes), 6 = RGBA.
ch = 3 if ctype == 2 else (4 if ctype == 6 else 0)
if ch == 0:
    print("badtype")
else:
    raw = zlib.decompress(idat)
    # The first scanline's filter byte, then its first pixel's red channel.
    # Filter 0 (None) or 1 (Sub) both leave byte 0 of the row untouched; 2 (Up)
    # on the FIRST row treats the missing row above as zero, so it does too.
    print(raw[1] if raw[0] in (0, 1, 2) else "filter%d" % raw[0])
PY
}

# ---------------------------------------------------------------------------
# Run A — granted. The stream runs and the shutter keeps the live frame.
# ---------------------------------------------------------------------------
DIR_A="$(run_boot granted /bin/true 14 \
    ZELTO_TAP_LABEL=Capture ZELTO_TAP_APP=os.zelto.camera ZELTO_TAP_AT=10000)"
LOG_A="$DIR_A/log"

if ! grep -q "zelto: camera open" "$LOG_A"; then
    zt_fail "the granted run never opened a stream — nothing below is evidence" \
        "'zelto: camera open WxH @Nfps'" "absent (see $LOG_A)"
    zt_done
fi
# The broker's own record. This is the load-bearing one for a privacy indicator:
# the system knows the camera is live from the BROKER, not from the app.
if ! grep -q "\[zsysd\] camera OPEN app=os.zelto.camera" "$LOG_A"; then
    zt_fail "the broker was never told the camera opened — an in-use indicator built on this would show nothing while the camera ran" \
        "'[zsysd] camera OPEN app=os.zelto.camera'" "absent (see $LOG_A)"
fi

cap_line="$(grep -m1 "zelto-camera: captured" "$LOG_A" || true)"
if [ -z "$cap_line" ]; then
    zt_fail "the shutter never wrote a still" \
        "'zelto-camera: captured <id> from frame N'" "absent (see $LOG_A)"
    zt_done
fi
shot_id="$(echo "$cap_line" | sed -n 's/.*captured \([0-9-]*\) from frame.*/\1/p')"
shot_seq="$(echo "$cap_line" | sed -n 's/.*from frame \([0-9]*\).*/\1/p')"

# The stream really ran rather than firing once: a 15fps preview reaches frame 10
# long before the shutter at 10s. This is the "frames advance" claim, and it is
# read off the frame the CAPTURE came from so it cannot pass on a stream that
# produced exactly one.
if [ "${shot_seq:-0}" -lt 10 ]; then
    zt_fail "the capture came from frame $shot_seq — the preview is not advancing, so this is one frame rather than a stream" \
        ">= frame 10 by the time the shutter fires" "$shot_seq (see $LOG_A)"
fi

photo="$DIR_A/lib/photos/$shot_id.png"
if [ ! -f "$photo" ]; then
    zt_fail "the capture did not land in the shared photo library" \
        "$photo" "absent"
    zt_done
fi

# THE ASSERTION THE SYNTHETIC SOURCE EXISTS TO MAKE POSSIBLE. Every frame writes
# its own sequence number into its top-left pixel, so the still can be proven to
# be the frame that was live when the shutter fired.
marker="$(png_marker "$photo")"
expect_marker="$(( shot_seq % 256 ))"
zt_expect_eq "$expect_marker" "$marker" \
    "the saved still is not the frame that was live when the shutter fired (its marker says frame %256=$marker, the app captured frame $shot_seq) — a stale buffer, a blank one, or a channel swap (see $photo)"

# ---------------------------------------------------------------------------
# Run B — backgrounded. The stream must STOP, and the run must show it ran first.
# ---------------------------------------------------------------------------
DIR_B="$(run_boot background /bin/true 16 SIM_LATE_APP="zelto-notes 10")"
LOG_B="$DIR_B/log"

# The positive control: it was streaming before anything covered it. Without this
# "no frames while backgrounded" is also true of a run where the camera never
# started at all.
if ! grep -q "zelto: camera open" "$LOG_B"; then
    zt_fail "the background run never started a stream — it cannot distinguish 'paused' from 'never ran'" \
        "'zelto: camera open' (the pre-background control)" "absent (see $LOG_B)"
    zt_done
fi
if ! grep -q "zelto: camera paused (backgrounded)" "$LOG_B"; then
    zt_fail "the camera kept streaming after the app left the foreground — the privacy rule the sensors route established in P39 is not on this path" \
        "'zelto: camera paused (backgrounded)'" "absent (see $LOG_B)"
fi
# And the broker must be told, or an indicator would keep showing a camera that
# is no longer running.
if ! grep -q "\[zsysd\] camera CLOSE app=os.zelto.camera" "$LOG_B"; then
    zt_fail "the broker still believes the camera is streaming after the app was backgrounded" \
        "'[zsysd] camera CLOSE app=os.zelto.camera'" "absent (see $LOG_B)"
fi

# ---------------------------------------------------------------------------
# Run C — denied. No stream at all.
# ---------------------------------------------------------------------------
DIR_C="$(run_boot denied /bin/false 12)"
LOG_C="$DIR_C/log"

if ! grep -q "zelto-camera: camera denied" "$LOG_C"; then
    zt_fail "the denied run never reported a denial — the consent path did not run, so a clean 'no stream' below proves nothing" \
        "'zelto-camera: camera denied; no stream opened'" "absent (see $LOG_C)"
    zt_done
fi
opened="$(grep -c "zelto: camera open" "$LOG_C" 2>/dev/null || true)"
zt_expect_eq "0" "${opened:-0}" \
    "a denied permission still opened a camera stream (see $LOG_C)"
brokered="$(grep -c "\[zsysd\] camera OPEN" "$LOG_C" 2>/dev/null || true)"
zt_expect_eq "0" "${brokered:-0}" \
    "the broker recorded a camera in use for an app that was denied (see $LOG_C)"

zt_done
