#!/usr/bin/env bash
# test_keyboard_text_rules_sim — caps lock, the double-space period, and
# auto-capitalisation, driven through the KEY CAPS.
#
# WHY THESE THREE ARE ONE FILE. They look like three unrelated conveniences and
# they are one idea: the keyboard reasoning about the TEXT rather than about its
# own buttons. Caps lock is the exception that proves it — it is the only one of
# the three that is a state machine on a key — and it is here because it shares
# the state everything else reads: what case the next letter comes out in.
#
# WHERE THE STATE LIVES, which was the open question. The keyboard cannot see the
# field: it sends commit_string and never hears back. So a rule like "two spaces
# become a full stop" needs to know what is already there, and there were two
# candidate answers — keep an echo of what this keyboard has committed, or read
# text-input-v3's SURROUNDING TEXT. It is the second, and it turns out the
# compositor has been relaying it since P21 into an SDK listener stub that dropped
# it on the floor. The echo would have been wrong the moment anything else touched
# the field: a paste, a caret move, an app clearing the buffer. Run 2's "Hi. X"
# is that plumbing working end to end.
#
# EVERY RUN DRIVES THE CAPS, not the commit function. ZELTO_KBD_TYPE (P45) proves
# the relay and nothing about the keyboard as a surface; these press the key by
# name through the P46/P47 probe and the P47 classifier, which is the same path a
# finger takes.
#
# NEGATIVE-TESTED, each break made, watched to fail with the right message, and
# restored:
#   the double-tap window 0.35s -> 0.0s (no double-tap can ever be seen)
#            -> run 1 types "abC" and logs "shift once" where LOCK was expected.
#               Read the string: with no lock, the three shift presses become
#               once / unlatch / once, so the capital lands on the LAST letter
#               instead of the first two. A test asserting only "not ABc" would
#               have accepted that as easily as it accepts a dead keyboard.
#   caps lock spends itself like a one-shot (on_char clears SHIFT_LOCK too)
#            -> run 1 types "AbC" and never logs leaving the lock
#   sentence_start() drops the empty-field case  -> run 2 types "hi. X"
#   the AUTO_CAPITALIZATION hint never sent (SDK always sends HINT_NONE)
#            -> run 2 types "hi. x" and logs no auto-capital line at all, which
#               is the difference from the break above: one loses a case, the
#               other loses the whole rule
#
# NOT TESTED, because it is a design constraint rather than a behaviour: the
# double-space rule could have been a `bool last_was_space` latch on the keyboard
# instead of a read of the field. It is not, and run 2 would pass either way —
# the case that separates them is a field edited by something OTHER than these
# key caps (a paste ending in a space, a caret moved), which this harness cannot
# produce because everything it does goes through the keyboard.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-rules.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# $1 = tag, $2 = ZELTO_KBD_TAP spec, rest = extra environment.
run_boot() {
    local tag="$1" taps="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    env "$@" \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=18 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        SIM_APP=zelto-notepad \
        ZELTO_NOTEPAD_ECHO=1 ZELTO_KBD_TAP="$taps" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}
field_of() { sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$1" | tail -1; }
alive() {
    if ! grep -q "\[keyboard\] press " "$1"; then
        zt_fail "the keyboard never pressed a cap, so nothing below this line means anything" \
            "'[keyboard] press ...'" "absent (see $1)"
        zt_done
    fi
}

# ---------------------------------------------------------------------------
# Run 1 — CAPS LOCK. Auto-capitalisation is off here on purpose: this run is
# about what the SHIFT KEY does, and a capital arriving from the text rules would
# be a second explanation for every letter in the expected string.
#
# SHIFT,SHIFT   two presses inside the double-tap window -> LOCK
# a,b           both come out upper case, and the lock does NOT spend itself
# SHIFT         a locked shift unlocks
# c             lower case again
# ---------------------------------------------------------------------------
LOG1="$(run_boot lock "SHIFT,SHIFT,a,b,SHIFT,c" ZELTO_NOTEPAD_NOAUTOCAP=1)"
alive "$LOG1"

zt_expect_eq "ABc" "$(field_of "$LOG1")" \
    "double-tapping shift did not lock it: expected 'a' and 'b' BOTH upper case (a lock is not spent by the letter that follows it) and 'c' lower again after a third press (see $LOG1)"

# The mark has to say which state it is in, and the state has to be reachable from
# the log — the shift key is the one control in the system whose two 'on' states
# produce different text from an identical-looking press.
if ! grep -q '\[keyboard\] shift LOCK' "$LOG1"; then
    zt_fail "the keyboard never reported entering caps lock, so a passing string above could be one-shot shift firing twice by luck" \
        "'[keyboard] shift LOCK'" "$(grep -m1 '\[keyboard\] shift' "$LOG1" || echo absent) (see $LOG1)"
fi
# ...and leaving it. Its positive control is the line above: both come out of the
# same printf, so a format change that hid one would have failed that assertion.
lock_offs="$(grep -c '\[keyboard\] shift off' "$LOG1" || true)"
zt_expect_eq "1" "$lock_offs" \
    "the third shift press should have left caps lock exactly once (see $LOG1)"

# ---------------------------------------------------------------------------
# Run 2 — THE TEXT RULES, both of them, in one string.
#
# h        the field is empty, which is a sentence start -> 'H'
# i        mid-word -> 'i'                                          "Hi"
# SPACE    an ordinary space                                        "Hi "
# SPACE    the second one: a word followed by one space becomes
#          ". " (delete one, commit two)                            "Hi. "
# x        a full stop and a space is a sentence start -> 'X'       "Hi. X"
#
# Every step of that is a claim about the FIELD's contents, which the keyboard
# only knows because it reads surrounding text.
# ---------------------------------------------------------------------------
LOG2="$(run_boot rules "h,i,SPACE,SPACE,x")"
alive "$LOG2"

zt_expect_eq "Hi. X" "$(field_of "$LOG2")" \
    "the text rules did not fire: expected the empty field to capitalise 'h', two spaces to become '. ', and the letter after the full stop to capitalise (see $LOG2)"

if ! grep -q "double-space period after 'Hi '" "$LOG2"; then
    zt_fail "the second space did not take the double-space path — the string above could be right for some other reason (an 'x' that happened to be capital, a period typed from the symbols layer)" \
        "double-space period after 'Hi '" "$(grep -m1 'double-space' "$LOG2" || echo absent) (see $LOG2)"
fi
if ! grep -q "auto-capital on after 'Hi. '" "$LOG2"; then
    zt_fail "auto-capitalisation did not re-arm after the full stop" \
        "auto-capital on after 'Hi. '" "$(grep 'auto-capital' "$LOG2" | tail -1 || echo absent) (see $LOG2)"
fi
# And it went OFF again in between, which is the half that a latch would get
# wrong: 'i' must be lower case because the cursor is mid-word.
if ! grep -q "auto-capital off after 'H'" "$LOG2"; then
    zt_fail "auto-capitalisation never turned itself off mid-word, so 'Hi' would be 'HI' — it is being latched by the keyboard rather than derived from the text" \
        "auto-capital off after 'H'" "$(grep 'auto-capital' "$LOG2" | head -2 | tail -1 || echo absent) (see $LOG2)"
fi

# ---------------------------------------------------------------------------
# Run 3 — the same keys with the field NOT asking for sentence case.
#
# THE DISCRIMINATOR for auto-capitalisation, and the reason it is a content HINT
# rather than a keyboard setting: a note wants sentence case and a username does
# not, and nothing the keyboard can see tells them apart. The double-space period
# is NOT gated on the hint, so it still fires — which is what makes this run a
# control for one rule rather than for "the text rules" in general.
# ---------------------------------------------------------------------------
LOG3="$(run_boot nocap "h,i,SPACE,SPACE,x" ZELTO_NOTEPAD_NOAUTOCAP=1)"
alive "$LOG3"

zt_expect_eq "hi. x" "$(field_of "$LOG3")" \
    "a field that did not ask for sentence case got it anyway (or lost the double-space period along with it) (see $LOG3)"
if ! grep -q "double-space period" "$LOG3"; then
    zt_fail "the double-space period stopped working when auto-capitalisation was off — they are separate rules and only one of them is the field's to ask for" \
        "double-space period ..." "absent (see $LOG3)"
fi

# ---------------------------------------------------------------------------
# Run 4 — a PASSWORD field gets neither rule.
#
# A password may legitimately end in a space, contain no sentences and start with
# a lower-case letter. A keyboard that "corrected" any of that would be unfixable
# from the app's side, which is why iOS turns the whole language layer off here
# and why this asserts the exact keys pressed come out.
# ---------------------------------------------------------------------------
LOG4="$(run_boot secure "h,i,SPACE,SPACE,x" ZELTO_NOTEPAD_SECURE=1)"
alive "$LOG4"

zt_expect_eq "hi  x" "$(field_of "$LOG4")" \
    "a password field was auto-corrected: it must receive exactly the keys pressed — two spaces stay two spaces and nothing is capitalised (see $LOG4)"
if grep -q "double-space period" "$LOG4"; then
    zt_fail "the double-space period fired in a password field" \
        "no 'double-space period'" "$(grep -m1 'double-space' "$LOG4") (see $LOG4)"
fi

zt_done
