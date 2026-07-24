#!/usr/bin/env bash
# test_control_metrics — the controls are sized in POINTS, and the ones a finger
# has to hit reach the 44pt minimum.
#
# WHY THIS IS A TEST, and it is the fourth time this project has written this
# comment. A control's metrics are numbers off a spec sheet, and a spec sheet is
# in POINTS while this OS lays out in SCREEN UNITS at 1pt = 1.85u. Transcribing
# one without Z_PT() produces a control at ~54% of its intended size, which is
# not a crash, not a warning, and not something a screenshot makes obvious — a
# small switch looks like a small switch. It has now been found in the type scale
# (P43), the safe areas (P44), every gap in the OS (P52 stage 1a) and here.
#
# THE SWITCH IS THE CLEAREST CASE THE PROJECT HAS. iOS's track is 51x31 POINTS
# and this OS drew 52x32 UNITS — the sheet transcribed verbatim. The tell is
# proportional and needs no external reference: iOS's switch stands 31 of a 44pt
# row (70% of the row's height), while Zelto's stood 32 of 81 (40%). It looked
# wrong NEXT TO ITS OWN ROW.
#
# The touch targets are the other half. Z_ROW_H already encodes Apple's 44pt
# minimum correctly, but Button and ZTextField each carried a hand-picked inset
# with a comment claiming it summed to 44 — written when Body was 17 units, i.e.
# before P43. At today's ramp they measured 69u (37pt) and 65u (35pt).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

UI_H="$REPO_ROOT/sdk/include/zelto/ui.h"
SETTINGS="$REPO_ROOT/system/apps/settings/main.c"
VIEW="$REPO_ROOT/sdk/src/view.c"
SLIDER="$REPO_ROOT/sdk/src/slider.c"

for f in "$UI_H" "$SETTINGS" "$VIEW" "$SLIDER"; do
    if [ ! -f "$f" ]; then
        zt_fail "missing $f" "present" "absent"
        zt_done
    fi
done

# Z_PT is integer arithmetic and TRUNCATES; resolve it exactly as the
# preprocessor does or this test disagrees with the binary by a unit.
NUM="$(sed -n 's/^#define Z_TYPE_NUM[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
DEN="$(sed -n 's/^#define Z_TYPE_DEN[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
pt() { echo $(( $1 * NUM / DEN )); }

# --- 1. THE SWITCH: every metric through Z_PT, none a bare number -----------
# Matched on the SOURCE rather than on a rendered frame: the failure being
# guarded is "someone typed the spec value straight in", which is visible in the
# text and would need a screenshot diff to catch any other way.
sw_line="$(grep -n 'const float TRACK_W' "$SETTINGS" || true)"
if [ -z "$sw_line" ]; then
    zt_fail "the switch's track metrics are gone from settings/main.c" \
        "const float TRACK_W = ..." "absent"
else
    case "$sw_line" in
        *Z_PT\(51\)*Z_PT\(31\)*) ;;
        *)  zt_fail "the switch track is not Apple's 51x31 POINTS through Z_PT — a bare number here is the spec sheet spent as pixels, which is what made it 40% of its row instead of 70%" \
                "Z_PT(51) x Z_PT(31)" "$sw_line" ;;
    esac
fi
# The knob and the inset must be DERIVED, not declared: the inset is what is left
# of the track after the knob, and writing it separately is how the three drift.
if ! grep -q 'INSET = (TRACK_H - KNOB)' "$SETTINGS"; then
    zt_fail "the switch's knob inset is not derived from the track and the knob" \
        "INSET = (TRACK_H - KNOB) * 0.5f" "$(grep -c 'INSET' "$SETTINGS") mention(s), none derived"
fi

# --- 2. THE TOUCH TARGETS: Button and ZTextField reach the 44pt floor -------
# They must ask for the floor by NAME (Z_ROW_H), not re-derive it: a control that
# computes its own 44pt is a second opinion about the minimum, and the two can
# disagree. min_h is checked rather than padding because the floor is an OUTER
# height — a padding-based total was exactly the arithmetic that went stale.
for ctl in z_button z_text_field; do
    body="$(sed -n "/^ZView ${ctl}\{0,1\}/,/^}/p" "$VIEW")"
    if [ -z "$body" ]; then
        # z_text_field may be spelled differently; fall back to a window search.
        body="$(grep -A 40 "$ctl" "$VIEW" | head -60)"
    fi
    if ! printf '%s' "$body" | grep -q 'min_h *= *(float)Z_ROW_H'; then
        zt_fail "$ctl does not take the 44pt touch-target floor (Z_ROW_H) as a minimum height — its total was a hand-picked padding that went stale when the type scale moved" \
            "n->min_h = (float)Z_ROW_H" "absent"
    fi
done
# And the stale comment must not come back: a literal inset claiming to produce
# 44px is the exact artefact that survived P43.
if grep -qE 'padding = 1[24]\.0f' "$VIEW"; then
    zt_fail "a control inset is a bare literal again, with the total it claims to produce written in a comment rather than expressed" \
        "an expression" "$(grep -nE 'padding = 1[24]\.0f' "$VIEW" | head -2 | tr '\n' ' ')"
fi

# --- 3. THE SLIDER: the thumb is the number that was wrong ------------------
# The track was 6u = 3.2pt (close enough to look right); the thumb was `t * 4` =
# 24u = 13pt against Apple's ~28pt. So the defect was the RATIO, hidden behind a
# plausible track. Both are points now.
if ! grep -q 'knob = (float)Z_PT(28)' "$SLIDER"; then
    zt_fail "the slider's thumb is not ~28pt through Z_PT — as a multiple of the track it was 13pt, and the ratio (4x where iOS is 7x) is what hid it" \
        "knob = (float)Z_PT(28)" "$(grep -n 'knob = ' "$SLIDER" | head -1)"
fi
if ! grep -q 'Z_PT(4)' "$SLIDER"; then
    zt_fail "the slider's default track thickness is not Apple's 4pt through Z_PT" \
        "(float)Z_PT(4)" "$(grep -n 'thickness > 0' "$SLIDER" | head -1)"
fi
# A caller repeating the default defeats it — that is how the pair drifts apart
# again, and two callers were doing exactly this before P52.
# Scoped to a Slider( call and its continuation lines: `.thickness` is also
# Stroke()'s stroke WIDTH (the disclosure chevron uses it), and an unscoped grep
# flagged that — a false positive in this lint, caught by running it.
slider_thick="$(awk '
    /Slider\(/ { inside = 1 }
    inside && /\.thickness *= *[0-9]/ { print FILENAME ":" NR ": " $0 }
    inside && /\);/ { inside = 0 }
' "$SETTINGS" "$REPO_ROOT/system/shade/main.c" || true)"
if [ -n "$slider_thick" ]; then
    zt_fail "a Slider caller hardcodes .thickness, overriding the toolkit default and re-splitting the track/thumb pair" \
        "no .thickness at a Slider call site" "$(printf '%s' "$slider_thick" | head -2 | tr '\n' ' ')"
fi

# --- 4. THE ARITHMETIC, stated as the numbers rather than as a claim --------
# A lint that only checks for the string "Z_PT" passes on Z_PT(3). These are the
# resolved values, so a plausible-looking wrong point value still fails.
zt_expect_eq "$(pt 51)" "94" "Z_PT(51) — the switch track's width in units"
zt_expect_eq "$(pt 31)" "57" "Z_PT(31) — the switch track's height in units"
zt_expect_eq "$(pt 44)" "81" "Z_PT(44) — the touch-target floor in units"
zt_expect_eq "$(pt 28)" "51" "Z_PT(28) — the slider thumb in units"
# The switch must be a majority of the row it sits in — the proportional tell
# that made this findable without any Apple number at all.
row="$(pt 44)"; track="$(pt 31)"
pct=$(( track * 100 / row ))
if [ "$pct" -lt 60 ]; then
    zt_fail "the switch is a minority of its row's height again — iOS's is ~70%, and 40% is what the transcribed-points bug looked like in frame" \
        ">=60% of the row" "${pct}%"
fi
echo "note: switch ${track}u in a ${row}u row = ${pct}% (iOS ~70%)"

zt_done
