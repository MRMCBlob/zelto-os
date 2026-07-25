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
#   sideways     LEFT THE SCREEN HORIZONTALLY (P51). The third axis, and the one
#                the accessibility sizes actually break: nothing overflows
#                anything when a 44pt row grows past 720 units and takes its
#                stepper with it — the key is a Frame, it got the 52 units it
#                asked for, and its label fits perfectly. It is simply 33 units
#                off the edge of the screen where it cannot be pressed. Read as
#                a DECLARED number per surface (P53) — see the note in check().
#
# WHAT THE AUDIT FOUND, which is what these boots are. P50: 'andemu Demo' needed
# 206 units in a 158-unit home cell; the consent alert's two buttons wanted 61 in
# 46 (and the card itself stood 44 units taller than the rect the compositor was
# blurring behind it); the switcher's card titles wanted 49 in 30; the share
# sheet's target names 117 in 96; and Fetch's permission line 910 in 656 — that
# last one at the DEFAULT size too, so it had been overflowing since P43. P51,
# with the range extended to AX5: every Settings row's control pushed off the
# right edge of the screen; the screen TITLE 798 units wide on a 720 screen; the
# stepper's tabular value column a fixed 56 units holding a 195-unit number; the
# home widgets' three glances 371, 324 and 379 in a 298-unit bento cell; and
# Fetch's status line 887 in 656 beside a 560x160 panel narrower than its own
# caption.
#
# THE POSITIVE CONTROL IS ON EVERY ASSERTION. "0 overflowing" out of 0 text nodes
# is an empty screen, not a healthy one — a crashed sim, a surface that never came
# up and a perfect layout all report it — so each check reads the scanned count
# and fails if the surface was not really there.
#
# ONE BOOT PER SURFACE, as of P53. This test used to boot every surface TWICE,
# once at each end of the range, purely to get a sideways baseline to subtract.
# P53 narrowed probe_sideways to what no gesture can reach, and the baseline then
# measured ZERO on eleven of twelve surfaces — so the second boot was five and a
# half minutes of every test run spent comparing 0 against 0. The baselines are
# declared at the call sites instead, as EXACT values (see check() for why
# equality rather than a ceiling is what stops a declared number rotting).
#
# NEGATIVE-TESTED: reverting the consent button height to its 46-unit literal
# makes `the consent alert` fail with the two button labels named; reverting the
# home label to a bare Text makes `the home grid` fail naming 'andemu Demo';
# reverting Z_TEXT_SIZE_REFLOW_FIRST to a step above the range makes `the
# Settings list` fail on the sideways delta, naming the stepper key that left the
# screen. P53: declaring the camera's sideways baseline as 1 when it measures 0
# fails too — which is the property that matters for a declared number, because a
# mere ceiling would have passed and let the declaration rot upward unnoticed.
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
# KEEP THE LOGS WHEN IT FAILS. Every failure message here ends with "see $log",
# and this trap used to delete that file unconditionally — so the evidence a
# failure pointed at was guaranteed to be gone before anyone could read it. This
# test's whole output is a DELTA between two boots' probe dumps, which is to say
# the logs ARE the finding; without them a failure says only that a number moved.
# (P52 fixed exactly this in test_capture_lock_suppression and the same defect
# was still sitting here. Found by a real failure that could not be diagnosed.)
# The notice goes to STDERR: run-tests.sh reproduces only the ZT_FAIL protocol
# lines from stdout, so a plain echo would be swallowed by the harness meant to
# show it.
trap '[ "${ZT_FAILURES:-0}" -eq 0 ] && rm -rf "$TMP" \
      || echo "kept the probe dumps for diagnosis: $TMP" >&2' EXIT

# Every boot is at the TOP of the range. There is no point auditing the middle:
# the ladder is monotonic, so the largest size is the worst case for every box in
# the OS, and a surface that survives it survives every step below it.
#
# P51 moved this from 6 to the top of the range, which is now AX5. Overridable so
# a bisect can ask "which step does this surface stop surviving at" without
# editing the file: TS_LARGEST=7 bash test/test_text_size_overflow_sim.sh.
#
# The number comes out of the HEADER, not out of this file: when the range grows
# again the audit follows it without anybody remembering to edit a test.
TS_STEPS="$(sed -n 's/^#define Z_TEXT_SIZE_STEPS \([0-9]*\).*/\1/p' \
    "$REPO_ROOT/sdk/include/zelto/ui.h" | head -1)"
if [ -z "$TS_STEPS" ]; then
    echo "FAIL: could not read Z_TEXT_SIZE_STEPS out of sdk/include/zelto/ui.h"
    exit 1
fi
TS_LARGEST="${TS_LARGEST:-$((TS_STEPS - 1))}"

run_boot() {   # <tag> <step> <probe-app> <delay> [env...]
    local tag="$1" step="$2" who="$3" delay="$4"; shift 4
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    printf 'sys.brightness\t5\nsys.text_size\t%s\n' "$step" \
        > "$dir/data/settings.conf"
    env HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY="$delay" \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP="$who" "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

field() {   # <log> <sed expr> — one number off the probe summary line
    sed -n "$2" "$1" | head -1
}

# <name> <largest log> <min texts> <expected sideways> — the surface drew text;
# none of it needs more room than it was given on either axis; and exactly the
# declared number of nodes run off the right edge.
check() {
    local name="$1" log="$2" min="$3" want_side="$4"
    local scanned over tall side
    scanned="$(field "$log" 's/.*text \([0-9]*\) scanned .*/\1/p')"
    over="$(field "$log" 's/.*text [0-9]* scanned \([0-9]*\) overflowing.*/\1/p')"
    tall="$(field "$log" 's/.*off-surface \([0-9]*\) clipped.*/\1/p')"
    side="$(field "$log" 's/.*) \([0-9]*\) sideways.*/\1/p')"
    if [ -z "$scanned" ]; then
        zt_fail "$name never reported a probe summary — the surface did not come up, so nothing below is evidence" \
            "'probe taps: ... text N scanned ...'" "absent (see $log)"
        return
    fi
    # The positive control. "0 overflowing" out of 0 text nodes is an empty
    # screen, not a healthy one — a crashed sim, a surface that never came up and
    # a perfect layout all report it.
    if [ "$scanned" -lt "$min" ]; then
        zt_fail "$name drew $scanned text nodes at the largest text size, fewer than the $min it must have — it is not showing the content this test boots it for, so a clean count means nothing" \
            ">= $min text nodes" "$scanned (see $log)"
        return
    fi
    zt_expect_eq "0" "$over" \
        "$name has text WIDER than its box at the largest text size: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
    zt_expect_eq "0" "$tall" \
        "$name has text TALLER than its box at the largest text size — a fixed height holding a line that outgrew it, painting through into its neighbours: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
    # THE THIRD AXIS (P51), and as of P53 a DECLARED NUMBER rather than a delta
    # against a second boot.
    #
    # It was a delta because the counter used to include everything off the right
    # edge — a carousel's other pages, a scroll's rows below the fold — so an
    # absolute figure was never zero and therefore never read. P53 narrowed
    # probe_sideways to what NO GESTURE CAN REACH (a node that BEGINS on screen
    # and runs off it), and the measurement then came back 0 for eleven of the
    # twelve surfaces here. A second boot per surface to compare 0 against 0 is
    # five minutes of every test run buying nothing.
    #
    # EQUALITY, NOT A CEILING, and that is what keeps a declared baseline honest.
    # A stored number that is only an upper bound rots silently upward: it passes
    # while the truth drifts beneath it, which is precisely the failure mode this
    # project keeps getting bitten by. Asserting the exact value means a
    # declaration that is too HIGH fails just as loudly as a regression, so the
    # number cannot quietly stop describing the surface. Regenerate them all with
    # the AUDIT_BASELINE lines this prints.
    zt_expect_eq "$want_side" "$side" \
        "$name has $side node(s) that begin on screen and run off the right edge, where no gesture can reach them (declared: $want_side): $(grep -m1 'offscreen' "$log" || echo '(see the log)') (see $log)"
    echo "AUDIT_BASELINE|$name|scanned=$scanned|over=$over|tall=$tall|side=$side"
}

# <name> <tag> <probe-app> <delay> <min texts> <expected sideways> [env...]
#
# ONE BOOT, at the top of the range. The ladder is monotonic, so the largest size
# is the worst case for every box in the OS and a surface that survives it
# survives every step below. The second boot this used to take existed only to
# supply a sideways baseline, and that baseline is now declared per surface (see
# check) — which halves a test that had grown to five and a half minutes and was
# the slowest thing in the repo.
audit() {
    local name="$1" tag="$2" who="$3" delay="$4" min="$5" side="$6"; shift 6
    local hi
    hi="$(run_boot "$tag-max" "$TS_LARGEST" "$who" "$delay" "$@")"
    check "$name" "$hi" "$min" "$side"
}

# --- the home grid -----------------------------------------------------------
# An app NAME comes out of a manifest into a 158-unit cell. Also the surface with
# the biggest structural sideways count in the OS — the carousel builds every
# home page side by side — which is exactly why that count is read as a delta.
audit "the home grid" home launcher_body 6 20 0

# --- the consent alert -------------------------------------------------------
# The dialog where being able to read the two answers is the entire point.
audit "the consent alert" consent Permission 7 4 0 \
    SIM_CONSENT="os.zelto.pinger notifications"

# --- the App Switcher --------------------------------------------------------
# THE ONE SURFACE WITH A NON-ZERO DECLARATION, and it is structural rather than a
# defect: the card deck parks the NEXT card peeking in at the right edge, so that
# card and its "Paused" chip both begin on screen and run off it. They are one
# swipe away, which is the whole idiom of a deck. It is 2 at every text size —
# the peek is a layout constant, not something the type pushes — so declaring 2
# asserts the deck still peeks by exactly one card.
audit "the App Switcher" switcher recents_body 9 5 2 SIM_APP=zelto-notes \
    SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1

# --- the share sheet ---------------------------------------------------------
audit "the share sheet" share Chooser 10 5 0 SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens"

# --- the grouped inset list --------------------------------------------------
# The densest rows in the OS: Settings ▸ Lock Screen is toggles, steppers and an
# action row. This is the screen the reflow break was measured on — at the
# largest size every one of these rows is a label above its control.
audit "the Settings list" settings os.zelto.settings 8 12 0 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock

# --- the new Accessibility screen --------------------------------------------
# The screen that sets the size has to survive the size it sets, which is not a
# joke: its slider row holds two glyphs at fixed steps and a preview at Body.
audit "the Accessibility screen" accessibility os.zelto.settings 8 8 0 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=accessibility

# --- an app whose prose is not its own ---------------------------------------
audit "Fetch" fetch os.zelto.fetch 8 5 0 SIM_APP=zelto-fetch

# --- the photo grid ----------------------------------------------------------
# P53's new surface, and "the image scales" is not an answer for it: the cell is
# derived from the column count and the spacing scale, neither of which moves
# with the text size, so everything AROUND the pictures — the title, the count —
# has to survive a size the grid does not. Audited with a populated library,
# because an empty one is a different screen (see below) and would leave the grid
# itself unexercised.
PHOTOLIB="$TMP/photolib"
mkdir -p "$PHOTOLIB/photos" "$PHOTOLIB/thumbs"
pl_i=0
for wp in "$REPO_ROOT"/resources/wallpaper/*.png; do
    [ -f "$wp" ] || continue
    [ "$pl_i" -ge 5 ] && break
    pl_id="$(printf '17849%08d-00' "$((10000000 + pl_i))")"
    cp "$wp" "$PHOTOLIB/photos/$pl_id.png"
    cp "$wp" "$PHOTOLIB/thumbs/$pl_id.png"
    pl_i=$((pl_i + 1))
done
audit "the photo grid" photos os.zelto.photos 8 2 0 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB"

# --- the viewer, with its confirmation up --------------------------------
# The densest thing this app draws: a three-button toolbar in a row, and over it
# an alert whose body is prose. The toolbar is where the size hurts — three
# labels side by side is exactly the shape that walks a control off the right
# edge — and the card is a WrapText that must not outgrow the card it is in.
audit "the photo viewer" photos-viewer os.zelto.photos 12 4 0 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB" ZELTO_PHOTOS_VIEW=1 \
    ZELTO_TAP_LABEL=Delete ZELTO_TAP_APP=os.zelto.photos ZELTO_TAP_AT=9000

# --- the camera ---------------------------------------------------------------
# A viewfinder is mostly picture, which is exactly why the labels around it are
# easy to forget: a status line and a shutter under a pane whose height is a
# fixed ratio of the column, so the text has to fit what is left rather than
# push the pane off the screen.
audit "the camera" camera os.zelto.camera 10 2 0     SIM_APP=zelto-camera ZELTO_CONSENT_BIN=/bin/true

# --- the empty photo library -------------------------------------------------
# The state a new phone is in, and the one screen in this app made of PROSE. A
# Text neither wraps nor truncates, so the sentence under "No photos" is a
# WrapText — and a WrapText is handed its column at BUILD time, which is where
# this kind of thing goes wrong.
audit "the empty photo library" photos-empty os.zelto.photos 8 2 0 \
    SIM_APP=zelto-photos

# --- the SDK's own List sample -----------------------------------------------
# Not a system surface, and in for a reason: it is what an APP DEVELOPER copies.
# A List virtualises on its row height, so a row too short for its own text does
# not merely clip — it puts the wrong rows on screen.
audit "the Rows sample" hello body 8 20 0 SIM_APP=zelto-hello

zt_done
