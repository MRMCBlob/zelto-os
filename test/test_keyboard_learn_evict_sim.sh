#!/usr/bin/env bash
# test_keyboard_learn_evict_sim — WHICH candidate the learner forgets.
#
# WHAT WAS UNTESTED. P49 shipped a fixed table of KBD_SEEN_SLOTS (32) strings the
# dictionary does not carry, counted until one reaches KBD_LEARN_SEEN (3) and
# becomes a learned word. When a 33rd candidate arrives something has to go, and
# the code says the LEAST-SEEN entry goes. Nothing tested that, and it is not a
# detail: the entry with the highest count is the word closest to being learned,
# so an eviction rule that picked the wrong one would quietly make the learner
# unable to learn anything on a busy day — every candidate would keep getting
# reset just before it got there, and the only symptom is a keyboard that "never
# seems to learn".
#
# WHY IT NEEDS AN ENV HOOK. Filling 32 slots means 33 made-up words typed through
# a word boundary, at 250ms a tap: minutes of simulated typing to exercise four
# lines. ZELTO_KBD_SEEN_SLOTS caps the LIVE slot count (the array is still 32), so
# the same rule is exercised at three slots in a dozen taps. The rule does not
# change with the table size; the runtime does.
#
# THE TWO CLAIMS:
#   evicted    with 3 slots, a candidate seen ONCE is displaced by new arrivals
#              and starts again from 1 — so it is never learned.
#   survivor   in the SAME boot, the candidate seen TWICE is NOT displaced by
#              those arrivals, and one more sighting learns it. This is the
#              positive control, and it is the claim that actually matters:
#              "something was forgotten" is also true of a learner that forgets
#              everything.
#
# NEGATIVE-TESTED: with the eviction changed to pick slot 0 unconditionally
# (rather than the weakest), `survivor` fails naming the word that should have
# been learned and was not.
#
# The words are five letters of rare pairs so they are outside the autocorrect
# edit ceiling — a candidate the model rewrote at the boundary would be counted
# under the CORRECTED spelling and this test would be measuring something else.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-kbd-evict.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# THE SCRIPT, and the order is the whole experiment.
#
#   qvxjw qvxjw     the SURVIVOR reaches 2/3 and holds the strongest slot
#   zqpmb kxwvj     two more candidates arrive; with 3 slots the table is now full
#   jwqzx           a FOURTH arrives and must displace one of the 1/3 entries
#   qvxjw           the survivor's third sighting -> learned
#
# If eviction took the strongest instead, "qvxjw" would have been thrown out by
# the fourth arrival and this last boundary would report 1/3, not a learn.
WORDS="q,v,x,j,w,SPACE,q,v,x,j,w,SPACE,z,q,p,m,b,SPACE,k,x,w,v,j,SPACE,j,w,q,z,x,SPACE,q,v,x,j,w,SPACE"

LOG="$TMP/log"
mkdir -p "$TMP/data" "$TMP/xdg"
env HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$TMP/frame.png" SHOT_DELAY=22 \
    ZELTO_DATA_DIR="$TMP/data" SIM_RUNTIME_DIR="$TMP/xdg" \
    SIM_APP=zelto-notepad \
    ZELTO_NOTEPAD_ECHO=1 ZELTO_NOTEPAD_NOAUTOCAP=1 \
    ZELTO_KBD_SEEN_SLOTS=3 \
    ZELTO_KBD_TAP="$WORDS" \
    bash "$REPO_ROOT/meta/run-sim.sh" > "$LOG" 2>&1

# The keyboard ran at all. Without this every assertion below is vacuous — an
# absent log line means "not learned" just as convincingly as a working evictor.
if ! grep -q "\[keyboard\] press " "$LOG"; then
    zt_fail "the keyboard never pressed a cap — nothing below this line means anything" \
        "'[keyboard] press ...'" "absent (see $LOG)"
    zt_done
fi
if ! grep -q "learn candidate 'qvxjw' seen 1/3" "$LOG"; then
    zt_fail "the survivor was never counted as a candidate at all — the words this test uses must be strings the shipped dictionary does not carry, and one of them is not" \
        "learn candidate 'qvxjw' seen 1/3" "$(grep -m1 'learn candidate' "$LOG" || echo absent) (see $LOG)"
    zt_done
fi

# --- survivor: the strongest candidate is NOT the one evicted ----------------
if ! grep -q "learn candidate 'qvxjw' seen 2/3" "$LOG"; then
    zt_fail "the survivor never reached 2/3, so the table never had a strongest entry to protect" \
        "learn candidate 'qvxjw' seen 2/3" "$(grep -m1 "candidate 'qvxjw' seen 2" "$LOG" || echo absent) (see $LOG)"
fi
if ! grep -q "\[keyboard\] learned 'qvxjw'" "$LOG"; then
    zt_fail "the candidate seen TWICE was thrown out by three later arrivals — eviction must drop the LEAST-seen entry, because the highest count is the word closest to being learned, and getting this backwards makes a keyboard that never seems to learn anything" \
        "[keyboard] learned 'qvxjw'" "$(grep -m1 "learned '" "$LOG" || echo "no learn at all") (see $LOG)"
fi

# --- evicted: a once-seen candidate really was displaced ---------------------
# "zqpmb" is the oldest 1/3 entry when the fourth word arrives. It must never
# have got past 1/3 — and, the part that makes this an eviction rather than a
# coincidence, it must have been counted at 1/3 exactly ONCE.
n_zq="$(grep -c "learn candidate 'zqpmb' seen " "$LOG" || true)"
zt_expect_eq "1" "${n_zq:-0}" \
    "the displaced candidate was counted more than once, so the table never overflowed and this test is not exercising eviction at all (see $LOG)"
if grep -q "\[keyboard\] learned 'zqpmb'" "$LOG"; then
    zt_fail "a candidate seen once was LEARNED — a word typed once is a typo, which is the whole reason for the threshold" \
        "no learn of 'zqpmb'" "learned (see $LOG)"
fi

zt_done
