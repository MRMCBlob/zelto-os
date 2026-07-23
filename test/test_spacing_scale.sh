#!/usr/bin/env bash
# test_spacing_scale — every gap in the OS is a step of Z_SPACE_*, and the steps
# themselves go through Z_PT().
#
# WHY THIS IS A TEST, and it is the same argument as test_type_scale_in_points.
# Until P52 there was no spacing scale at all: every gap was an inline literal.
# The histogram of those literals is what convicted them — the three commonest
# were `spacing = 8`, `spacing = 10` and `spacing = 12`, which are exactly
# SwiftUI's default VStack spacing (8pt), its default HStack spacing (10pt) and
# the 12pt step of Apple's grid, typed straight into SCREEN UNITS. The paddings
# were 16 / 20 / 24 / 32 — the grid's POINT steps. So every gap in the OS was
# drawn at ~54% of what it was written for.
#
# That is the P43 type-scale bug and the P44 safe-area bug for the third time,
# and it survived for the third time for the same reason: a bare integer in
# `spacing = 8` is indistinguishable from a correct one, the factor is 1.85, and
# a uniformly cramped OS looks like a style rather than a defect. No compiler,
# no screenshot and no reviewer catches it. A lint does.
#
# The rule is therefore mechanical: a layout may not contain a bare number where
# a gap goes. Two carve-outs are legitimate and are enumerated below rather than
# left to judgement — if a new one is needed it gets added HERE, with a reason,
# which is the point.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

UI_H="$REPO_ROOT/sdk/include/zelto/ui.h"

if [ ! -f "$UI_H" ]; then
    zt_fail "sdk/include/zelto/ui.h is missing" "present" "absent"
    zt_done
fi

# --- 1. The scale exists, and every step is Z_PT(points) -------------------
# A step written as a bare number would be the very bug the scale exists to
# stop, sitting in the scale itself.
steps="2XS XS S M L XL 2XL 3XL"
for s in $steps; do
    line="$(grep -E "^#define Z_SPACE_${s}[[:space:]]" "$UI_H" || true)"
    if [ -z "$line" ]; then
        zt_fail "the spacing scale is missing a step" "#define Z_SPACE_$s" "absent"
        continue
    fi
    case "$line" in
        *Z_PT\(*) ;;
        *) zt_fail "Z_SPACE_$s is not a point value through Z_PT() — a bare number here is a point spent as a pixel" \
               "Z_PT(<points>)" "$line" ;;
    esac
done

# The scale must ASCEND. A ladder whose steps are not ordered is not a ladder,
# and a mis-ordered step would let "the smaller gap" silently be the larger one.
prev=-1
for s in $steps; do
    pts="$(sed -n "s/^#define Z_SPACE_${s}[[:space:]]*Z_PT(\([0-9]*\)).*/\1/p" \
        "$UI_H" | head -1)"
    if [ -z "$pts" ]; then
        continue
    fi
    if [ "$pts" -le "$prev" ]; then
        zt_fail "the spacing scale does not ascend at Z_SPACE_$s" \
            "> ${prev}pt" "${pts}pt"
    fi
    prev="$pts"
done

# --- 2. No bare gap literal anywhere in the toolkit or the system ----------
#
# THE CARVE-OUTS, each with its reason:
#
#   glyphs.h, the launcher's battery meter and its App Library quad mark
#     A glyph is DRAWING, not layout — the gaps between a battery meter's
#     segments are part of the mark the way a stroke width is, and snapping them
#     to a 4pt grid would redraw the icon rather than align it.
#   sdk/src/view.c's Button and TextField insets
#     Control METRICS, which the control-metrics stage owns. They are also
#     currently WRONG in a way this scale would paper over: each carries a
#     comment reading "~44px tall at body size" written when Body was 17 units
#     (pre-P43), so today Button measures 69 units and TextField 65 against the
#     81-unit (44pt) touch-target minimum. Regridding the inset would hide that;
#     it needs deriving from z_row_h() instead.
#   a `.padding = 1.0f` ring
#     A hairline ring drawn as padding, not a gap.
#   settings' wallpaper selection ring, `Padding(4.0f)` inside `CornerRadius(18)`
#     Not a gap either: it is the CONCENTRIC inset between a selected thumb's
#     corner (14) and the ring drawn around it (18 = 14 + 4). That relationship
#     is the radius stage's to express, and regridding the 4 here would break the
#     concentricity while looking like an improvement.
#
# Everything else must name a step.
scan_dirs="$REPO_ROOT/sdk $REPO_ROOT/system"
hits="$(grep -rnE '(\.spacing[[:space:]]*=|\.padding[[:space:]]*=|Padding\()[[:space:]]*[0-9]' \
        --include='*.c' --include='*.h' $scan_dirs 2>/dev/null \
    | grep -vE '(\.spacing|\.padding)[[:space:]]*=[[:space:]]*0(\.0f)?[,.)[:space:]}]*$' \
    | grep -vE '(\.spacing|\.padding)[[:space:]]*=[[:space:]]*0(\.0f)?[,)]' \
    | grep -vE 'Padding\(0' \
    | grep -vE '(\.padding)[[:space:]]*=[[:space:]]*1\.0f' \
    | grep -vE '^[^:]*/settings/main\.c:[0-9]+:.*CornerRadius\(18\.0f, Padding\(4\.0f' \
    | grep -vE '^[^:]*/(glyphs\.h):' \
    | grep -vE '^[^:]*/launcher/main\.c:[0-9]+:[[:space:]]*(ZStackOpts row = \{\.spacing = 3\.0f|.*\.spacing = 2, \.align = Z_ALIGN_CENTER)' \
    | grep -vE '^[^:]*/sdk/src/view\.c:[0-9]+:[[:space:]]*n->padding = 1[24]\.0f;' \
    | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' \
    || true)"
if [ -n "$hits" ]; then
    n="$(printf '%s\n' "$hits" | wc -l | tr -d ' ')"
    zt_fail "a layout still writes a gap as a bare number — that is a point value being spent as a pixel, the P43/P44/P52 bug" \
        "a Z_SPACE_* step" "$n site(s): $(printf '%s\n' "$hits" | head -4 | tr '\n' ' ')"
else
    echo "note: no bare gap literals outside the enumerated carve-outs"
fi

# --- 3. POSITIVE CONTROL for the scan above -------------------------------
# Rule 2 asserts an ABSENCE, and an absence is also what a scan that matches
# nothing reports — a broken regex, a bad path and a clean tree are the same
# output. So run the identical pipeline over a file that is KNOWN to contain the
# pattern and require that it FINDS it.
ctl="$(mktemp)"
printf '%s\n' 'ZStackOpts col = {.spacing = 12, .align = Z_ALIGN_LEADING};' > "$ctl"
ctl_hits="$(grep -rnE '(\.spacing[[:space:]]*=|\.padding[[:space:]]*=|Padding\()[[:space:]]*[0-9]' \
        "$ctl" 2>/dev/null || true)"
rm -f "$ctl"
if [ -z "$ctl_hits" ]; then
    zt_fail "the bare-literal scan cannot see a bare literal — rule 2's clean result proves nothing" \
        "the control line matches" "no match"
else
    echo "note: positive control — the scan does detect '.spacing = 12'"
fi

# --- 4. The two metrics the scale is anchored on ---------------------------
# ROW_PAD and SEC_GAP are the settings list's own insets and were the pair that
# proved the bug: ROW_PAD had already been fixed to Z_PT(16) in P44 (its comment
# says "16 units is 8.6pt where the list it copies uses 16pt"), while SEC_GAP sat
# beside it as a bare 30.0f. They must both be steps now.
SET_C="$REPO_ROOT/system/apps/settings/main.c"
for m in ROW_PAD SEC_GAP; do
    line="$(grep -E "^#define ${m}[[:space:]]" "$SET_C" || true)"
    case "$line" in
        *Z_SPACE_*) ;;
        "") zt_fail "settings/main.c no longer defines $m" "defined" "absent" ;;
        *) zt_fail "$m is not a step of the spacing scale" "Z_SPACE_*" "$line" ;;
    esac
done

zt_done
