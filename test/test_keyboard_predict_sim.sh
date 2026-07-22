#!/usr/bin/env bash
# test_keyboard_predict_sim — type "hello" while missing two of the keys.
#
# THE CLAIM. The keyboard's touch targets are not its key caps. A press 45 units
# left of the 'l' cap's centre lands inside the PAINTED 'k' cap — 24 units from
# k's own centre, well past the gutter — and after "he" it must still be an 'l',
# because "el" is a common English bigram and "ek" is not. That is the whole of
# Ken Kocienda's contribution to the original iPhone keyboard: the art never
# moves and the invisible targets do, in proportion to how likely each letter is.
# It is the single mechanism that makes a 32pt cap typeable by a thumb, and it is
# why the 44pt argument P46 correctly closed was the wrong argument to be having.
#
# THE MISS HAS TO BE REAL, AND PROVED IN THE SAME RUN. "It typed hello" is
# worthless on its own: the easy way to write this test by accident is with an
# offset that never leaves the key, and it passes. So every press logs what the
# PLAIN GEOMETRIC hit walk finds at that exact point first (z_probe_at, taken
# before the press and deliberately not routed through the classifier). The
# assertions below require that witness to say 'k' and 'p' — the NEIGHBOURS —
# and only then that the field contains "hello".
#
# 45 UNITS, NOT 35. The obvious offset is half a cap plus a little, and it does
# not work: at 720x1440 the caps are 58 wide on a 69 pitch, so ±35 from a centre
# lands in the 11-unit DEAD GUTTER between two caps, where the geometric walk
# finds nothing at all. That proves the classifier fills the gutter (run 3 does
# exactly that, on purpose) but it cannot prove the stronger claim, which is that
# a confident prefix reaches into the neighbour's own painted cap.
#
# THE NEGATIVE CONTROL IS THE POINT. Run 2 is the same boot, the same sequence
# and the same offsets with ZELTO_KBD_PREDICT=0, and it must type "heklp" — the
# letters that are actually under those two points. The wrong word is asserted
# EXPLICITLY rather than as "not hello", because "not hello" also passes when the
# keyboard types nothing, crashes, or commits "hellp" for some third reason. One
# point, two answers, one difference: that is the only thing that distinguishes a
# working language model from a lucky rectangle.
#
# NEGATIVE-TESTED, each break made, watched to fail with the right message, and
# restored:
#   the resolver never installed  -> the abort at the top of run 1: no press is
#                        logged at all, because the SDK's rectangle walk dispatches
#                        the tap and the keyboard never sees the point
#   Z_KBD_ODDS 8 -> 1 (the model may not move anything) -> run 1 types "heklp"
#                        and both corrected presses report why=geometry
#   the centre-zone rule deleted  -> run 3's 'q' STILL COMES OUT 'q' — and the
#                        assertion fails anyway, because it requires why=centre.
#                        That is deliberate and worth reading twice: at the
#                        shipped constants the rule is redundant (test_kbd_predict
#                        proves the inequality that makes it so), so a test that
#                        only checked the letter would pass with the guarantee
#                        gone. It checks that the RULE decided.
#   the password purpose ignored  -> run 4 types "hello" and logs lm=bigram
#   the surrounding-text relay dropped (im_surrounding_text back to a stub)
#                     -> run 1 types "heklp" with prefix='' on every press
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-predict.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One simulator boot with the keyboard pressing its own caps. $1 = tag,
# $2 = ZELTO_KBD_TAP spec; anything after that is extra environment. Notepad is
# launched so a field takes focus, which is what raises the input method and
# therefore the keyboard — the same handshake a user's tap on a field produces.
run_boot() {
    local tag="$1" taps="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    env "$@" \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=16 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        SIM_APP=zelto-notepad \
        ZELTO_NOTEPAD_ECHO=1 ZELTO_KBD_TAP="$taps" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# The note field's contents after the last change it reported.
field_of() { sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$1" | tail -1; }

# The keyboard reached a laid-out grid at all. Without this every assertion below
# is vacuous: with no input-method handshake there are no caps to resolve, no
# presses to log, and every "must not contain" check passes against an empty file.
alive() {
    if ! grep -q "\[keyboard\] press " "$1"; then
        zt_fail "the keyboard never pressed a cap — no press was logged, so nothing below this line means anything" \
            "'[keyboard] press ...'" "absent (see $1)"
        zt_done
    fi
}

MISSES="h,e,l@-45,l,o@45"

# ---------------------------------------------------------------------------
# Run 1 — the model on. Two presses land on the wrong key and the right letters
# come out.
# ---------------------------------------------------------------------------
LOG1="$(run_boot on "$MISSES")"
alive "$LOG1"

# --- A. the misses were real -------------------------------------------------
# The geometric witness, taken at the press point before anything clever ran.
# This is the assertion that makes the rest non-vacuous.
if ! grep -q "tap 'l' .* off -45,0 geom='k'" "$LOG1"; then
    zt_fail "the deliberate miss on 'l' did not land on the 'k' cap — the offset is not big enough to leave the key (or the layout moved), so 'it typed hello' would only prove the press was inside 'l' all along" \
        "tap 'l' ... off -45,0 geom='k'" "$(grep -m1 "tap 'l' .* off -45" "$LOG1" || echo absent) (see $LOG1)"
fi
if ! grep -q "tap 'o' .* off 45,0 geom='p'" "$LOG1"; then
    zt_fail "the deliberate miss on 'o' did not land on the 'p' cap" \
        "tap 'o' ... off 45,0 geom='p'" "$(grep -m1 "tap 'o' .* off 45" "$LOG1" || echo absent) (see $LOG1)"
fi

# --- B. and the right word came out ------------------------------------------
zt_expect_eq "hello" "$(field_of "$LOG1")" \
    "the keyboard did not correct the two deliberate misses (see $LOG1)"

# --- C. and it was the MODEL that did it, not a wider rectangle ---------------
# The classifier says which of the two factors decided each press.
# why=model-over-'k' means: the geometry said 'k', the language model said 'l',
# and 'l' won. A pass with why=centre or why=geometry here would mean the misses
# were not misses.
if ! grep -q "chose='l' why=model-over-'k'" "$LOG1"; then
    zt_fail "the 'l' came out of geometry rather than out of the language model" \
        "chose='l' why=model-over-'k'" "$(grep -m1 "geom='k'" "$LOG1" || echo absent) (see $LOG1)"
fi
if ! grep -q "chose='o' why=model-over-'p'" "$LOG1"; then
    zt_fail "the 'o' came out of geometry rather than out of the language model" \
        "chose='o' why=model-over-'p'" "$(grep -m1 "geom='p'" "$LOG1" || echo absent) (see $LOG1)"
fi
# Which model answered. The seam in predict.h allows a dictionary to replace the
# bigram table without the hit path changing; this line is how a reader of a
# failing log knows which one was in the image.
if ! grep -q "lm=bigram" "$LOG1"; then
    zt_fail "no press reported which language model resolved it" \
        "lm=bigram" "$(grep -m1 'lm=' "$LOG1" || echo absent) (see $LOG1)"
fi
# The context came from the FIELD, not from an echo of our own keystrokes. If the
# surrounding-text relay were dropped every prefix would be empty and the model
# would be guessing from nothing — which still types four of the five letters, so
# nothing else in this file would notice.
if ! grep -q "prefix='hell'" "$LOG1"; then
    zt_fail "the keyboard never saw the field's contents — text-input-v3 surrounding text is not reaching it, so the model is predicting from an empty prefix" \
        "prefix='hell'" "$(grep -m1 "prefix=" "$LOG1" || echo absent) (see $LOG1)"
fi

# ---------------------------------------------------------------------------
# Run 2 — THE NEGATIVE CONTROL. Same sequence, same offsets, model off.
# ---------------------------------------------------------------------------
LOG2="$(run_boot off "$MISSES" ZELTO_KBD_PREDICT=0)"
alive "$LOG2"

zt_expect_eq "heklp" "$(field_of "$LOG2")" \
    "with the language model off, the two deliberate misses must produce the letters that are actually under them — 'k' for the press inside the 'k' cap and 'p' for the press inside the 'p' cap. Anything else means the offsets are not landing where run 1 claims they do (see $LOG2)"

# The same two presses, same points, resolved the other way. Stated separately
# from the string above because it is a different claim: not "a different word
# came out" but "this press was decided by geometry".
if ! grep -q "geom='k' chose='k' why=geometry" "$LOG2"; then
    zt_fail "with the model off the press inside 'k' did not resolve to 'k' by geometry" \
        "geom='k' chose='k' why=geometry" "$(grep -m1 "geom='k'" "$LOG2" || echo absent) (see $LOG2)"
fi
if grep -q "lm=bigram" "$LOG2"; then
    zt_fail "ZELTO_KBD_PREDICT=0 did not turn the language model off" \
        "lm=off on every press" "$(grep -m1 'lm=bigram' "$LOG2") (see $LOG2)"
fi

# ---------------------------------------------------------------------------
# Run 3 — the two things the classifier must NEVER do, and the gutter it fills.
#
# h,e,l    build a prefix in which 'q' is about as unlikely as English gets
# q        pressed DEAD CENTRE: it must be a 'q'. If a confident prefix could
#          override the middle of a cap, this keyboard could not type a password,
#          a name, or any word the model has never seen.
# a@35     pressed in the DEAD GUTTER between two caps, where the geometric walk
#          finds nothing at all — the ~15% of every row that P46 measured and
#          could not fix. It must commit SOMETHING.
# ---------------------------------------------------------------------------
LOG3="$(run_boot centre "h,e,l,q,a@35")"
alive "$LOG3"

if ! grep -q "geom='q' chose='q' why=centre" "$LOG3"; then
    zt_fail "a press in the dead centre of the 'q' cap after \"hel\" did not give a 'q' — the language model overrode a deliberate, accurate press, which makes every string the model has not seen untypeable" \
        "geom='q' chose='q' why=centre" "$(grep -m1 "geom='q'" "$LOG3" || echo absent) (see $LOG3)"
fi

# The gutter. Its positive control is the line right above: both come out of the
# same printf in the same run, so a format change that silenced one would have
# silenced the other.
# The PRESS line, not the tap line: both name the geometric witness, and only the
# press line says what the classifier did with it.
gutter_line="$(grep -m1 "press .*geom='-'" "$LOG3" || true)"
if [ -z "$gutter_line" ]; then
    zt_fail "the press aimed at the gutter between two caps was not in the gutter — the geometric walk found a key there, so this run says nothing about dead space" \
        "a press logging geom='-'" "none (see $LOG3)"
elif ! echo "$gutter_line" | grep -q "chose='[a-z]'"; then
    zt_fail "a press in the dead gutter still hit nothing: the geometric walk found no key AND the classifier chose none, so the ~15% of each row that belongs to no cap is still dead" \
        "chose='<some key>'" "$gutter_line (see $LOG3)"
fi
zt_expect_eq "5" "$(sed -n "s/.*\[notepad\] field='.*' len=\([0-9]*\).*/\1/p" "$LOG3" | tail -1)" \
    "five presses did not produce five characters (see $LOG3)"

# ---------------------------------------------------------------------------
# Run 4 — a PASSWORD field turns the whole thing off.
#
# Nothing in this run sets ZELTO_KBD_PREDICT. The keyboard learns that the field
# is secure the only way it can: the app declares content purpose PASSWORD on
# text-input-v3, the compositor relays it to input-method-v2, and the keyboard
# reads it. Every link in that chain has existed since P21 except the two ends,
# which dropped the event on the floor.
#
# iOS disables prediction here for the reason this test asserts: a password is
# precisely the string the language model gets wrong, and a keyboard that
# "corrected" it would be unfixable from the app's side.
# ---------------------------------------------------------------------------
LOG4="$(run_boot secure "$MISSES" ZELTO_NOTEPAD_SECURE=1)"
alive "$LOG4"

if grep -q "lm=bigram" "$LOG4"; then
    zt_fail "the language model stayed on for a PASSWORD field — the content purpose is not reaching the keyboard, or it is not being honoured" \
        "lm=off on every press" "$(grep -m1 'lm=bigram' "$LOG4") (see $LOG4)"
fi
zt_expect_eq "heklp" "$(field_of "$LOG4")" \
    "a secure field must receive exactly the keys that were pressed, uncorrected (see $LOG4)"

zt_done
