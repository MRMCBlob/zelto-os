#!/usr/bin/env bash
# test_power_services_sim — battery and volume are SYSTEM SERVICES, and a change
# in one process actuates another.
#
# WHAT THIS REPLACES. VOLUME=1, the last of the five harnesses P44 disabled with
# a claim covered nowhere else: that the two hardware indicators every phone
# shows are zsysd duties published through the settings broker, and that a volume
# change pops the rocker HUD. It opened the app drawer P40 deleted to launch
# Settings and ended in an unconditional exit 0.
#
# It is a SIM test for the same reason test_settings_broker_sim is: nothing here
# needs the ARM target — it is zsysd's power source, the broker, and two overlay
# clients — and a QEMU two-boot costs ~6 minutes where this costs seconds.
#
# THE CHAIN UNDER TEST, which is worth more than any single assertion: the fake
# battery drains -> crosses the low threshold -> zsysd writes sys.brightness
# down -> the broker fans that out -> zelto-dim, a DIFFERENT PROCESS that knows
# nothing about batteries, paints a stronger scrim. Three processes, ending in a
# real pixel-level actuation. If any link is broken the last log line never
# appears.
#
# Both halves carry a CONTROL, because "the battery did not drain" and "the HUD
# did not pop" are claims about absence, which pass trivially on a boot that
# never got started.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-power.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# zsysd's low-battery threshold; start just above it so a few fast ticks cross
# it inside the run rather than minutes later.
START_PCT=22
# brightness 5 -> the low-battery nudge takes it to 2, which zelto-dim turns into
# a scrim of (5-2)*48 = 144. Seeded at 5 so the change is unambiguous: the guard
# in check_low_battery only dims when brightness is already >= 3.
EXPECT_ALPHA=144

run_boot() {   # tag, extra env assignments as "K=V K=V", seed body
    local tag="$1" env_extra="$2" seed="${3:-}"
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/xdg"
    [ -n "$seed" ] && printf '%b\n' "$seed" > "$dir/data/settings.conf"
    env $env_extra \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=12 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# --- A. the battery source runs, drains, and actuates the screen -----------
LOG_A="$(run_boot drain \
    "ZELTO_FAKE_BATTERY=1 ZELTO_BATTERY_START=$START_PCT ZELTO_BATTERY_TICK_MS=400" \
    'sys.brightness\t5\nsys.lock_enabled\t0')"

mapfile -t pcts < <(sed -n 's/.*\[zsysd\] battery \([0-9]*\)%.*/\1/p' "$LOG_A")
if [ "${#pcts[@]}" -lt 2 ]; then
    zt_fail "the battery service produced fewer than two ticks — there is no drain to observe, so nothing below would mean anything" \
        ">= 2 '[zsysd] battery N%' lines" "${#pcts[@]} (see $LOG_A)"
    zt_done
fi

# It must DRAIN, not merely report. A source stuck at its seed value would
# publish happily forever and never actuate anything.
first="${pcts[0]}"; last="${pcts[${#pcts[@]}-1]}"
if [ "$last" -ge "$first" ]; then
    zt_fail "the fake battery never discharged" \
        "a falling percentage" "$first -> $last (see $LOG_A)"
fi

# THE CROSS-PROCESS ACTUATION. zelto-dim knows nothing about batteries; it only
# observes sys.brightness. This line appearing means the drain crossed zsysd's
# threshold, zsysd wrote brightness down, the broker fanned it out, and a
# separate process changed what is on the screen.
if ! grep -q "\[dim\] sys.brightness=2 -> scrim alpha=$EXPECT_ALPHA" "$LOG_A"; then
    zt_fail "a low battery did not dim the screen — the drain crossed zsysd's threshold but no other process acted on it" \
        "'[dim] sys.brightness=2 -> scrim alpha=$EXPECT_ALPHA'" "absent (see $LOG_A)"
fi

# --- B. the control: no source, no drain, no nudge -------------------------
# Same binaries, same seed, the source switched off. Without this, run A's
# assertions are satisfied by anything that happens to write brightness.
LOG_B="$(run_boot nosource "ZELTO_FAKE_BATTERY=0" \
    'sys.brightness\t5\nsys.lock_enabled\t0')"

# NOT "zero ticks". With the fake source off zsysd falls back to the REAL sysfs
# battery, and the machine this suite runs on is a laptop — so the control does
# report ticks, at whatever the host's actual charge is. That is correct
# behaviour and the first version of this test asserted it away.
#
# The discriminator is DRAIN, not presence: the fake source falls 1% per tick by
# construction, a real one does not move measurably in a twelve-second run. So
# the control must be FLAT.
mapfile -t pcts_b < <(sed -n 's/.*\[zsysd\] battery \([0-9]*\)%.*/\1/p' "$LOG_B")
if [ "${#pcts_b[@]}" -ge 2 ]; then
    fb="${pcts_b[0]}"; lb="${pcts_b[${#pcts_b[@]}-1]}"
    if [ "$((fb - lb))" -gt 1 ]; then
        zt_fail "the control boot drained too — run A's discharge is not evidence of the fake source" \
            "a flat percentage (a real battery does not move in 12s)" \
            "$fb -> $lb (see $LOG_B)"
    fi
fi
if grep -q "\[dim\] sys.brightness=2 " "$LOG_B"; then
    zt_fail "the screen dimmed to the low-battery level with no fake drain — run A's dim proves nothing" \
        "no low-battery nudge" "present (see $LOG_B)"
fi

# --- C. a volume change pops the HUD, in another process -------------------
# The Settings app writes sys.volume through the broker; zelto-volume is a
# separate overlay client that observes it and springs the rocker in. Driven by
# the write, not by a media key, so there is no keycode or coordinate involved.
VOL=7
LOG_C="$(run_boot volume \
    "ZELTO_FAKE_BATTERY=0 SIM_APP=zelto-settings ZELTO_SETTINGS_SET=sys.volume:$VOL" \
    'sys.brightness\t5\nsys.lock_enabled\t0\nsys.volume\t3')"

if ! grep -q "\[zsysd\] settings_set sys.volume=$VOL" "$LOG_C"; then
    zt_fail "sys.volume was never written through the broker, so the HUD had nothing to react to" \
        "'[zsysd] settings_set sys.volume=$VOL'" "absent (see $LOG_C)"
    zt_done
fi
if ! grep -q "\[volume\] HUD shown: volume=$VOL" "$LOG_C"; then
    zt_fail "the volume HUD did not pop for a brokered volume change" \
        "'[volume] HUD shown: volume=$VOL'" "absent (see $LOG_C)"
fi

# And it popped for the NEW value, not merely at startup on the seeded one: the
# seed is 3, so a HUD reporting 3 would mean the overlay came up on its own.
if grep -q "\[volume\] HUD shown: volume=3 " "$LOG_C"; then
    zt_fail "the HUD popped on the seeded value, not the written one — it is showing at startup rather than reacting to a change" \
        "only volume=$VOL" "a volume=3 pop too (see $LOG_C)"
fi

zt_done
