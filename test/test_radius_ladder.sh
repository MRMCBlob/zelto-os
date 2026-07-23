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
            "Z_RADIUS_WIDGET 40" "Z_RADIUS_SHEET 45"; do
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

# --- 5. NESTED CORNERS: the container is never squarer than what is in it ----
#
# P45 left SHEET and CARD alone for want of an in-frame anchor — the widget got
# one because an app icon's corner is calibrated (ICON_SIZE x Z_RADIUS_ICON) and
# sits in the same frame. There is no calibrated object beside a sheet.
#
# There is, however, a relationship, and it needs no external reference at all:
# a rounded thing INSIDE another rounded thing. The share sheet (Z_RADIUS_SHEET)
# holds action rows drawn with Z_RADIUS_CARD, inset by SHEET_PAD. Two claims come
# out of that, and only the first is checkable rather than taste:
#
#   MUST: outer >= inner. A container squarer than its own contents makes the
#         inner corner poke out of the outer curve — it is wrong at any padding
#         and in any style.
#   CONCENTRIC: outer == inner + padding (the gap between the two curves stays
#         constant all the way round). P45/P46/P47 REPORTED this as a deficit
#         (sheet 32, rows 16 inset 20, so concentric wanted 36) and left it,
#         because moving Z_RADIUS_SHEET touches four surfaces and knocks it off the
#         round 10/16/22/32 ladder. P48 CLOSED it from the other side — the share
#         sheet's own inset dropped 20 -> 16, so 32 = 16 + 16 exactly — and now
#         that it is closed it is ASSERTED, so the deficit cannot creep back the
#         next time someone nudges SHEET_PAD.
# P52 TURNED THIS RELATIONSHIP THE RIGHT WAY ROUND, and the assertion survives
# the turn unchanged — which is the point of having written it down.
#
# P48 closed the deficit by pinning the sheet's INSET to whatever made the sum
# work (16, to match a 32 sheet holding 16 rows). That made a layout metric the
# slave of a radius, and it cost: 16 units is 8.6pt, half the 16pt content margin
# every other container in this OS uses, so a share sheet's rows sat twice as
# close to its edge as a Settings row does to its card.
#
# P52's spacing scale put the inset on Z_SPACE_L (29) like every other content
# margin, which leaves the sheet's own corner as the only free number: 16 + 29 =
# 45. So Z_RADIUS_SHEET is 45 now, and it is DERIVED rather than chosen. The old
# 32 also made the ladder non-monotonic — the largest surface in the OS had a
# smaller corner than a home widget (40) — which rule 6 below now forbids.
#
# The inset is a macro now, so it has to be resolved rather than read: it names a
# step of the spacing scale, and the step names its own points.
sheet="$(radius_of Z_RADIUS_SHEET)"
card="$(radius_of Z_RADIUS_CARD)"
UI_H="$REPO_ROOT/sdk/include/zelto/ui.h"
pad_step="$(sed -n 's/^#define SHEET_PAD[[:space:]]*((float)\(Z_SPACE_[A-Z0-9]*\)).*/\1/p' \
    "$REPO_ROOT/system/chooser/main.c" | head -1)"
if [ -z "$pad_step" ]; then
    zt_fail "the share sheet's inset is not a step of the spacing scale, so the concentric rule cannot be resolved" \
        "SHEET_PAD = ((float)Z_SPACE_*)" "$(grep -E '^#define SHEET_PAD' "$REPO_ROOT/system/chooser/main.c")"
fi
pad_pt="$(sed -n "s/^#define ${pad_step}[[:space:]]*Z_PT(\([0-9]*\)).*/\1/p" "$UI_H" | head -1)"
tnum="$(sed -n 's/^#define Z_TYPE_NUM[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
tden="$(sed -n 's/^#define Z_TYPE_DEN[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
sheet_pad=""
if [ -n "$pad_pt" ] && [ -n "$tnum" ] && [ -n "$tden" ]; then
    # Z_PT is integer arithmetic and TRUNCATES; resolve it the same way the
    # preprocessor does, or this check disagrees with the binary by a unit.
    sheet_pad="$(( pad_pt * tnum / tden ))"
fi
if [ -z "$sheet" ] || [ -z "$card" ] || [ -z "$sheet_pad" ]; then
    zt_fail "could not read the share sheet's nesting (radius/radius/padding)" \
            "three numbers" "sheet='$sheet' card='$card' pad='$sheet_pad'"
else
    if awk -v o="$sheet" -v i="$card" 'BEGIN { exit !(o < i) }'; then
        zt_fail "the share sheet's corner is SQUARER than the rows inside it, so their corners cut outside its curve" \
                "Z_RADIUS_SHEET >= Z_RADIUS_CARD ($card)" "$sheet"
    fi
    want="$(awk -v i="$card" -v p="$sheet_pad" 'BEGIN{printf "%.0f", i+p}')"
    if ! awk -v o="$sheet" -v w="$want" 'BEGIN { exit !(o == w) }'; then
        zt_fail "the share sheet's corners are not concentric with its rows: the row radius plus the sheet inset must equal the sheet radius, or the gap between the two curves varies around the corner" \
                "Z_RADIUS_SHEET == Z_RADIUS_CARD + SHEET_PAD ($card + $sheet_pad = $want)" "$sheet"
    fi
    echo "note: sheet corner $sheet == rows $card + inset $sheet_pad — concentric"
fi

# --- 6. THE LADDER ASCENDS --------------------------------------------------
# A ladder whose biggest surface is not its roundest is not a ladder, and it went
# unnoticed for seven phases: Z_RADIUS_SHEET was 32 against Z_RADIUS_WIDGET's 40,
# so the largest pulled surface in the OS had a squarer corner than a small home
# widget card. Nothing in rules 1-5 could see it, because each of those checks a
# radius against something OTHER than its siblings.
prev_name=""
prev_val=""
for name in Z_RADIUS_CHIP Z_RADIUS_CARD Z_RADIUS_PANEL Z_RADIUS_WIDGET Z_RADIUS_SHEET; do
    val="$(radius_of "$name")"
    if [ -z "$val" ]; then
        continue
    fi
    if [ -n "$prev_val" ] && awk -v a="$prev_val" -v b="$val" 'BEGIN { exit !(b <= a) }'; then
        zt_fail "the radius ladder does not ascend: a larger surface is drawn squarer than a smaller one" \
            "$name > $prev_name ($prev_val)" "$val"
    fi
    prev_name="$name"
    prev_val="$val"
done
echo "note: ladder ascends — chip $(radius_of Z_RADIUS_CHIP) < card $(radius_of Z_RADIUS_CARD) < panel $(radius_of Z_RADIUS_PANEL) < widget $(radius_of Z_RADIUS_WIDGET) < sheet $(radius_of Z_RADIUS_SHEET)"

# --- 7. THE CORNER EXPONENT IS DEFINED ONCE AND AGREED IN BOTH RENDERERS ----
# The squircle exponent is implemented TWICE — the SDK paints the surface, the
# compositor masks the blurred backdrop behind it — and if the two disagree, a
# material's blur is cut to a different curve than the panel drawn over it. That
# is a seam no screenshot of a single surface can show.
BACKDROP="$REPO_ROOT/compositor/src/backdrop.c"
n_sdk="$(sed -n 's/^#define Z_CORNER_N[[:space:]]*\([0-9.]*\)f\{0,1\}.*/\1/p' "$GFX" | head -1)"
n_comp="$(sed -n 's/^#define CORNER_N[[:space:]]*\([0-9.]*\)f.*/\1/p' "$BACKDROP" | head -1)"
if [ -z "$n_sdk" ]; then
    zt_fail "gfx.h does not state the corner exponent" "#define Z_CORNER_N" "absent"
elif [ -z "$n_comp" ]; then
    zt_fail "the compositor does not state its corner exponent" "#define CORNER_N" "absent"
elif ! awk -v a="$n_sdk" -v b="$n_comp" 'BEGIN { exit !(a + 0 == b + 0) }'; then
    zt_fail "the SDK and the compositor round corners to DIFFERENT curves, so a material's blur is masked to a different shape than the surface painted on it" \
        "equal exponents" "sdk=$n_sdk compositor=$n_comp"
else
    echo "note: corner exponent n=$n_sdk in both renderers"
fi

zt_done
