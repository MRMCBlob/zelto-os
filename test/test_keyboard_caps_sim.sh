#!/usr/bin/env bash
# test_keyboard_caps_sim — the on-screen keyboard as a SURFACE, not as a relay.
#
# WHAT WAS NOT COVERED BEFORE THIS. P45's KBD harness types with ZELTO_KBD_TYPE,
# which calls z_im_commit_text() from inside zelto-keyboard. That is a real and
# valuable test — it drives the genuine text-input-v3 <-> input-method-v2 relay
# and proves a typed note survives a reboot — and it says nothing at all about
# the keyboard people actually touch. Untested by it: that a cap is laid out
# where the grid implies, that pressing that cap reaches THAT key rather than a
# neighbour, that the marks on shift and delete describe what those keys do, and
# how big the things you are aiming at really are. Twenty-five phases, and no
# test had ever measured a key.
#
# WHY THIS IS NOT THE COORDINATE TAPPING THAT ROTTED FIVE HARNESSES. Those
# harnesses HELD the numbers: a screenshot was measured, "(96, 1180) is the 'a'
# key" was pasted into a shell script, the layout moved, and the tap went on
# landing somewhere — silently, because a tap that hits nothing still returns.
# Here the test names a CHARACTER and the toolkit answers where that character
# is (z_probe_tap, sdk/src/app.c) by looking up the laid-out node that carries
# the handler. There is not one coordinate in this file. The test cannot drift
# when the layout moves, because the layout is what it asks.
#
# WHAT THE PROBE ASSERTS THAT A TAP CANNOT. z_probe_tap resolves the cap by
# handler, then runs the REAL hit walk at the centre of the frame it found and
# reports whether the walk came back with that same node (hit=same). That is the
# claim every coordinate tap was making implicitly and none of them ever checked:
# the key is where the layout says AND pressing there reaches it rather than
# something painted over it. It then dispatches whatever the HIT found, not what
# was resolved — so if the two ever disagree, the wrong character is committed
# and the text assertions below fail too, rather than passing on a technicality.
#
# THE FIRST RUN OF THIS FOUND A REAL BUG, which is now locked in run 1:
# the caps were built with Grow(), which divides the SLACK left after each child
# is measured — so every cap was as wide as the letter printed on it ('w' 69
# units, 'i' 49, in the same row) and latching shift re-measured the uppercase
# glyphs and MOVED EVERY KEY IN THE ROW under the finger. Fixed with a new
# Share() (zelto/ui.h): the weights divide the whole row and the content gets no
# vote. Assertion C is that fix's regression lock.
#
# NEGATIVE-TESTED five ways; each break was made, watched to fail with the right
# message, and restored:
#   Share() back to Grow()   -> C and D, with the numbers (b at 385,59 idle and
#                               392,57 after shift; three ragged rows)
#   shift never un-latches   -> B reads "a aB aBC aB"
#   backspace deletes two    -> B reads "a aB aBc a"
#   an invisible tappable scrim over the grid (the P44 visible-but-untappable
#                               bug class) -> A, B, C, D and BOTH run-2
#                               assertions; every cap resolves and every press
#                               reaches the scrim instead
#   the slide-settled gate removed -> NOTHING failed. Reported as what it is: a
#                               guard, not a covered behaviour. The first press
#                               is armed 400ms after the handshake and the show
#                               spring has already arrived, so this run never
#                               exercises it.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-caps.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# 1pt in screen units, as the toolkit computes it (Z_PT, zelto/ui.h). Kept as
# integer arithmetic in the same order the macro uses so this file and the C
# agree to the unit rather than to a rounded decimal.
PT_NUM=185
PT_DEN=100
pt() { echo $(( $1 * PT_NUM / PT_DEN )); }

# One simulator boot with the keyboard driving itself. $1 = tag,
# $2 = ZELTO_KBD_TAP spec. Notepad is launched so a field takes focus, which is
# what raises the input method and therefore the keyboard — the same handshake a
# user's tap on a text field produces. Nothing is injected.
run_boot() {
    local tag="$1" taps="$2"
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY=14 \
    ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
    SIM_APP=zelto-notepad \
    ZELTO_NOTEPAD_ECHO=1 ZELTO_KBD_CAPS=1 ZELTO_KBD_TAP="$taps" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# ---------------------------------------------------------------------------
# Run 1 — the letters, the modifiers, and the grid.
#
# a       a character cap commits its own character
# SHIFT   latches
# b       comes out UPPERCASE (shift applied) ...
# c       ... and the very next one does not (shift is ONE-SHOT)
# BKSP    removes exactly one character, not the field
# ---------------------------------------------------------------------------
LOG1="$(run_boot letters "a,SHIFT,b,c,BKSP")"

# The keyboard came up at all. Everything below is vacuous without this: with no
# input-method handshake there are no caps to resolve and every "absent" check
# passes trivially.
if ! grep -q '\[keyboard\] caps: ' "$LOG1"; then
    zt_fail "the keyboard never reached a settled, laid-out grid — no cap audit was printed, so nothing below this line means anything" \
        "'[keyboard] caps: N total ...'" "absent (see $LOG1)"
    zt_done
fi

# --- A. every named cap was found, and pressing its centre reached IT --------
taps_ok="$(grep -c "hit=same ran=yes" "$LOG1" || true)"
zt_expect_eq "5" "$taps_ok" \
    "not every key press resolved to its own cap (see $LOG1)"

# The absence checks below have their positive control right above: the 5
# hit=same lines come out of the SAME printf as a hit=DIFFERENT would, so a
# format change that silenced one would have silenced the other and $taps_ok
# would already have failed.
if grep -q "hit=DIFFERENT" "$LOG1"; then
    zt_fail "pressing the centre of a cap's own frame reached a DIFFERENT node — the key is not where the layout says it is, or something is painted over it" \
        "no hit=DIFFERENT" "$(grep -m1 'hit=DIFFERENT' "$LOG1")"
fi
if grep -q "NOT ON THIS LAYER" "$LOG1"; then
    zt_fail "a key named by the tap sequence is not on the letter layer at all" \
        "no 'NOT ON THIS LAYER'" "$(grep -m1 'NOT ON THIS LAYER' "$LOG1")"
fi

# --- B. what the caps actually TYPED ----------------------------------------
# The field echoes on every change, so the intermediate states are visible: it is
# not enough that the end of the string is right, because "shift is one-shot" and
# "backspace deletes one" are claims about the steps.
got="$(sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$LOG1" | tr '\n' ' ')"
zt_expect_eq "a aB aBc aB " "$got" \
    "the caps did not type what their marks claim: expected 'a' then shift+'b' as UPPERCASE, then 'c' still lowercase (one-shot), then backspace removing exactly one (see $LOG1)"

# --- C. latching shift must not MOVE the keys -------------------------------
# The regression lock for the Grow/Share bug. 'b' is resolved once by the audit
# (shift idle) and once by the press that follows SHIFT; the two frames must be
# the same frame. Under the old Grow() they were not: the cap re-measured itself
# around the uppercase glyph and slid 7 units sideways between the two.
audit_b="$(sed -n "s/.*cap 'b' x=\([0-9]*\) y=[0-9]* w=\([0-9]*\) .*/\1,\2/p" "$LOG1" | head -1)"
tap_b="$(sed -n "s/.*tap 'b' cap x=\([0-9]*\) y=[0-9]* w=\([0-9]*\) .*/\1,\2/p" "$LOG1" | head -1)"
if [ -z "$audit_b" ] || [ -z "$tap_b" ]; then
    zt_fail "could not read the 'b' cap's frame from both the audit and the press" \
        "two 'x,w' readings" "audit='$audit_b' tap='$tap_b' (see $LOG1)"
else
    zt_expect_eq "$audit_b" "$tap_b" \
        "latching shift MOVED the 'b' key: its frame differs between the idle audit and the press right after SHIFT, so every cap shifts under the finger the moment shift is pressed (see $LOG1)"
fi

# --- D. the touch targets, measured -----------------------------------------
# Every cap is at least as tall as the height the grid declares. This is the
# assertion that would catch ZELTO_KEY_H drifting away from what is drawn — the
# KBD_H species of bug, one layer down.
KEY_H="$(pt 42)"
short="$(awk -v min="$KEY_H" '
    /\[keyboard\] cap /{
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^h=/) { split($i, a, "="); if (a[2] + 0 < min) c++ }
        }
    }
    END { print c + 0 }' "$LOG1")"
zt_expect_eq "0" "$short" \
    "at least one cap is shorter than ZELTO_KEY_H ($KEY_H units) (see $LOG1)"

# Character caps in a row are all the same width. THE bug this test found: they
# were not, because each cap measured its own letter. Reported per row as
# "char widths MIN..MAX"; MIN must equal MAX in every row.
ragged="$(sed -n 's/.*char widths \([0-9]*\)\.\.\([0-9]*\).*/\1 \2/p' "$LOG1" |
    awk '$1 != $2 { c++ } END { print c + 0 }')"
rows="$(grep -c '\[keyboard\] row y=' "$LOG1" || true)"
zt_expect_eq "4" "$rows" \
    "the grid did not report four rows (see $LOG1)"
zt_expect_eq "0" "$ragged" \
    "a row's character caps are NOT all the same width — a cap is sized by the letter printed on it instead of by its share of the row (see $LOG1)"

# The dead gutter between caps. KEY_GAP is a real gap that belongs to no key, so
# some of the strip hits nothing; this pins how much. Not a pass/fail judgement
# on 44pt — see the note at the foot of this file — but a lint that catches the
# gutters silently widening.
worst_dead="$(sed -n 's/.*dead [0-9]*\.\?[0-9]* (\([0-9]*\)\.[0-9]*%).*/\1/p' "$LOG1" |
    sort -n | tail -1)"
if [ -z "$worst_dead" ]; then
    zt_fail "no row reported its dead-gutter share" "'dead N (P%)'" "absent (see $LOG1)"
elif [ "$worst_dead" -ge 20 ]; then
    zt_fail "more than a fifth of a keyboard row hits nothing at all" \
        "< 20% dead" "$worst_dead% (see $LOG1)"
fi

# ---------------------------------------------------------------------------
# Run 2 — the layer key, and the probe's own honesty about absence.
#
# SYM     switches to the numbers/symbols layer ...
# 5       ... which is the only layer '5' exists on, so finding it proves the
#         switch really happened (and it types a '5')
# SHIFT   does NOT exist on that layer, and the probe must SAY so rather than
#         quietly pressing nothing. '5' is this assertion's positive control:
#         both lines come from the same resolve path in the same run, so
#         "SHIFT absent" cannot be an artefact of the probe finding nothing.
# ---------------------------------------------------------------------------
LOG2="$(run_boot symbols "SYM,5,SHIFT")"

if ! grep -q '\[keyboard\] caps: ' "$LOG2"; then
    zt_fail "the keyboard never came up on the symbols run" \
        "'[keyboard] caps: N total ...'" "absent (see $LOG2)"
    zt_done
fi

if ! grep -q "tap '5' cap .* hit=same ran=yes" "$LOG2"; then
    zt_fail "'5' was not on the grid after pressing the layer key — the key marked 123 did not switch layers" \
        "tap '5' ... hit=same ran=yes" "$(grep -m1 "tap '5'" "$LOG2" || echo absent) (see $LOG2)"
fi
got2="$(sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$LOG2" | tail -1)"
zt_expect_eq "5" "$got2" \
    "pressing the cap marked '5' on the symbols layer did not type a 5 (see $LOG2)"

if ! grep -q "tap 'SHIFT': NOT ON THIS LAYER" "$LOG2"; then
    zt_fail "the symbols layer has no shift key, but the probe did not report it missing — a probe that cannot tell 'absent' from 'pressed' would let every assertion in this file pass against an empty grid" \
        "tap 'SHIFT': NOT ON THIS LAYER" "$(grep -m1 "tap 'SHIFT'" "$LOG2" || echo absent) (see $LOG2)"
fi

# ---------------------------------------------------------------------------
# MEASURED, and deliberately NOT asserted: the 44pt touch target.
#
# The caps come out 31.5-32.1pt wide by 41.6pt tall at 720x1440, and the widest
# thing a character cap can be is the row pitch: 720 units / 10 keys = 72 units
# = 38.9pt, with no gap at all. So a 44pt-wide key is not achievable on a
# 390pt-wide phone with a ten-key row, by anyone — iOS's own key caps are about
# 32pt wide for exactly this reason, and the guideline is not what its keyboard
# obeys. Vertically the ROW PITCH is 88 units = 47.6pt, which does clear 44pt;
# the cap paints 77 of that and the remaining 11 is the dead gutter measured
# above.
#
# So the honest finding is not "the caps fail the touch target" but "the caps
# are at the geometric maximum in the tight axis, and the gutters between them
# belong to no key". Closing those gutters needs the toolkit to separate what a
# node PAINTS from what it can be HIT in (a hit-slop), which does not exist
# today; the numbers are printed by the audit every run so the next phase argues
# from them instead of from a guideline.
# ---------------------------------------------------------------------------
zt_done
