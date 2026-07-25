#!/usr/bin/env bash
# test_theme_setting_sim — the appearance is ONE brokered value: written by
# Settings, applied by EVERY process without a reboot, and still there after one.
#
# WHY IT IS A SIM TEST. Nothing in this claim needs the ARM target: it is zsysd,
# its clients and an ext4-backed settings.conf, all of which the simulator runs
# natively. A "reboot" is a second boot pointed at the SAME ZELTO_DATA_DIR.
#
# WHAT MAKES IT NON-VACUOUS — four runs, and two of them are controls:
#   Run 1 (write)   Settings writes sys.theme through the broker. zsysd reports
#                   the subscriber count, which is the LIVE cross-process
#                   fan-out; zero would mean the write went nowhere but the one
#                   process that made it.
#   Run 2 (reload)  A second boot on the same data dir with NO writer. Every
#                   surface must come up light off the disk.
#   Run 3 (control) An EMPTY data dir, same binaries. Every surface must come up
#                   DARK — the compiled-in default. Without this, run 2 passes
#                   just as well if light were the default all along.
#   Run 4 (control) The same empty data dir with ZELTO_THEME=light, which is the
#                   harness override. This one proves the env hook the shot
#                   catalogue depends on still beats an unset setting, and that
#                   run 3's darkness was the SETTING rather than a dead seam.
#
# THE ASSERTION IS PER-PROCESS, not per-screenshot. Every libzelto surface prints
# its own "zelto: type ... theme=<name>" line at startup (sdk/src/app.c), so the
# claim "every process followed" is checked against every process that booted
# rather than against one frame that happened to look right.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-theme.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One simulator boot. $1 = tag, $2 = data dir, $3 = ZELTO_SETTINGS_SET spec
# (empty for none), $4 = ZELTO_THEME override (empty for none).
run_boot() {
    local tag="$1" data="$2" setspec="${3:-}" themeenv="${4:-}"
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$data" "$dir/xdg"
    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY=10 \
    ZELTO_DATA_DIR="$data" SIM_RUNTIME_DIR="$dir/xdg" \
    SIM_APP="${setspec:+zelto-settings}" \
    ZELTO_SETTINGS_SET="$setspec" \
    ZELTO_THEME="$themeenv" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# How many surfaces in this boot reported each appearance. The startup line is
# one per libzelto process, so these two counts are the whole population.
count_theme() { grep -c "zelto: type .*theme=$2" "$1" 2>/dev/null || true; }

# Assert that every surface in a boot agrees, and that there were surfaces at
# all — a boot that started nothing would otherwise pass every "none disagreed"
# test in this file.
all_surfaces() {
    local log="$1" want="$2" other="$3" what="$4"
    local n_want n_other
    n_want="$(count_theme "$log" "$want")"
    n_other="$(count_theme "$log" "$other")"
    if [ "${n_want:-0}" -lt 3 ]; then
        zt_fail "$what: fewer than three surfaces reported the appearance at all, so 'they all agreed' is a claim about an empty set" \
            ">= 3 surfaces reporting theme=$want" "${n_want:-0} (see $log)"
        return
    fi
    if [ "${n_other:-0}" -ne 0 ]; then
        zt_fail "$what: some surfaces drew the other appearance — an OS where the bar and the launcher disagree about the palette is worse than one with no setting" \
            "0 surfaces at theme=$other" "${n_other} of $((n_want + n_other)) (see $log)"
    fi
}

DATA_SHARED="$TMP/shared-data"
DATA_FRESH="$TMP/fresh-data"

# --- Run 1: Settings writes it, live -----------------------------------------
LOG1="$(run_boot write "$DATA_SHARED" "sys.theme:1")"

if ! grep -q "\[zsysd\] settings_set sys.theme=1" "$LOG1"; then
    zt_fail "the Settings app never wrote sys.theme through the broker — nothing was persisted, so every run below would prove nothing" \
        "'[zsysd] settings_set sys.theme=1'" "absent (see $LOG1)"
    zt_done
fi

# THE LIVE CROSS-PROCESS HALF. libzelto subscribes in every process since P50, so
# a theme write has to reach more than the app that made it.
subs="$(sed -n 's/.*settings_set sys\.theme=1 -> \([0-9]*\) subscriber(s).*/\1/p' \
    "$LOG1" | tail -1)"
if [ -z "$subs" ]; then
    zt_fail "no subscriber count was reported for the sys.theme write" \
        "'settings_set sys.theme=1 -> N subscriber(s)'" "absent (see $LOG1)"
elif [ "$subs" -lt 1 ]; then
    zt_fail "the appearance write reached NO other process — the bar, the launcher and the homebar all draw from the palette, so a fan-out of 0 means only Settings changed colour" \
        ">= 1 subscriber" "$subs (see $LOG1)"
fi

# AND THAT THE SUBSCRIBERS ACTED ON IT, which the count above does NOT say.
# zsysd broadcasts to everyone subscribed to ANY key; a process that receives
# sys.theme and does nothing with it is counted just the same. This assertion
# exists because the first version of this test PASSED with sys.theme deleted
# from libzelto's fan-out list — it was checking the postman, not the letter.
#
# Every process that actually moved prints one "settings applied ... theme=" line
# (sdk/src/app.c). Two or more means at least one process that is NOT the writer
# repainted, live, with no reboot.
applied="$(grep -c 'zelto: settings applied .*theme=light' "$LOG1" || true)"
if [ "${applied:-0}" -lt 2 ]; then
    zt_fail "the appearance changed in at most the process that wrote it — the fan-out reached other processes but they did not apply it, which is a phone whose Settings screen goes light while the bar above it stays dark" \
        ">= 2 surfaces applying theme=light live" "${applied:-0} (see $LOG1)"
fi

# --- Run 2: a second boot on the same data dir (the reboot) ------------------
LOG2="$(run_boot reload "$DATA_SHARED")"
all_surfaces "$LOG2" light dark "after a reboot on the same data dir"

# --- Run 3: CONTROL — an empty data dir must come up dark --------------------
LOG3="$(run_boot fresh "$DATA_FRESH")"
all_surfaces "$LOG3" dark light "on a fresh data dir (the compiled-in default)"

# --- Run 4: CONTROL — the harness override still wins ------------------------
# The shot catalogue drives the appearance with ZELTO_THEME, and a shot that says
# light must not depend on what the seeded store happened to contain. This also
# proves run 3's darkness was the SETTING and not a seam that never fires.
LOG4="$(run_boot envwin "$DATA_FRESH" "" light)"
all_surfaces "$LOG4" light dark "with ZELTO_THEME=light over an unset setting"

zt_done
