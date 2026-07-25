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
#                a DELTA against the default size, never as a zero: a scroll
#                builds the rows below its fold and the home carousel builds the
#                page you have not swiped to, and both are one gesture away.
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
# THE POSITIVE CONTROL IS ON EVERY ASSERTION, at BOTH SIZES. "0 overflowing" out
# of 0 text nodes is an empty screen, not a healthy one — a crashed sim, a
# surface that never came up and a perfect layout all report it — so each check
# reads the scanned count from both log lines and fails if either surface was not
# really there. That matters twice over for the sideways delta, whose baseline is
# worthless if the boot it came from drew nothing.
#
# NEGATIVE-TESTED: reverting the consent button height to its 46-unit literal
# makes `the consent alert` fail with the two button labels named; reverting the
# home label to a bare Text makes `the home grid` fail naming 'andemu Demo';
# reverting Z_TEXT_SIZE_REFLOW_FIRST to a step above the range makes `the
# Settings list` fail on the sideways delta, naming the stepper key that left the
# screen.
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
TS_DEFAULT="$(sed -n 's/^#define Z_TEXT_SIZE_DEFAULT \([0-9]*\).*/\1/p' \
    "$REPO_ROOT/sdk/include/zelto/ui.h" | head -1)"
: "${TS_DEFAULT:=3}"

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

# <name> <largest log> <default log> <min texts> — the surface drew text; none of
# it needs more room than it was given on either axis; and nothing left the
# screen SIDEWAYS that had not already left it at the default size.
check() {
    local name="$1" log="$2" base="$3" min="$4"
    local scanned over tall side scanned0 side0
    scanned="$(field "$log" 's/.*text \([0-9]*\) scanned .*/\1/p')"
    over="$(field "$log" 's/.*text [0-9]* scanned \([0-9]*\) overflowing.*/\1/p')"
    tall="$(field "$log" 's/.*off-surface \([0-9]*\) clipped.*/\1/p')"
    side="$(field "$log" 's/.*) \([0-9]*\) sideways.*/\1/p')"
    scanned0="$(field "$base" 's/.*text \([0-9]*\) scanned .*/\1/p')"
    side0="$(field "$base" 's/.*) \([0-9]*\) sideways.*/\1/p')"
    if [ -z "$scanned" ] || [ -z "$scanned0" ]; then
        zt_fail "$name never reported a probe summary at one of the two text sizes — the surface did not come up, so nothing below is evidence" \
            "'probe taps: ... text N scanned ...'" "absent (see $log and $base)"
        return
    fi
    # The positive control, on BOTH boots. "0 overflowing" out of 0 text nodes is
    # an empty screen, and a sideways count that did not grow is worth nothing if
    # the surface it was measured on never drew.
    if [ "$scanned" -lt "$min" ] || [ "$scanned0" -lt "$min" ]; then
        zt_fail "$name drew $scanned text nodes at the largest text size and $scanned0 at the default, fewer than the $min it must have — it is not showing the content this test boots it for, so a clean count means nothing" \
            ">= $min text nodes at both sizes" "$scanned / $scanned0 (see $log)"
        return
    fi
    zt_expect_eq "0" "$over" \
        "$name has text WIDER than its box at the largest text size: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
    zt_expect_eq "0" "$tall" \
        "$name has text TALLER than its box at the largest text size — a fixed height holding a line that outgrew it, painting through into its neighbours: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
    # THE THIRD AXIS (P51), and it is a DELTA rather than a zero on purpose. A
    # scroll builds the rows below its fold and the home carousel builds the page
    # you have not swiped to; both are off the surface and both are one gesture
    # away, so an absolute zero here would be a number that is never zero and
    # therefore never read. What is never reachable is content that left the
    # screen SIDEWAYS — which is exactly what a row does when its label grows and
    # pushes its control past the right edge. So the question is P50's question
    # about `worst`, asked of a count: does it GROW WHEN THE TEXT DOES.
    if [ "$side" -gt "$side0" ]; then
        zt_fail "$name pushed $((side - side0)) more node(s) off the screen SIDEWAYS at the largest text size than at the default — a control or a label that grew past the right edge, where no scroll can reach it: $(grep -m1 'offscreen' "$log" || echo '(see the log)') (see $log)" \
            "<= $side0 sideways (what it has at the default size)" "$side (see $log)"
    fi
}

# <name> <tag> <probe-app> <delay> <min texts> [env...] — the same surface, twice:
# once at the top of the range and once at the default. The second boot is not
# duplication, it is the BASELINE the sideways count is read against and the
# positive control that says the first boot drew anything at all.
audit() {
    local name="$1" tag="$2" who="$3" delay="$4" min="$5"; shift 5
    local hi lo
    hi="$(run_boot "$tag-max" "$TS_LARGEST" "$who" "$delay" "$@")"
    lo="$(run_boot "$tag-def" "$TS_DEFAULT" "$who" "$delay" "$@")"
    check "$name" "$hi" "$lo" "$min"
}

# --- the home grid -----------------------------------------------------------
# An app NAME comes out of a manifest into a 158-unit cell. Also the surface with
# the biggest structural sideways count in the OS — the carousel builds every
# home page side by side — which is exactly why that count is read as a delta.
audit "the home grid" home launcher_body 6 20

# --- the consent alert -------------------------------------------------------
# The dialog where being able to read the two answers is the entire point.
audit "the consent alert" consent Permission 7 4 \
    SIM_CONSENT="os.zelto.pinger notifications"

# --- the App Switcher --------------------------------------------------------
audit "the App Switcher" switcher recents_body 9 5 SIM_APP=zelto-notes \
    SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1

# --- the share sheet ---------------------------------------------------------
audit "the share sheet" share Chooser 10 5 SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens"

# --- the grouped inset list --------------------------------------------------
# The densest rows in the OS: Settings ▸ Lock Screen is toggles, steppers and an
# action row. This is the screen the reflow break was measured on — at the
# largest size every one of these rows is a label above its control.
audit "the Settings list" settings os.zelto.settings 8 12 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock

# --- the new Accessibility screen --------------------------------------------
# The screen that sets the size has to survive the size it sets, which is not a
# joke: its slider row holds two glyphs at fixed steps and a preview at Body.
audit "the Accessibility screen" accessibility os.zelto.settings 8 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=accessibility

# --- an app whose prose is not its own ---------------------------------------
audit "Fetch" fetch os.zelto.fetch 8 5 SIM_APP=zelto-fetch

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
audit "the photo grid" photos os.zelto.photos 8 2 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB"

# --- the viewer, with its confirmation up --------------------------------
# The densest thing this app draws: a three-button toolbar in a row, and over it
# an alert whose body is prose. The toolbar is where the size hurts — three
# labels side by side is exactly the shape that walks a control off the right
# edge — and the card is a WrapText that must not outgrow the card it is in.
audit "the photo viewer" photos-viewer os.zelto.photos 12 4 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB" ZELTO_PHOTOS_VIEW=1 \
    ZELTO_TAP_LABEL=Delete ZELTO_TAP_APP=os.zelto.photos ZELTO_TAP_AT=9000

# --- the empty photo library -------------------------------------------------
# The state a new phone is in, and the one screen in this app made of PROSE. A
# Text neither wraps nor truncates, so the sentence under "No photos" is a
# WrapText — and a WrapText is handed its column at BUILD time, which is where
# this kind of thing goes wrong.
audit "the empty photo library" photos-empty os.zelto.photos 8 2 \
    SIM_APP=zelto-photos

# --- the SDK's own List sample -----------------------------------------------
# Not a system surface, and in for a reason: it is what an APP DEVELOPER copies.
# A List virtualises on its row height, so a row too short for its own text does
# not merely clip — it puts the wrong rows on screen.
audit "the Rows sample" hello body 8 20 SIM_APP=zelto-hello

zt_done
