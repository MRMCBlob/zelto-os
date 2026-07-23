#!/usr/bin/env bash
# test_text_size_overflow_sim — the P50 audit, kept.
#
# WHAT THIS IS. Dynamic Type's real cost is not the seam that resizes the type,
# it is that EVERY FIXED HEIGHT HOLDING TEXT IS NOW WRONG. A box whose number came
# off a spec sheet — a 44pt row, a 46-unit button, a 30-unit card header — does
# not grow when the line inside it does, and there is no clipping in this
# renderer: the glyphs paint straight through into whatever is next to them. So
# the phase's actual work was a LIST, and this file is how the list stays at zero.
#
# WHAT IT MEASURES, on both axes, and neither by eye nor by a pixel delta.
# ZELTO_PROBE_TAPS reports, for every Text node on the settled frame, the box the
# layout gave it against the size the shaper says it needs (sdk/src/app.c):
#
#   overflowing  WIDER than its box. A string somebody ELSE wrote arriving in a
#                column — an app name out of a manifest, a share target, a
#                permission sentence.
#   clipped      TALLER than its box. A box somebody else DECLARED, holding type
#                the user just made bigger. This is the P50 half, and it was
#                already non-zero at the default size before this phase: the App
#                Switcher wanted 35 units of Subhead in a 30-unit strip and the
#                SDK's Rows sample wanted 40 in 36, both shipped, both invisible.
#
# WHAT THE AUDIT FOUND, which is what these boots are: 'andemu Demo' needed 206
# units in a 158-unit home cell; the consent alert's two buttons wanted 61 in 46
# (and the card itself stood 44 units taller than the rect the compositor was
# blurring behind it); the switcher's card titles wanted 49 in 30; the share
# sheet's target names 117 in 96; and Fetch's permission line 910 in 656 — that
# last one at the DEFAULT size too, so it had been overflowing since P43.
#
# THE POSITIVE CONTROL IS ON EVERY ASSERTION. "0 overflowing" out of 0 text nodes
# is an empty screen, not a healthy one — a crashed sim, a surface that never
# came up and a perfect layout all report it — so each check reads the scanned
# count from the same log line and fails if the surface was not really there.
#
# NEGATIVE-TESTED: reverting the consent button height to its 46-unit literal
# makes `the consent alert` fail with the two button labels named; reverting the
# home label to a bare Text makes `the home grid` fail naming 'andemu Demo'.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-tsoverflow.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Every boot is at the TOP of the range. There is no point auditing the middle:
# the ladder is monotonic, so the largest size is the worst case for every box in
# the OS, and a surface that survives it survives all seven.
TS_LARGEST=6

run_boot() {   # <tag> <probe-app> <delay> [env...]
    local tag="$1" who="$2" delay="$3"; shift 3
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    printf 'sys.brightness\t5\nsys.text_size\t%s\n' "$TS_LARGEST" \
        > "$dir/data/settings.conf"
    env HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY="$delay" \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP="$who" "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# <name> <log> <min texts> — the surface drew text, and none of it needs more
# room than it was given, on either axis.
check() {
    local name="$1" log="$2" min="$3"
    local scanned over tall
    scanned="$(sed -n 's/.*text \([0-9]*\) scanned .*/\1/p' "$log" | head -1)"
    over="$(sed -n 's/.*text [0-9]* scanned \([0-9]*\) overflowing.*/\1/p' "$log" | head -1)"
    tall="$(sed -n 's/.*off-surface \([0-9]*\) clipped.*/\1/p' "$log" | head -1)"
    if [ -z "$scanned" ]; then
        zt_fail "$name never reported a probe summary at the largest text size — the surface did not come up, so nothing below is evidence" \
            "'probe taps: ... text N scanned ...'" "absent (see $log)"
        return
    fi
    if [ "$scanned" -lt "$min" ]; then
        zt_fail "$name drew only $scanned text nodes at the largest text size, fewer than the $min it must have — it is not showing the content this test boots it for, so a clean overflow count means nothing" \
            ">= $min text nodes" "$scanned (see $log)"
        return
    fi
    zt_expect_eq "0" "$over" \
        "$name has text WIDER than its box at the largest text size: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
    zt_expect_eq "0" "$tall" \
        "$name has text TALLER than its box at the largest text size — a fixed height holding a line that outgrew it, painting through into its neighbours: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
}

# --- the home grid -----------------------------------------------------------
# An app NAME comes out of a manifest into a 158-unit cell.
check "the home grid" "$(run_boot home launcher_body 6)" 20

# --- the consent alert -------------------------------------------------------
# The dialog where being able to read the two answers is the entire point.
check "the consent alert" \
    "$(run_boot consent Permission 7 SIM_CONSENT="os.zelto.pinger notifications")" 4

# --- the App Switcher --------------------------------------------------------
check "the App Switcher" \
    "$(run_boot switcher recents_body 9 SIM_APP=zelto-notes \
        SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1)" 5

# --- the share sheet ---------------------------------------------------------
check "the share sheet" \
    "$(run_boot share Chooser 10 SIM_APP=zelto-notes \
        SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
        ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens")" 5

# --- the grouped inset list --------------------------------------------------
# The densest fixed-height rows in the OS: Settings ▸ Lock Screen is toggles,
# steppers and an action row, all built on Z_ROW_H.
check "the Settings list" \
    "$(run_boot settings os.zelto.settings 8 SIM_APP=zelto-settings \
        ZELTO_SETTINGS_SCREEN=lock)" 12

# --- the new Accessibility screen --------------------------------------------
# The screen that sets the size has to survive the size it sets, which is not a
# joke: its slider row holds two glyphs at fixed steps and a preview at Body.
check "the Accessibility screen" \
    "$(run_boot accessibility os.zelto.settings 8 SIM_APP=zelto-settings \
        ZELTO_SETTINGS_SCREEN=accessibility)" 8

# --- an app whose prose is not its own ---------------------------------------
check "Fetch" "$(run_boot fetch os.zelto.fetch 8 SIM_APP=zelto-fetch)" 5

# --- the SDK's own List sample -----------------------------------------------
# Not a system surface, and in for a reason: it is what an APP DEVELOPER copies.
# A List virtualises on its row height, so a row too short for its own text does
# not merely clip — it puts the wrong rows on screen.
check "the Rows sample" "$(run_boot hello body 8 SIM_APP=zelto-hello)" 20

zt_done
