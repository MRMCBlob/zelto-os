#!/usr/bin/env bash
# test_keyboard_autocorrect_sim — the suggestion strip and whole-word autocorrect.
#
# WHY THIS IS A SEPARATE TEST FROM test_keyboard_predict_sim. That one is the
# CLASSIFIER: it corrects a PRESS before it commits, from the prefix, invisibly.
# This is AUTOCORRECT: it changes a WORD already in the field, after a boundary,
# from the whole word, visibly. They are different mechanisms with different
# failure modes and the trap is treating them as one — so they are tested apart,
# and this file never asserts anything about where a press landed.
#
# THE FIVE CLAIMS, each a boot:
#   typo    "teh" + space           -> the field holds "the " (corrected)
#   real    "the" + space           -> the field holds "the " (NOT touched)
#   revert  "teh" + space + bksp     -> the field holds "teh" (undo restores it)
#   prefix  "hel" + space           -> the field holds "hel " (a live prefix of
#                                       "hello"/"help" is not a typo, even though
#                                       "hel" is not itself a word)
#   pw      "teh" + space, secure    -> the field holds "teh " (off for passwords)
#   suggest "wrold" then tap slot 0  -> the field holds "world " (the strip is the
#                                       manual path AND the undo)
#
# AND THE BOUNDARY IS NOT A SPACE (P49 item 3). P48 hung autocorrect off on_space,
# so a space was the only spelling of a word boundary the keyboard knew:
#   period  "teh" + "."           -> "the."  (a full stop ends a word)
#   period-revert  ... + BKSP      -> "teh"   (the undo follows the boundary that
#                                       fired, not an assumed space)
#   enter   "teh" + RETURN        -> corrected (the end of a line is one too)
#   contraction "dont" + space    -> "don't " (the apostrophe put back — a
#                                       correction like any other, through the
#                                       same visible, revertible path)
#   were    "were" + space        -> "were " (the rule that keeps that table
#                                       safe: only bare forms that are NOT
#                                       English words may be expanded)
#
# NEGATIVE-TESTED, each break made, watched to fail with its own message, restored:
#   kbd_autocorrect always returns false        -> `typo` keeps "teh "; the
#       assertion fires naming teh vs the
#   the prefix guard removed (z_lm_is_prefix)    -> `prefix` autocorrects "hel" to
#       "he", and the `prefix` assertion fires
#   the password purpose ignored (predict_on)    -> `pw` corrects to "the ", the
#       pw assertion fires
#   the revert declined unconditionally          -> `revert` keeps "the", fires
#   on_suggest deletes the wrong count           -> `suggest` field is "wroldworld"
#       or similar, the suggest assertion fires
#
# Driven only by ZELTO_KBD_TAP (coordinate-free key names, plus SUG0 for the strip
# slot); the field's own echo is the witness. No pixels, no coordinates.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-ac.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One boot pressing its own caps. $1 = tag, $2 = ZELTO_KBD_TAP; rest = extra env.
run_boot() {
    local tag="$1" taps="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/xdg"
    env "$@" \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=16 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        SIM_APP=zelto-notepad \
        ZELTO_NOTEPAD_ECHO=1 ZELTO_NOTEPAD_NOAUTOCAP=1 \
        ZELTO_KBD_TAP="$taps" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}
field_of() { sed -n "s/.*\[notepad\] field='\(.*\)' len=.*/\1/p" "$1" | tail -1; }

# The keyboard reached a laid-out grid at all (else every check below is vacuous).
alive() {
    if ! grep -q "\[keyboard\] press " "$1"; then
        zt_fail "the keyboard never pressed a cap — nothing below this line means anything" \
            "'[keyboard] press ...'" "absent (see $1)"
        zt_done
    fi
}

# --- typo: a mistyped word is corrected on the boundary ----------------------
LOG="$(run_boot typo "t,e,h,SPACE")"
alive "$LOG"
if ! grep -q "autocorrect 'teh' -> 'the'" "$LOG"; then
    zt_fail "\"teh\" + space did not autocorrect to \"the\" — the whole-word correction did not fire on the boundary" \
        "autocorrect 'teh' -> 'the'" "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "the " "$(field_of "$LOG")" \
    "the field should hold the corrected word and a space (see $LOG)"

# --- real: a correctly-spelled word is left alone ----------------------------
# The control for autocorrect firing at all: if this word were "corrected", the
# mechanism is changing words that are already right, which is the failure users
# hate most. "the" is a dictionary word, so z_lm_is_word must stop it.
LOG="$(run_boot real "t,h,e,SPACE")"
alive "$LOG"
if grep -q "autocorrect '" "$LOG"; then
    zt_fail "a correctly-spelled word was autocorrected — z_lm_is_word must stop a real word from being 'corrected'" \
        "no autocorrect line" "$(grep -m1 autocorrect "$LOG") (see $LOG)"
fi
zt_expect_eq "the " "$(field_of "$LOG")" \
    "a correctly-typed word must be left exactly as typed (see $LOG)"

# --- revert: backspace immediately after undoes the correction ---------------
LOG="$(run_boot revert "t,e,h,SPACE,BKSP")"
alive "$LOG"
if ! grep -q "autocorrect reverted 'the' -> 'teh'" "$LOG"; then
    zt_fail "backspace after an autocorrection did not restore what was typed — the revert is the whole reason autocorrect is defensible" \
        "autocorrect reverted 'the' -> 'teh'" "$(grep -m1 reverted "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "teh" "$(field_of "$LOG")" \
    "the revert must put back exactly what was typed, minus the auto-inserted space (see $LOG)"

# --- prefix: a live prefix of a real word is not a typo ----------------------
# "hel" is not a word, but it IS the start of "hello"/"help", so autocorrecting it
# to the commoner "he" would fight the user mid-word. z_lm_is_prefix holds fire.
LOG="$(run_boot prefix "h,e,l,SPACE")"
alive "$LOG"
if grep -q "autocorrect '" "$LOG"; then
    zt_fail "\"hel\" was autocorrected — it is a live prefix of \"hello\"/\"help\", so it is someone part-way through a word, not a typo" \
        "no autocorrect line" "$(grep -m1 autocorrect "$LOG") (see $LOG)"
fi
zt_expect_eq "hel " "$(field_of "$LOG")" \
    "a live word-prefix must be left as typed (see $LOG)"

# --- pw: autocorrect is off for a password field -----------------------------
LOG="$(run_boot pw "t,e,h,SPACE" ZELTO_NOTEPAD_SECURE=1)"
alive "$LOG"
if grep -q "autocorrect '" "$LOG"; then
    zt_fail "autocorrect ran in a PASSWORD field — a password is exactly the string it gets wrong, and a corrected password is unfixable from the app" \
        "no autocorrect line" "$(grep -m1 autocorrect "$LOG") (see $LOG)"
fi
zt_expect_eq "teh " "$(field_of "$LOG")" \
    "a password field must receive exactly the keys pressed, uncorrected (see $LOG)"

# --- suggest: tapping a strip slot commits that word -------------------------
# The manual path and the undo in one. "wrold" is a typo whose best candidate is
# "world"; tapping slot 0 must replace the whole typed word with it plus a space,
# not append to it.
LOG="$(run_boot suggest "w,r,o,l,d,SUG0")"
alive "$LOG"
if ! grep -q "suggest slot 0 'world' (was 'wrold')" "$LOG"; then
    zt_fail "tapping the first suggestion slot did not commit \"world\" for \"wrold\"" \
        "suggest slot 0 'world' (was 'wrold')" "$(grep -m1 'suggest slot' "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "world " "$(field_of "$LOG")" \
    "tapping a suggestion must REPLACE the typed word (delete it, commit the slot + a space), not append to it (see $LOG)"

# --- period: a full stop is a word boundary too (P49 item 3) -----------------
# P48 put autocorrect on the SPACE BAR, and a space is one SPELLING of a word
# boundary, not the boundary itself: "teh." "teh!" "teh?" and a typo finished with
# RETURN all committed uncorrected, while the strip that had been showing the
# pending correction simply vanished. The rule now is the same one kbd_cur_word
# reads from the other side — a word ends where the letters stop — so a full stop
# on the symbols layer ends one exactly as the space bar does.
LOG="$(run_boot period "t,e,h,SYM,.")"
alive "$LOG"
if ! grep -q "autocorrect 'teh' -> 'the' at boundary '\.'" "$LOG"; then
    zt_fail "\"teh.\" did not autocorrect — a word boundary is the END OF A WORD, not a space, and a typo ended with a full stop is the commonest one there is (it is the end of a sentence)" \
        "autocorrect 'teh' -> 'the' at boundary '.'" "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "the." "$(field_of "$LOG")" \
    "the field should hold the corrected word and the full stop that ended it (see $LOG)"

# --- period-revert: the undo follows the boundary that fired -----------------
# THE BUG THIS EXISTS FOR. The revert reads the field to check the correction is
# still under the cursor, and P48 could hard-code the tail it looked for as
# "<word> " because the space bar was the only boundary there was. Generalising
# the boundary without generalising the revert would have left "teh." corrected
# and NOT undoable — a correction you cannot take back, which is the one thing
# that makes autocorrect defensible at all.
LOG="$(run_boot period-revert "t,e,h,SYM,.,BKSP")"
alive "$LOG"
if ! grep -q "autocorrect reverted 'the' -> 'teh'" "$LOG"; then
    zt_fail "backspace did not undo a correction that fired on a FULL STOP — the revert must follow whichever boundary committed, not assume a space" \
        "autocorrect reverted 'the' -> 'teh'" "$(grep -m1 'revert' "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "teh" "$(field_of "$LOG")" \
    "the revert must put back what was typed, minus the full stop that ended it (see $LOG)"

# --- return: so is a newline -------------------------------------------------
# The one typo a phone keyboard never fixed, because a line ends with RETURN and
# only the space bar knew what a boundary was. Asserted off the log rather than
# the field echo: the field's content contains a newline, which the echo reader
# above cannot represent on one line.
LOG="$(run_boot enter "t,e,h,ENTER")"
alive "$LOG"
if ! grep -q "autocorrect 'teh' -> 'the' at boundary '\\\\n'" "$LOG"; then
    zt_fail "\"teh\" + RETURN did not autocorrect — the end of a line is the end of a word" \
        "autocorrect 'teh' -> 'the' at boundary '\\n'" "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
fi

# --- contraction: the apostrophe put back (P49 item 3b) ----------------------
# "dont" IS in the shipped list — it has to be, or edit distance would "correct"
# it to "dot" — so this cannot come from the dictionary and does not: it is a
# table of contractions whose BARE FORM IS NOT ITSELF AN ENGLISH WORD, consulted
# before the is-a-word guard. It ships through kbd_autocorrect like every other
# correction, which is what makes it visible in the strip and revertible.
LOG="$(run_boot contraction "d,o,n,t,SPACE")"
alive "$LOG"
if ! grep -q "autocorrect 'dont' -> 'don't'" "$LOG"; then
    zt_fail "\"dont\" did not expand to \"don't\" — the apostrophe is on the symbols layer and nobody reaches for it, which is why the list carries the bare form in the first place" \
        "autocorrect 'dont' -> 'don't'" "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
fi
zt_expect_eq "don't " "$(field_of "$LOG")" \
    "the field should hold the expanded contraction (see $LOG)"

# --- were: the rule that keeps the contraction table safe --------------------
# The control for the expansion above, and the whole reason that table is thirty
# entries and not ninety: "were" is a real English word ("they were here"), so it
# must never become "we're". A table that expanded every bare contraction would be
# wrong several times a page, which is how autocorrect earns its reputation.
LOG="$(run_boot were "w,e,r,e,SPACE")"
alive "$LOG"
if grep -q "autocorrect '" "$LOG"; then
    zt_fail "\"were\" was rewritten — it is an ordinary English word, and only contractions whose bare form is NOT a word may be in the expansion table" \
        "no autocorrect line" "$(grep -m1 autocorrect "$LOG") (see $LOG)"
fi
zt_expect_eq "were " "$(field_of "$LOG")" \
    "an ordinary word must be left exactly as typed (see $LOG)"

zt_done
