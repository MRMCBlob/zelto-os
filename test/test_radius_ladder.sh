#!/usr/bin/env bash
# test_radius_ladder — the corner geometry, and the one relationship that is
# checkable rather than a matter of taste.
#
# WHAT P45 SETTLED. P44 flagged Z_RADIUS_* as the same shape of number as the
# type scale and the safe areas — both of which turned out to be Apple's POINTS
# spent as screen pixels — and left it alone on the grounds that the ladder
# "reads as hand-authored rather than transcribed". That was an assertion about
# intent. Measured, it is TRUE, and the measurement is the useful part:
#
#   points-as-pixels means the raw values are round in POINTS.
#   10 / 16 / 22 / 32 are round and evenly stepped (+6, +6, +10) in SCREEN UNITS,
#   and in points they are 5.4 / 8.6 / 11.9 / 17.3 — round nowhere.
#   A transcribed table looks the opposite way round.
#
# So this ladder is NOT the P43/P44 bug, and the first job of this lint is to
# stop the next phase re-litigating that: the values are pinned, and a change to
# them has to be deliberate.
#
# WHAT WAS ACTUALLY WRONG, and the assertion worth keeping. The ladder was
# UNDER-RESOLVED, not mis-scaled. Judged against the one radius in the OS that is
# calibrated rather than chosen — an app icon's corner, Z_RADIUS_ICON x ICON_SIZE
# — a home widget's corner was 22 units where the icons beside it on the SAME
# SCREEN have corners of 23.3. The big soft card was fractionally SQUARER than
# the small tiles next to it, which is backwards, and it is checkable arithmetic
# rather than an opinion because both numbers are in the repo.
#
# Z_RADIUS_PANEL could not simply be raised: it was serving four surfaces whose
# correct radii differ by more than 2x (a widget, the Control Center slider slab,
# a button, and the consent ALERT), so moving it would have fixed the widget by
# breaking the alert. Hence Z_RADIUS_WIDGET.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

GFX="$REPO_ROOT/sdk/include/zelto/gfx.h"
LAUNCHER="$REPO_ROOT/system/launcher/main.c"
VIEW="$REPO_ROOT/sdk/src/view.c"

for f in "$GFX" "$LAUNCHER" "$VIEW"; do
    if [ ! -f "$f" ]; then
        zt_fail "missing $f" "present" "absent"
        zt_done
    fi
done

radius_of() {   # the float value of a #define Z_RADIUS_<x>
    sed -n "s/^#define $1[[:space:]]*\([0-9.]*\)f.*/\1/p" "$GFX" | head -1
}

# --- 1. The ladder is pinned ------------------------------------------------
# Not because these values are sacred, but because P44 and P45 both spent a phase
# arguing about them. A change should be a decision, not a drift.
for pair in "Z_RADIUS_CHIP 10" "Z_RADIUS_CARD 16" "Z_RADIUS_PANEL 22" \
            "Z_RADIUS_WIDGET 40" "Z_RADIUS_SHEET 32"; do
    set -- $pair
    got="$(radius_of "$1")"
    if [ -z "$got" ]; then
        zt_fail "$1 is not defined in gfx.h" "$2" "missing"
    else
        zt_expect_eq "$2" "${got%.*}" "$1 changed — was that deliberate?"
    fi
done

# --- 2. The icon corner stays a FRACTION -----------------------------------
# The one radius that is resolution-independent by construction: Apple's icon
# grid makes the corner proportional to the tile, so an icon keeps its shape at
# every size it is drawn. A fixed px value here is the transcription bug arriving
# in the one place that was immune to it.
icon="$(radius_of Z_RADIUS_ICON)"
if [ -z "$icon" ]; then
    zt_fail "Z_RADIUS_ICON is not defined" "0.2237" "missing"
elif ! printf '%s' "$icon" | grep -qE '^0\.'; then
    zt_fail "Z_RADIUS_ICON is no longer a FRACTION of the icon's width" \
            "a value < 1 (a proportion)" "$icon"
fi

# --- 3. A WIDGET READS ROUNDER THAN THE ICONS BESIDE IT ---------------------
# The finding, as arithmetic. Both operands are in the repo, and both objects
# appear in the same frame, so this is a side-by-side comparison rather than a
# comparison against a remembered spec value.
icon_size="$(sed -n 's/^#define ICON_SIZE[[:space:]]*\([0-9.]*\)f.*/\1/p' \
    "$LAUNCHER" | head -1)"
widget="$(radius_of Z_RADIUS_WIDGET)"
if [ -n "$icon_size" ] && [ -n "$icon" ] && [ -n "$widget" ]; then
    verdict="$(awk -v s="$icon_size" -v i="$icon" -v w="$widget" \
        'BEGIN { c = s * i; printf "%s %.1f", (w > c ? "ok" : "SQUARER"), c }')"
    corner="${verdict#* }"
    if [ "${verdict%% *}" != "ok" ]; then
        zt_fail "a home widget's corner is not rounder than the app icons beside it" \
                "> $corner (ICON_SIZE x Z_RADIUS_ICON)" "$widget"
    fi
fi

# --- 4. The widget card actually USES the widget token ---------------------
# Catches the assertion above being satisfied by a token nothing reads.
if ! grep -q 'Z_RADIUS_WIDGET' "$VIEW"; then
    zt_fail "z_widget() no longer uses Z_RADIUS_WIDGET" \
            "the widget card draws with it" "not referenced"
fi
# And the consent alert must NOT have been swept along with it: keeping the two
# apart is the whole reason a fifth token exists.
if grep -q 'Z_RADIUS_WIDGET' "$REPO_ROOT/system/consent/main.c" 2>/dev/null; then
    zt_fail "the consent alert now uses the WIDGET radius" \
            "Z_RADIUS_PANEL (an alert is not a widget)" "Z_RADIUS_WIDGET"
fi

zt_done
