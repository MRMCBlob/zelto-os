#!/usr/bin/env bash
# test_keyboard_hold_sim — what a finger does while it is still down.
#
# THREE BEHAVIOURS, ONE GESTURE. A tap handler hears about a press once, when it
# is over, which is enough for a button and not for a keyboard. Held, a letter
# offers its accents; held, delete repeats, accelerates and then starts taking
# whole words; and for as long as any key is held, a callout floats the letter
# above the finger so you can read what you hit under your own thumb. All three
# are the same seam — z_press_hook's DOWN / MOVE / UP — which is why they are one
# file: break the seam and all three assertions go at once, which is exactly what
# a reader of a failure should be told.
#
# WHY NOT THE TOOLKIT'S OnLongPress (P16). It fires once, on a node, and says
# nothing about the finger before or after. A repeating delete needs "still
# down, 1.4 seconds in"; an accent popup needs the release point, which is over a
# control that did not exist when the press began. Neither is expressible as a
# single callback on a node.
#
# THE HOLDS ARE COORDINATE-FREE, INCLUDING THE SLIDE. "e~700>é" presses the 'e'
# cap where the layout put it, waits 700ms, then resolves the popup cell that
# commits "é" — a control the HOLD created, looked up by its handler and data the
# same way any other cap is — moves there and releases. Nothing in this file, or
# in the harness it drives, is a number read off a screenshot.
#
# NEGATIVE-TESTED, each break made, watched to fail with the right message, and
# restored:
#   KBD_HOLD_S 0.5 -> 5.0 (no hold can complete in the harness's window)
#            -> runs 1 and 3 fail: no popup, no repeat, and run 1 types "he"
#               because the release becomes an ordinary tap on 'e'
#   the accent popup does not filter the caps under it (keep_accents_only made a
#   no-op) -> run 2 types "he": the popup is drawn over the grid but the LETTERS
#            are still candidates, so a release that was over nothing resolves to
#            the key it started on. Run 1 still passes — the slide lands on a real
#            accent cell either way — which is why run 2 exists.
#   releasing off the popup commits the nearest accent anyway
#            -> run 2 types "hé" where nothing should have been committed
#   the callout shows the geometric cap instead of the classifier's choice
#            (refresh_preview asks z_kbd_nearest instead of kbd_pick)
#            -> run 4: no "callout 'l' (geom 'k')" line at all; it reports 'k'
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-hold.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

run_boot() {
    local tag="$1" taps="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    env "$@" \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=22 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        SIM_APP=zelto-notepad \
        ZELTO_NOTEPAD_ECHO=1 ZELTO_NOTEPAD_NOAUTOCAP=1 ZELTO_KBD_TAP="$taps" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}
field_of() { sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$1" | tail -1; }
alive() {
    if ! grep -q "\[keyboard\] tap " "$1"; then
        zt_fail "the keyboard never resolved a cap, so nothing below this line means anything" \
            "'[keyboard] tap ...'" "absent (see $1)"
        zt_done
    fi
}

# ---------------------------------------------------------------------------
# Run 1 — LONG-PRESS ACCENTS: hold a letter, slide onto a mark, release.
#
# h        an ordinary tap, so the run has a positive control for "taps work"
# e~700>é  hold 'e' for 700ms (the hold threshold is 500ms), then slide onto the
#          popup cell for "é" and release there
# ---------------------------------------------------------------------------
LOG1="$(run_boot accent "h,e~700>é")"
alive "$LOG1"

if ! grep -q "accents open for 'e' (4)" "$LOG1"; then
    zt_fail "holding 'e' did not open its accent popup" \
        "accents open for 'e' (4)" "$(grep -m1 'accents' "$LOG1" || echo absent) (see $LOG1)"
fi
# The slide resolved a control the HOLD created. Without this line the run below
# could pass with the popup never laid out and the release landing on a letter.
if ! grep -q "slide to 'é' at" "$LOG1"; then
    zt_fail "the accent cell for 'é' was not on screen to slide onto — the popup opened but laid out nothing, or its cells carry different tap data than the table says" \
        "slide to 'é' at X,Y" "$(grep -m1 'slide to' "$LOG1" || echo absent) (see $LOG1)"
fi
zt_expect_eq "hé" "$(field_of "$LOG1")" \
    "the accent was not committed: expected the tapped 'h' then 'é' from the popup, and NOT a plain 'e' (a hold must not also type the letter it was held on) (see $LOG1)"

# ---------------------------------------------------------------------------
# Run 1b — the UPPER-CASE accent (P48). Shift, then the same held slide onto 'é',
# must commit 'É'. The slide target stays ">é" because the popup cell's IDENTITY
# is its stored lower-case pointer (the case is applied at commit, not in the
# table); asking for ">É" would be asking for a cell that does not exist. This is
# the fix for P47's known gap — the letters that most need the capital are the
# ones a name starts with, which a dictionary cannot help with.
# ---------------------------------------------------------------------------
LOG1U="$(run_boot upper "SHIFT,e~700>é")"
alive "$LOG1U"
if ! grep -q "commit 'É' (accent of 'e')" "$LOG1U"; then
    zt_fail "holding 'e' with shift held did not commit the UPPER-CASE 'É' — accent_upper is not applying the case at commit" \
        "commit 'É' (accent of 'e')" "$(grep -m1 "accent of 'e'" "$LOG1U" || echo absent) (see $LOG1U)"
fi
zt_expect_eq "É" "$(field_of "$LOG1U")" \
    "a shifted accent must commit its capital: À/Ö are exactly the marks a name starts with, which is why lower-case-only read as a bug (see $LOG1U)"

# ---------------------------------------------------------------------------
# Run 2 — the same hold, RELEASED WITHOUT SLIDING.
#
# The popup opens above the key, so a finger that lifts where it landed is not
# over any of it. That commits NOTHING, deliberately: a hold you did not follow
# through has to be cancellable, and the alternatives (commit the nearest accent,
# or fall back to the base letter) both mean an accidental pause silently changes
# what you typed.
#
# This is also run 1's discriminator. Same key, same duration, one difference —
# whether the finger moved — and a different result.
# ---------------------------------------------------------------------------
LOG2="$(run_boot cancel "h,e~700")"
alive "$LOG2"

if ! grep -q "accents open for 'e'" "$LOG2"; then
    zt_fail "the popup did not open on the run that is supposed to cancel it, so 'nothing was committed' proves nothing" \
        "accents open for 'e'" "absent (see $LOG2)"
fi
zt_expect_eq "h" "$(field_of "$LOG2")" \
    "releasing a hold away from the accent popup committed something: it must commit neither an accent nor the base letter (see $LOG2)"

# ---------------------------------------------------------------------------
# Run 3 — BACKSPACE REPEAT, its acceleration, and the switch to whole words.
#
# The field is SEEDED rather than typed: what is under test is how much a held
# delete removes and in what units, and typing thirty characters a cap at a time
# would spend twenty seconds proving what two other tests already prove.
# ---------------------------------------------------------------------------
SEED="alpha beta gamma delta epsilon"
LOG3="$(run_boot bksp "BKSP~2800" ZELTO_NOTEPAD_SEED="$SEED")"
alive "$LOG3"

if ! grep -q "backspace repeat begins" "$LOG3"; then
    zt_fail "holding delete did not start repeating" \
        "backspace repeat begins" "$(grep -m1 backspace "$LOG3" || echo absent) (see $LOG3)"
fi
# It ACCELERATED. Read off the logged gaps rather than assumed from a count: the
# cadence is a function of how long the key has been held, so a test counting
# repetitions would measure something else on a loaded machine.
first_gap="$(sed -n 's/.*mode=char gap=\([0-9]*\)ms.*/\1/p' "$LOG3" | head -1)"
last_gap="$(sed -n 's/.*mode=char gap=\([0-9]*\)ms.*/\1/p' "$LOG3" | tail -1)"
if [ -z "$first_gap" ] || [ -z "$last_gap" ]; then
    zt_fail "no repeat reported its cadence" "'mode=char gap=Nms'" "absent (see $LOG3)"
elif [ "$last_gap" -ge "$first_gap" ]; then
    zt_fail "the repeating delete never accelerated — it deletes at the same rate after two seconds as after half of one, which is the behaviour that makes a held delete useless on a long field" \
        "a smaller gap later in the hold" "first ${first_gap}ms, last ${last_gap}ms (see $LOG3)"
fi
# And it changed UNITS. A word delete is one edit (delete_surrounding_text of N),
# not N backspaces, so the count it reports is the assertion that it computed a
# word boundary rather than repeating faster.
if ! grep -q "mode=word deleted [1-9]" "$LOG3"; then
    zt_fail "the held delete never switched to whole words" \
        "'mode=word deleted N' with N > 0" "$(grep -m1 'mode=word' "$LOG3" || echo absent) (see $LOG3)"
fi
# WHAT IS LEFT, asserted as a SHAPE rather than as a string. The cadence is a
# function of how long the key has been held, so how many repeats fit into 2.8
# seconds depends on how busy the machine is — an exact expected string here
# passed on a quiet run and failed on a loaded one by exactly one word. What does
# NOT vary is the shape: everything removed came off the END (the result is a
# prefix of the seed), the accelerating phase alone got it under half, and the
# word phase left the cursor on a word boundary rather than in the middle of one.
end="$(field_of "$LOG3")"
case "$SEED" in
    "$end"*) : ;;
    *) zt_fail "the held delete changed text it did not delete: what is left is not a prefix of the seed" \
           "a prefix of '$SEED'" "'$end' (see $LOG3)" ;;
esac
if [ "${#end}" -ge 13 ]; then
    zt_fail "2.8 seconds of held delete removed less than the accelerating phase alone should have" \
        "under 13 of ${#SEED} characters left" "${#end} left ('$end') (see $LOG3)"
fi
case "$end" in
    ""|*" ") : ;;
    *) zt_fail "the held delete stopped in the MIDDLE of a word — the word phase is deleting a fixed count rather than back to a boundary" \
           "empty, or ending at a space" "'$end' (see $LOG3)" ;;
esac

# ---------------------------------------------------------------------------
# Run 4 — the KEY PREVIEW CALLOUT shows what the CLASSIFIER chose.
#
# This is the one design question the callout raises. Under the finger is 'k';
# what will be committed after "he" is 'l'. Showing 'k' would be the
# honest-looking option and would be a lie — it would misinform the user at the
# single moment they could still slide and fix it. It shows the choice, which is
# also what makes the adaptive targets visible to a person at all. iOS does the
# same.
#
# 'l' has no accents, so the hold opens nothing and the release is swallowed:
# this run asserts the callout and nothing else, and the empty field is part of
# the assertion.
# ---------------------------------------------------------------------------
LOG4="$(run_boot callout "h,e,l@-45~700")"
alive "$LOG4"

if ! grep -q "callout 'l' (geom 'k')" "$LOG4"; then
    zt_fail "the preview callout showed the cap under the finger instead of the letter that will be typed — at the one moment the correction is still visible, the keyboard told the user the wrong thing" \
        "callout 'l' (geom 'k')" "$(grep -m1 callout "$LOG4" || echo absent) (see $LOG4)"
fi
# The positive control for that: a press with nothing to correct reports the same
# letter twice, out of the same printf. If callouts stopped being logged at all,
# this would have gone too.
if ! grep -q "callout 'h' (geom 'h')" "$LOG4"; then
    zt_fail "no callout was reported for an ordinary, accurate press" \
        "callout 'h' (geom 'h')" "$(grep -m1 callout "$LOG4" || echo absent) (see $LOG4)"
fi

zt_done
