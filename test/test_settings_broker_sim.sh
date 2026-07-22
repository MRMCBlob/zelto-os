#!/usr/bin/env bash
# test_settings_broker_sim — a brokered setting is ONE source of truth: written
# by one process, seen live by the others, and still there after a reboot.
#
# WHAT THIS REPLACES. Two of the five harnesses P44 disabled encoded this claim
# and nothing else covered it:
#   SETTINGS=1 — the Settings app and the shade read/write the same keys, live
#                in both directions, persisted across a power cycle.
#   LOCK=1     — the idle/lock TIMEOUTS survive a reboot.
# Both swiped up to the app drawer P40 deleted and tapped toggle chips at
# coordinates read off a screenshot, and both ended in an unconditional exit 0,
# so from P40 to P44 they injected a swipe that now means Home, tapped wherever
# stale numbers landed, and reported success.
#
# WHY IT IS A SIM TEST AND NOT A QEMU HARNESS. Nothing in this claim needs the
# ARM target: it is zsysd, its clients and an ext4-backed settings.conf, all of
# which the simulator runs natively. A QEMU two-boot costs ~6 minutes under TCG
# and cannot be run from test/run-tests.sh; two sim boots cost seconds and this
# lands in the suite with everything else. The "reboot" is a second boot pointed
# at the SAME ZELTO_DATA_DIR — the same thing the QEMU test proved with a shared
# data.img, minus the emulator.
#
# WHAT MAKES IT NON-VACUOUS. Three runs, and the third is a CONTROL:
#   Run 1 (write)   — the Settings app writes two keys through the broker.
#                     zsysd reports "settings_set K=V -> N subscriber(s)"; N is
#                     the live cross-process fan-out, and N=0 would mean the
#                     write went nowhere but this one process.
#   Run 2 (reload)  — a second boot on the SAME data dir. zelto-lock, a DIFFERENT
#                     process that was not running when the value was written,
#                     must report the persisted timeout in its own config line.
#   Run 3 (control) — an EMPTY data dir, same binaries, same everything. It must
#                     report the COMPILED-IN default instead. Without this, run 2
#                     passes just as well if the value it read was the default
#                     all along.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-settings-broker.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# A lock timeout that is NOT the compiled-in default, so run 2 reading it back
# cannot be confused with run 2 reading the default. zelto-lock prints its whole
# config on startup ("config enabled=N dim=Ns lock=Ns off=Ns"), which is the one
# line in the system that reports a persisted setting from a process that did
# not write it.
LOCK_S=33
WIFI=0

# One simulator boot against a given data dir. $1 = tag, $2 = data dir,
# $3 = optional ZELTO_SETTINGS_SET spec for the Settings app.
run_boot() {
    local tag="$1" data="$2" setspec="${3:-}"
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$data" "$dir/xdg"
    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY=10 \
    ZELTO_DATA_DIR="$data" SIM_RUNTIME_DIR="$dir/xdg" \
    SIM_APP="${setspec:+zelto-settings}" \
    ZELTO_SETTINGS_SET="$setspec" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

DATA_SHARED="$TMP/shared-data"
DATA_FRESH="$TMP/fresh-data"

# --- Run 1: write two keys through the broker ------------------------------
LOG1="$(run_boot write "$DATA_SHARED" "sys.wifi:$WIFI,sys.idle_lock_s:$LOCK_S")"

# The write happened at all.
if ! grep -q "\[zsysd\] settings_set sys.idle_lock_s=$LOCK_S" "$LOG1"; then
    zt_fail "the Settings app never wrote sys.idle_lock_s through the broker — nothing was persisted, so runs 2 and 3 would prove nothing" \
        "'[zsysd] settings_set sys.idle_lock_s=$LOCK_S'" "absent (see $LOG1)"
    zt_done
fi

# THE LIVE CROSS-PROCESS HALF. zsysd fans a write out to every subscribed
# observer; the count it prints is how many OTHER processes saw it without a
# reboot. Zero means the broker is a private variable.
subs="$(sed -n "s/.*settings_set sys\.wifi=$WIFI -> \([0-9]*\) subscriber(s).*/\1/p" \
    "$LOG1" | tail -1)"
if [ -z "$subs" ]; then
    zt_fail "no subscriber count was reported for the sys.wifi write" \
        "'settings_set sys.wifi=$WIFI -> N subscriber(s)'" "absent (see $LOG1)"
elif [ "$subs" -lt 1 ]; then
    zt_fail "a brokered write reached NO other process — the shade, bar, dim and lock all observe these keys, so a fan-out of 0 means the broadcast is broken" \
        ">= 1 subscriber" "$subs (see $LOG1)"
fi

# --- Run 2: a fresh boot on the SAME data dir ------------------------------
# No writer this time: whatever zelto-lock reports it read off the disk.
LOG2="$(run_boot reload "$DATA_SHARED")"

if ! grep -q "settings loaded from" "$LOG2"; then
    zt_fail "the second boot's broker never loaded a settings file" \
        "'settings loaded from ...'" "absent (see $LOG2)"
fi
got2="$(sed -n 's/.*zelto-lock: config .*lock=\([0-9]*\)s.*/\1/p' "$LOG2" | tail -1)"
if [ -z "$got2" ]; then
    zt_fail "zelto-lock never reported its config on the reload boot" \
        "'zelto-lock: config ... lock=Ns'" "absent (see $LOG2)"
    zt_done
fi
zt_expect_eq "$LOCK_S" "$got2" \
    "the lock timeout did not survive the reboot — zelto-lock is a different process from the one that wrote it, and it read $got2 (see $LOG2)"

# Nothing re-wrote it on this boot, or "persisted" would mean "written again".
if grep -q "settings_set sys.idle_lock_s" "$LOG2"; then
    zt_fail "the reload boot WROTE sys.idle_lock_s again, so its value proves nothing about persistence" \
        "no settings_set on the reload boot" "present (see $LOG2)"
fi

# --- Run 3: the control — an empty data dir --------------------------------
# Same binaries, same sequence, nothing on disk. It must fall back to the
# compiled-in default, which is what makes run 2 evidence rather than a
# coincidence.
LOG3="$(run_boot control "$DATA_FRESH")"
got3="$(sed -n 's/.*zelto-lock: config .*lock=\([0-9]*\)s.*/\1/p' "$LOG3" | tail -1)"
if [ -z "$got3" ]; then
    zt_fail "zelto-lock never reported its config on the control boot" \
        "'zelto-lock: config ... lock=Ns'" "absent (see $LOG3)"
elif [ "$got3" = "$LOCK_S" ]; then
    zt_fail "the control boot ALSO read $LOCK_S with an empty data dir — run 2 was reading the default, not the persisted value" \
        "the compiled-in default, != $LOCK_S" "$got3 (see $LOG3)"
fi

zt_done
