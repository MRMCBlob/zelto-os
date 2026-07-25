#!/usr/bin/env bash
# test_camera_privacy_sim — the three privacy controls around the camera, each
# EXECUTED rather than asserted about.
#
# "A privacy control nobody has run is a claim, not a control" is already written
# in test_capture_lock_suppression, and it applies twice as hard here: every one
# of these is invisible when it breaks. A camera that keeps streaming under the
# lock screen looks exactly like one that stopped. An indicator that an app has
# switched off looks exactly like an app that is not using the camera.
#
#   1. THE INDICATOR IS SHOWN while a stream is open. The bar draws a dot; a dot
#      carries no string, so the assertion reads the bar's own marks line (the
#      P47 idiom — a 10-unit disc moves the 720x81 strip by less than the clock
#      does between boots, so a pixel check could not tell them apart).
#
#   2. THE INDICATOR CANNOT BE SUPPRESSED BY AN APP. This is the one that
#      matters. sys.camera_in_use is published by the broker from its own record
#      and refused to client writes; without that refusal the app being indicated
#      could clear the key and go on streaming behind a clean status bar. An
#      ordinary app is made to attempt exactly that write here, and the run must
#      show the broker refusing it AND the dot still lit afterwards.
#
#   3. LOCKING THE SCREEN STOPS THE STREAM. Found broken by measurement, not by
#      review: taking the screen with a modal layer used to change only the
#      keyboard route, so the app underneath kept its xdg activated state and a
#      preview opened before the lock went on producing frames for the rest of
#      the boot. The fix is in the compositor (layer.c); this is what holds it.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-cam-privacy.XXXXXX")"
trap '[ "${ZT_FAILURES:-0}" -eq 0 ] && rm -rf "$TMP" \
      || echo "kept the simulator logs: $TMP" >&2' EXIT

# $1 = tag, $2 = settings.conf body, $3 = run seconds, $4.. = extra env.
run_boot() {
    local tag="$1" seed="$2" secs="$3"; shift 3
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/xdg" "$dir/lib"
    printf '%b\n' "$seed" > "$dir/data/settings.conf"

    env HEADLESS=1 SKIP_BUILD=1 SIM_SIZE=720x1440 \
        SHOT="$dir/frame.png" SHOT_DELAY="$secs" \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PHOTOS_ROOT="$dir/lib" ZELTO_CONSENT_BIN=/bin/true \
        SIM_APP=zelto-camera "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir"
}

# ---------------------------------------------------------------------------
# 1 + 2. The indicator lights, and an app cannot put it out.
# ---------------------------------------------------------------------------
# THE CAMERA APP ITSELF attempts the write, while it is streaming and in the
# foreground (ZELTO_CAMERA_SUPPRESS). That is the only faithful simulation: the
# app being indicated is the one with a motive to hide, and it is the one app
# certain to be in front while its own stream is running.
#
# The first version of this test used a SECOND app to make the write, and it
# failed on its own setup — a second app must take the foreground to run, which
# backgrounds the camera, stops the stream and puts the dot out entirely
# legitimately. The bug was in the test rather than in the OS, and it is recorded
# here because the same trap waits for anyone testing a foreground-only control.
DIR_A="$(run_boot suppress 'sys.lock_enabled\t0\nsys.brightness\t5' 16 \
    ZELTO_CAMERA_SUPPRESS=1)"
LOG_A="$DIR_A/log"

if ! grep -q "\[zsysd\] camera OPEN app=os.zelto.camera" "$LOG_A"; then
    zt_fail "no camera stream was ever opened — nothing below is evidence" \
        "'[zsysd] camera OPEN app=os.zelto.camera'" "absent (see $LOG_A)"
    zt_done
fi

# 1. The bar SHOWS it. Read off the bar's own account of what it drew.
if ! grep -q "\[bar\] marks .*camera=in-use:[1-9]" "$LOG_A"; then
    zt_fail "the status bar never showed the camera in-use indicator while a stream was open — the dot is the whole point of the broker publishing this" \
        "'[bar] marks ... camera=in-use:N' with N >= 1" "absent (see $LOG_A)"
fi

# 2a. The broker REFUSED the app's write. Without this line the assertion below
# would also pass on a boot where the app simply never got around to trying.
if ! grep -q "zelto-camera: attempting to clear my own in-use indicator" "$LOG_A"; then
    zt_fail "the app never attempted to suppress its own indicator — this run does not exercise the refusal at all" \
        "'zelto-camera: attempting to clear my own in-use indicator'" \
        "absent (see $LOG_A)"
    zt_done
fi
if ! grep -q "settings_set REFUSED sys.camera_in_use" "$LOG_A"; then
    zt_fail "the broker ACCEPTED an app's write to the key describing that app — the indicator is suppressible and therefore worthless" \
        "'[zsysd] settings_set REFUSED sys.camera_in_use'" "absent (see $LOG_A)"
    zt_done
fi

# 2b. AND THE INDICATOR IS STILL LIT AFTERWARDS. The refusal log alone is not
# enough: what matters is the state the bar ends up in, so this reads the LAST
# marks line of the boot rather than any of them.
last_marks="$(grep "\[bar\] marks " "$LOG_A" | tail -1)"
case "$last_marks" in
    *camera=in-use:[1-9]*) ;;
    *) zt_fail "an app cleared the camera in-use indicator — it went out while the camera was still streaming, which is the failure the broker-owned key exists to prevent" \
           "the last bar marks still carrying camera=in-use:N" "$last_marks (see $LOG_A)" ;;
esac

# ---------------------------------------------------------------------------
# 3. Locking the screen stops the stream.
# ---------------------------------------------------------------------------
# The camera opens ~6s in; the lock engages at 8s. A run of 18s leaves ten
# seconds in which a stream that ignored the lock would keep logging frames.
DIR_B="$(run_boot locked \
    'sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t8\nsys.idle_off_s\t9999\nsys.brightness\t5' \
    18)"
LOG_B="$DIR_B/log"

# The positive control: it really was streaming before the lock. Otherwise
# "paused" is indistinguishable from "never started".
if ! grep -q "zelto: camera open" "$LOG_B"; then
    zt_fail "the locked run never opened a stream — it cannot tell 'stopped by the lock' from 'never ran'" \
        "'zelto: camera open' before the lock" "absent (see $LOG_B)"
    zt_done
fi
if ! grep -q "\[zsysd\] camera OPEN" "$LOG_B"; then
    zt_fail "the broker never recorded the pre-lock stream — the control for the CLOSE assertion below is missing" \
        "'[zsysd] camera OPEN'" "absent (see $LOG_B)"
    zt_done
fi

if ! grep -q "zelto: camera paused (backgrounded)" "$LOG_B"; then
    zt_fail "the camera kept streaming under the lock screen — a locked phone was reading its camera, which is the defect this whole stage exists to close" \
        "'zelto: camera paused (backgrounded)' once the screen locked" \
        "absent (see $LOG_B)"
fi
# And the broker agrees nothing is streaming, so the indicator goes out with it.
if ! grep -q "\[zsysd\] camera CLOSE app=os.zelto.camera (0 in use)" "$LOG_B"; then
    zt_fail "the broker still believes a camera is in use after the screen locked — the indicator would keep claiming a stream that stopped" \
        "'[zsysd] camera CLOSE app=os.zelto.camera (0 in use)'" "absent (see $LOG_B)"
fi

zt_done
