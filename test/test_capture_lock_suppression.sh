#!/usr/bin/env bash
# test_capture_lock_suppression — the two ways a window can be exempt from the
# App Switcher's snapshot: the screen was LOCKED when it was backgrounded, or the
# app declared `no_snapshot=1` in its manifest.
#
# WHY THIS EXISTS. P42 shipped per-toplevel capture with a privacy guard: if a
# layer surface holds the screen (zelto-lock takes EXCLUSIVE keyboard
# interactivity when it locks, which is what sets server->focused_layer), the
# capture edge bails. That guard was code-reviewed and never once executed. A
# privacy control nobody has run is a claim, not a control — and this one is
# easy to break silently, because the thing it prevents is a picture that no
# screen ever shows you. Nothing looks wrong when it stops working.
#
# WHY IT IS AN A/B AND NOT A SINGLE ASSERTION. "the log contains no snapshot for
# app X" passes trivially if the boot never reached the state at all — the exact
# failure mode that put three shots photographing nothing into the P41 catalogue
# and rotted 47-settings-toggle in P42. So each run asserts a POSITIVE control
# from the same boot:
#
#   Run A (unlocked): app A is up, app B launches on top of it 6s in. A's
#     active->inactive edge fires with no lock present => the log MUST carry a
#     snapshot for A. This proves the sequence produces a capture at all.
#   Run B (locked):   identical, except the broker is seeded to lock at 4s — so
#     the SAME edge lands after the lock. The log MUST carry a suppression for A
#     and MUST NOT carry a snapshot for A. It must also carry a snapshot for the
#     LAUNCHER, which is backgrounded early in that same boot while the screen
#     was still unlocked: capture was demonstrably working in this process, and
#     the lock is what stopped it.
#
# Needs the desktop simulator (meta/run-sim.sh headless + grim + a WSLg/Wayland
# session). If that is unavailable the test SKIPS loudly rather than passing —
# an unrunnable proof must not read as a green one.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="${BUILD:-$REPO_ROOT/build-host}"

if [ ! -x "$BUILD/compositor/zcomp" ]; then
    echo "SKIP: $BUILD/compositor/zcomp not built"
    exit 0
fi
if ! command -v grim >/dev/null 2>&1; then
    echo "SKIP: grim not installed; the simulator cannot complete a run"
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-capture-lock.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# The two apps. A is up from boot; B launches later and pushes A into the
# background, which is the edge under test.
APP_A="zelto-notes";  APP_A_ID="os.zelto.notes"
APP_B="zelto-cards"

# One simulator boot. $1 = tag, $2 = settings.conf body (TAB-separated), $3 = the
# app to leave running from boot, $4 = optional extra manifest body. The whole
# run's output (which includes zcomp's own wlr_log on stderr) is captured; the
# screenshot is incidental — SHOT is what makes run-sim.sh terminate.
run_boot() {
    local tag="$1" seed="$2" extra_app="${3:-$APP_A}" manifest="${4:-}"
    local dir="$TMP/$tag"
    mkdir -p "$dir/data/apps/manifests" "$dir/xdg"
    printf '%b\n' "$seed" > "$dir/data/settings.conf"
    if [ -n "$manifest" ]; then
        # run-sim.sh regenerates the manifests it owns on every boot, but leaves
        # anything carrying the installed-at-runtime marker alone (that is how an
        # installed .zap survives a reboot). Borrow the marker so this one stands.
        { echo '# Installed at runtime by zelto-install'; printf '%b\n' "$manifest"; } \
            > "$dir/data/apps/manifests/os.zelto.widget.app"
    fi
    # A 12s window: the lock (seeded at 4s) is up well before app B lands at 6s,
    # and there is room afterwards for the activation to be logged.
    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY=12 \
    ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
    SIM_EXTRA="$extra_app" SIM_LATE_APP="$APP_B 6" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# --- Run A: no lock. The control. -----------------------------------------
LOG_A="$(run_boot unlocked 'sys.lock_enabled\t0\nsys.brightness\t5')"

snap_a="$(grep -c "capture: snapshot $APP_A_ID" "$LOG_A" 2>/dev/null || true)"
if [ "${snap_a:-0}" -lt 1 ]; then
    zt_fail "unlocked control never captured $APP_A_ID — the harness did not reach the edge under test, so the locked run below would prove nothing" \
        ">=1 'capture: snapshot $APP_A_ID'" "$snap_a (see $LOG_A)"
    zt_done
fi

# --- Run B: locked before the edge. ---------------------------------------
LOG_B="$(run_boot locked \
    'sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t4\nsys.idle_off_s\t9999\nsys.brightness\t5')"

# The lock must actually have engaged, or run B is just run A with extra steps.
if ! grep -q "capture: suppressed" "$LOG_B"; then
    zt_fail "the locked run never suppressed a capture — either the screen did not lock within the run, or the guard is not on this path" \
        "'capture: suppressed ...' present" "absent (see $LOG_B)"
    zt_done
fi

# Capture was working in THIS process before the lock: the launcher is
# backgrounded as soon as app A maps, seconds before the lock engages.
if ! grep -q "capture: snapshot os.zelto.launcher" "$LOG_B"; then
    zt_fail "the locked run captured nothing at all, including before the lock — this boot cannot distinguish 'suppressed' from 'never worked'" \
        "'capture: snapshot os.zelto.launcher' present" "absent (see $LOG_B)"
fi

# The assertion itself: app A was backgrounded under the lock, and no picture of
# it was taken.
if ! grep -q "capture: suppressed $APP_A_ID" "$LOG_B"; then
    zt_fail "$APP_A_ID was not the window whose capture was suppressed" \
        "'capture: suppressed $APP_A_ID'" "absent (see $LOG_B)"
fi
snap_b="$(grep -c "capture: snapshot $APP_A_ID" "$LOG_B" 2>/dev/null || true)"
zt_expect_eq "0" "${snap_b:-0}" \
    "a locked screen produced a snapshot of $APP_A_ID (see $LOG_B)"

# --- Run C: the app's own opt-out (manifest no_snapshot=1). ----------------
# The other half of the capture privacy story, and the half P42 left unbuilt.
# Same shape as run A — unlocked, a window pushed into the background — but the
# app in the foreground declares no_snapshot=1, so the compositor must never take
# its picture. Widget is used because run-sim.sh does not manage its manifest,
# which leaves the test free to write one.
LOG_C="$(run_boot no_snapshot 'sys.lock_enabled\t0\nsys.brightness\t5' \
    zelto-widget \
    'id=os.zelto.widget\nname=Widget\nno_snapshot=1')"

if ! grep -q "capture: os.zelto.widget declares no_snapshot" "$LOG_C"; then
    zt_fail "zcomp never resolved the manifest opt-out — the declaration did not reach the compositor, so what follows proves nothing" \
        "'capture: os.zelto.widget declares no_snapshot'" "absent (see $LOG_C)"
    zt_done
fi
if ! grep -q "capture: snapshot os.zelto.launcher" "$LOG_C"; then
    zt_fail "the opt-out run captured nothing at all — this boot cannot distinguish 'opted out' from 'never worked'" \
        "'capture: snapshot os.zelto.launcher' present" "absent (see $LOG_C)"
fi
snap_c="$(grep -c "capture: snapshot os.zelto.widget" "$LOG_C" 2>/dev/null || true)"
zt_expect_eq "0" "${snap_c:-0}" \
    "an app declaring no_snapshot=1 was photographed anyway (see $LOG_C)"

zt_done
