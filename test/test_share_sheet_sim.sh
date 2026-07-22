#!/usr/bin/env bash
# test_share_sheet_sim — a sheet sized by its content, holding content it did not
# write.
#
# WHAT WAS SHIPPING. system/chooser drew the shared payload — a URL, or whatever
# text an app hands the share sheet — as a bare Text in a row whose height was
# summed into a hand-written sheet_h. A Text measures to ONE line however long
# that line is, so a shared URL ran off the right edge of the sheet and off the
# screen: no wrap, no truncation, no warning. P45 found it, wrote down correctly
# that WrapText was the wrong tool (a share preview wants one line, and a sheet
# whose height is a sum of its rows cannot absorb a paragraph), and left the
# overflow in place because the toolkit had no truncation primitive.
#
# It has one now. EllipsizeText measures with the real shaper and cuts on a
# character boundary, so the node's own width is bounded by its column — which is
# the actual guarantee, and the one this test checks.
#
# AND THE SHEET WAS NEVER ITS OWN HEIGHT. sheet_h was a hand-written sum with the
# type metrics guessed: a target's name budgeted 14 units where a Caption2 line
# stands 26, the preview row budgeted the height of its ICON (52) where its two
# text lines stand 77. That did not overflow anything — the stack simply measured
# taller than the Frame asked for and stood at its own height, so the sheet LOOKED
# right — but everything computed FROM sheet_h was wrong by 40 units: the backdrop
# blur covered a shorter rectangle than the sheet, leaving an unblurred strip
# under the top of the material, and the entrance rise `(1 - e) * sheet_h` was 40
# short of the sheet's height, so it did not start off the bottom edge, it started
# with its top 40 units on screen and popped. On top of that, a Frame's fixed
# height is an INNER height when the node carries padding (measure() does
# `n->h = fixed_h + 2 * padding`), so the same 40 was paid twice.
#
# HOW IT IS MEASURED. ZELTO_PROBE_TAPS (sdk/src/app.c) dumps the laid-out frames
# on the first SETTLED build, plus a count of Text nodes that need more room
# than the box they were given (a Text's frame is clamped to its parent, but the
# renderer draws from the origin and does not clip, so the frames alone cannot
# show this — measure() records the shaped width in text_w for exactly this).
# No coordinate in this file is a number anyone chose: every one is read back out
# of the boot log and checked against another number from the same log.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-share-sheet.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# A payload no layout can absorb: 100+ characters with no spaces to break at,
# which is what a real shared link looks like.
LONG="https://example.org/a/very/long/path/that/nobody/would/type/by/hand?utm_source=zelto&utm_campaign=overflow"
SHORT="hello"

run_boot() {   # <tag> <payload>
    local tag="$1" payload="$2"
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY=9 \
    ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
    SIM_CHOOSER="os.zelto.notes os.zelto.cards os.zelto.share" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="$payload" \
    ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP=Chooser \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# --- Run 1: the payload nothing bounds -------------------------------------
LOG1="$(run_boot long "$LONG")"

if ! grep -q '\[chooser\] sheet_h=' "$LOG1"; then
    zt_fail "the share sheet never came up — no sheet_h was reported, so every check below would pass against an empty screen" \
        "'[chooser] sheet_h=...'" "absent (see $LOG1)"
    zt_done
fi

sheet_h="$(sed -n 's/.*\[chooser\] sheet_h=\([0-9]*\)\..*/\1/p' "$LOG1" | head -1)"
screen_h="$(sed -n 's/.*probe taps: .*([0-9]*x\([0-9]*\)).*/\1/p' "$LOG1" | head -1)"

# --- A. no text needs more room than it was given ---------------------------
# The scanned count is the positive control: 0 off-surface out of 0 text nodes
# would mean the sheet drew no text at all, which is a different failure wearing
# the same number.
scanned="$(sed -n 's/.*text \([0-9]*\) scanned .*/\1/p' "$LOG1" | head -1)"
over="$(sed -n 's/.*text [0-9]* scanned \([0-9]*\) off-surface.*/\1/p' "$LOG1" | head -1)"
if [ -z "$scanned" ] || [ "$scanned" -lt 1 ]; then
    zt_fail "the probe scanned no text at all, so 'nothing overflowed' means nothing" \
        ">= 1 text node" "${scanned:-none} (see $LOG1)"
else
    zt_expect_eq "0" "$over" \
        "a Text needs more room than the box it was given, with a 105-character payload in the sheet — this is the overflow that shipped for two phases (see $LOG1)"
fi

# --- B. the sheet IS its declared height ------------------------------------
# The backdrop is the tappable that fills everything above the sheet, so its
# height is where the sheet starts. Reserve plus backdrop must be the screen: if
# they are not, then sheet_h is not the sheet's height and the blur rectangle and
# the entrance rise computed from it are both wrong.
back_h="$(sed -n "s/.*probe tap '-' x=0 y=0 w=[0-9]* h=\([0-9]*\).*/\1/p" "$LOG1" | head -1)"
if [ -z "$back_h" ] || [ -z "$sheet_h" ] || [ -z "$screen_h" ]; then
    zt_fail "could not read the sheet's geometry back out of the boot log" \
        "backdrop height, sheet_h and the screen height" \
        "back='$back_h' sheet='$sheet_h' screen='$screen_h' (see $LOG1)"
    zt_done
fi
zt_expect_eq "$screen_h" "$((back_h + sheet_h))" \
    "the sheet does not stand at the height it declares: it starts at y=$back_h on a ${screen_h}-tall screen, so it is $((screen_h - back_h)) tall while sheet_h says $sheet_h — the backdrop blur and the entrance rise are both computed from sheet_h (see $LOG1)"

# --- C. the last row is inside the sheet ------------------------------------
cancel_y="$(sed -n "s/.*probe tap 'Cancel' x=[0-9]* y=\([0-9]*\) .*/\1/p" "$LOG1" | head -1)"
cancel_h="$(sed -n "s/.*probe tap 'Cancel' x=[0-9]* y=[0-9]* w=[0-9]* h=\([0-9]*\).*/\1/p" "$LOG1" | head -1)"
if [ -z "$cancel_y" ]; then
    zt_fail "the Cancel row is not on the sheet at all" "a laid-out frame" \
        "absent (see $LOG1)"
else
    if [ "$cancel_y" -lt "$back_h" ]; then
        zt_fail "the Cancel row sits ABOVE the sheet it belongs to" \
            ">= $back_h" "$cancel_y (see $LOG1)"
    fi
    if [ "$((cancel_y + cancel_h))" -gt "$screen_h" ]; then
        zt_fail "the Cancel row runs off the bottom of the screen" \
            "<= $screen_h" "$((cancel_y + cancel_h)) (see $LOG1)"
    fi
fi

# --- Run 2: the control — a payload that fits -------------------------------
# The sheet's geometry must not depend on what it was handed. Under the old code
# the long payload made the preview row 1600 units wide inside a 720 sheet; under
# the new one the row is cut to its column, so both runs must lay out identically.
# This is what makes run 1's numbers evidence rather than one lucky string.
LOG2="$(run_boot short "$SHORT")"
sheet_h2="$(sed -n 's/.*\[chooser\] sheet_h=\([0-9]*\)\..*/\1/p' "$LOG2" | head -1)"
cancel_y2="$(sed -n "s/.*probe tap 'Cancel' x=[0-9]* y=\([0-9]*\) .*/\1/p" "$LOG2" | head -1)"
zt_expect_eq "$sheet_h" "$sheet_h2" \
    "the sheet is a different height for a 5-character payload than for a 105-character one — its size depends on content it does not control (see $LOG2)"
zt_expect_eq "$cancel_y" "$cancel_y2" \
    "the Cancel row moves depending on how long the shared string is (see $LOG2)"

zt_done
