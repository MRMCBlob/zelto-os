#!/usr/bin/env bash
# test_keyboard_learn_sim — the LEARNED user dictionary (P49 item 2).
#
# WHY THIS IS A SEPARATE FILE FROM test_keyboard_autocorrect_sim. That one is
# about a mechanism that changes text. This is about a STORE: what goes into it,
# what must never go into it, that it survives a reboot, and that there is a way
# out of it. Those are privacy claims as much as behavioural ones, and a privacy
# claim that is bundled into a test about something else is a privacy claim
# nobody re-reads.
#
# THE SIX CLAIMS, and the reboot is done the way every persistence test here does
# it — a SECOND SIM BOOT ON THE SAME ZELTO_DATA_DIR, which is a reboot as far as
# /var/zelto is concerned, in seconds rather than TCG minutes:
#
#   learn-explicit  "zelto" + space is autocorrected to "hello"; BACKSPACE takes
#                   it back, and that revert LEARNS the word. The strongest
#                   signal there is: a person disagreeing with the model about a
#                   word the model already had an opinion on.
#   persist         a second boot on the same data dir restores it, and "zelto"
#                   + space is no longer corrected. This is the claim that the
#                   learned word reached the SAME model — an entry that did not
#                   merge into the list would still be autocorrected away.
#   learn-implicit  a word nothing corrects ("mrblo") is counted, not learned, at
#                   one and two boundaries, and learned at the third. A word typed
#                   ONCE is a typo.
#   password        the same three boundaries in a PASSWORD field produce no
#                   candidate and no learn. The positive control is the
#                   learn-implicit boot above: identical taps, identical build,
#                   one bit of content purpose different.
#   forget          a data dir with a learned word plus a bumped
#                   sys.kbd_forget_learned comes up with the word gone and the
#                   file deleted — the Settings > Keyboard > Clear Learned Words
#                   path, including the case where the keyboard was not running
#                   when the clear was requested.
#   count           the keyboard publishes sys.kbd_learned, which is what makes
#                   the store VISIBLE to the user at all.
#
# NEGATIVE-TESTED, each break made, watched to fail with its own message,
# restored:
#   kbd_learn_seen threshold 3 -> 1   `learn-implicit` learns on the first
#       boundary and the "not yet" assertion fires
#   kbd_learn ignores predict_on      `password` learns from a password field and
#       its assertion fires naming the word
#   dict_save writes nothing          `persist` finds no restore line
#   dict_load ignores the epoch       `forget` still has the word
#
# Driven only by ZELTO_KBD_TAP (coordinate-free key names); the field's own echo
# and the keyboard's own log are the witnesses. No pixels, no coordinates.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-learn.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One boot. $1 = tag, $2 = data-dir name (SHARED between boots to make a reboot),
# $3 = ZELTO_KBD_TAP, $4 = SHOT_DELAY; rest = extra env.
run_boot() {
    local tag="$1" datatag="$2" taps="$3" delay="$4"; shift 4
    local dir="$TMP/$tag" data="$TMP/data-$datatag"
    mkdir -p "$dir" "$data" "$TMP/xdg-$tag"
    env "$@" \
        HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY="$delay" \
        ZELTO_DATA_DIR="$data" SIM_RUNTIME_DIR="$TMP/xdg-$tag" \
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

# --- learn-explicit: a revert teaches the word -------------------------------
# "zelto" is a string the shipped dictionary corrects to "hello" (it is within
# the edit ceiling of hello / melt / zero), which is exactly the situation
# learning exists for: the keyboard is confidently wrong about a word that is
# yours. The revert is the user saying so.
LOG="$(run_boot explicit shared "z,e,l,t,o,SPACE,BKSP" 18)"
alive "$LOG"
if ! grep -q "autocorrect 'zelto' -> " "$LOG"; then
    zt_fail "\"zelto\" was not autocorrected on the first boot — this test's whole premise is that the shipped dictionary gets this word wrong, so nothing below is meaningful" \
        "autocorrect 'zelto' -> ..." "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
    zt_done
fi
if ! grep -q "autocorrect reverted .* -> 'zelto'" "$LOG"; then
    zt_fail "backspace did not revert the correction" \
        "autocorrect reverted ... -> 'zelto'" "$(grep -m1 reverted "$LOG" || echo absent) (see $LOG)"
fi
if ! grep -q "\[keyboard\] learned 'zelto'" "$LOG"; then
    zt_fail "reverting an autocorrection did not LEARN the word — a revert is a person saying \"no, I meant this\" about a word the model already judged, and it is the strongest signal a keyboard ever gets" \
        "[keyboard] learned 'zelto'" "$(grep -m1 "learned '" "$LOG" || echo absent) (see $LOG)"
fi

# --- count: the store is visible ---------------------------------------------
# A learned dictionary the user cannot see the size of is a store with no way to
# ask what is in it. The keyboard publishes the count through the same broker
# Settings reads (sys.kbd_learned), which is what puts a number on the Keyboard
# screen.
if ! grep -Eq "settings_set sys.kbd_learned=1" "$LOG"; then
    zt_fail "the keyboard did not publish sys.kbd_learned — Settings > Keyboard reads that key, and without it the user cannot see that a store exists at all" \
        "settings_set sys.kbd_learned=1" "$(grep -m1 'sys.kbd_learned' "$LOG" || echo absent) (see $LOG)"
fi

# --- persist: a SECOND BOOT on the same data dir ------------------------------
# The reboot. It restores the word, and — the part that proves the word reached
# the same model rather than a list beside it — "zelto" is no longer corrected.
LOG="$(run_boot persist shared "z,e,l,t,o,SPACE" 18)"
alive "$LOG"
if ! grep -q "learned: 1 word(s) restored" "$LOG"; then
    zt_fail "the learned word did not survive a reboot — it is written to /var/zelto through the OS's own storage, and a second boot on the same data dir is that reboot" \
        "learned: 1 word(s) restored" "$(grep -m1 'learned:' "$LOG" || echo absent) (see $LOG)"
fi
if grep -q "autocorrect 'zelto'" "$LOG"; then
    zt_fail "a LEARNED word was autocorrected anyway — it did not merge into the list the trie is built from, so there are two models and they disagree" \
        "no autocorrect of 'zelto'" "$(grep -m1 "autocorrect 'zelto'" "$LOG") (see $LOG)"
fi
zt_expect_eq "zelto " "$(field_of "$LOG")" \
    "after the word is learned the field must hold exactly what was typed (see $LOG)"

# --- learn-implicit: three boundaries, not one -------------------------------
# "mrblo" is far enough from everything that autocorrect never proposes anything
# for it, so it reaches the counter. One occurrence is a typo; three is a word.
LOG="$(run_boot implicit fresh1 "m,r,b,l,o,SPACE,m,r,b,l,o,SPACE,m,r,b,l,o,SPACE" 22)"
alive "$LOG"
if ! grep -q "learn candidate 'mrblo' seen 1/3" "$LOG"; then
    zt_fail "a novel word was not counted as a learning candidate at its first boundary" \
        "learn candidate 'mrblo' seen 1/3" "$(grep -m1 'learn candidate' "$LOG" || echo absent) (see $LOG)"
fi
if grep -q "learned 'mrblo'" "$LOG" && \
   ! grep -q "learn candidate 'mrblo' seen 2/3" "$LOG"; then
    zt_fail "a word typed ONCE was learned — one occurrence of a string the dictionary does not carry is a typo, and learning it makes the typo permanent and uncorrectable" \
        "counted twice before being learned" "learned immediately (see $LOG)"
fi
if ! grep -q "learned 'mrblo'" "$LOG"; then
    zt_fail "a word typed three times without correction was never learned — the implicit path is what makes this work for words a user never gets a correction on" \
        "[keyboard] learned 'mrblo'" "$(grep -m1 "learned '" "$LOG" || echo absent) (see $LOG)"
fi

# --- password: nothing typed into a password field is ever learned -----------
# The positive control is the boot immediately above: the same taps, the same
# binary, one bit of relayed content purpose different. Without it "no learn
# line" would also be satisfied by the keyboard being broken.
LOG="$(run_boot pw fresh2 "m,r,b,l,o,SPACE,m,r,b,l,o,SPACE,m,r,b,l,o,SPACE" 22 \
        ZELTO_NOTEPAD_SECURE=1)"
alive "$LOG"
if grep -q "learn candidate" "$LOG"; then
    zt_fail "a PASSWORD field produced a learning candidate — the string never even reaches the in-RAM counter table, let alone the file, and that is the first of the three rules that make this store defensible" \
        "no learn candidate line" "$(grep -m1 'learn candidate' "$LOG") (see $LOG)"
fi
if grep -q "\[keyboard\] learned '" "$LOG"; then
    zt_fail "a word was LEARNED from a password field" \
        "no learned line" "$(grep -m1 "learned '" "$LOG") (see $LOG)"
fi

# --- forget: the way out, honoured even from a cold boot ---------------------
# Settings bumps sys.kbd_forget_learned. The keyboard compares it against an
# epoch it persisted rather than observing an event, so a clear requested while
# the keyboard was dead is still applied — which is the case a live broadcast
# cannot cover and the one a phone actually hits (Settings, then a reboot).
printf 'sys.kbd_forget_learned\t1\n' > "$TMP/data-shared/settings.conf"
LOG="$(run_boot forget shared "z,e,l,t,o,SPACE" 18)"
alive "$LOG"
if ! grep -q "learned: cleared (epoch 1)" "$LOG"; then
    zt_fail "a clear requested while the keyboard was not running was not honoured on its next boot — Clear Learned Words is the only way out of this store, and it has to work even when the keyboard is not up to hear about it" \
        "learned: cleared (epoch 1)" "$(grep -m1 'learned:' "$LOG" || echo absent) (see $LOG)"
fi
if ! grep -q "autocorrect 'zelto' -> " "$LOG"; then
    zt_fail "\"zelto\" was still treated as a known word after the store was cleared — the words were forgotten from the file but not from the model" \
        "autocorrect 'zelto' -> ..." "$(grep -m1 autocorrect "$LOG" || echo absent) (see $LOG)"
fi

zt_done
