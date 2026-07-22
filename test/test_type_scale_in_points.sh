#!/usr/bin/env bash
# test_type_scale_in_points — every step of the type scale must go through
# Z_TYPE(), and no call site may pass a raw pixel size to Font().
#
# WHY THIS IS A TEST. Zelto's surface coordinate is a raw device pixel (720x1440)
# and every layout metric is written in those pixels; the type scale is written in
# HIG POINTS and converted by Z_TYPE(), because that is how a designer reads a
# ramp. The two units look identical in source — both are bare integers — and the
# conversion factor is 1.85, so getting it wrong is not a crash, a warning or
# anything a compiler can see. It is type at 54% of the size its container was
# built for, uniformly, across every surface.
#
# That is precisely what happened: the ramp was transcribed from the HIG 1:1
# (Body 17, Caption2 11) into a pixel layout and shipped for thirteen phases. It
# survived that long because it was CONSISTENT — nothing clashed, nothing
# overlapped, every screen was quietly under-typed together, and there was no
# single screenshot you could point at and call broken. The only reliable signal
# was the ratio between a step and the box around it, which no human reviews.
#
# So: pin the invariant. Two rules, both cheap, both mechanical.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

UI_H="$REPO_ROOT/sdk/include/zelto/ui.h"

if [ ! -f "$UI_H" ]; then
    zt_fail "sdk/include/zelto/ui.h is missing" "present" "absent"
    zt_done
fi

# --- 1. The converter exists and says what it is worth ---------------------
if ! grep -q '#define Z_TYPE(' "$UI_H"; then
    zt_fail "ui.h does not define Z_TYPE() — the point-to-screen-unit converter" \
        "defined" "missing"
    zt_done
fi

# The ratio is the screen's, not a taste knob: 720 screen units across a 390pt
# design reference. Pinned so a change to it is a deliberate one with a reason.
num="$(sed -n 's/^#define Z_TYPE_NUM[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
den="$(sed -n 's/^#define Z_TYPE_DEN[[:space:]]*\([0-9]*\).*/\1/p' "$UI_H" | head -1)"
zt_expect_eq "185" "$num" "Z_TYPE_NUM should be 185 (720 screen units / 390 design points)"
zt_expect_eq "100" "$den" "Z_TYPE_DEN should be 100"

# --- 2. Every ZFont step goes through it -----------------------------------
# A step written as a bare integer is a point value being used as a pixel value —
# the original bug, exactly. Match `Z_FONT_X = <digits>` and expect no hits.
raw_steps="$(grep -nE '^[[:space:]]*Z_FONT_[A-Z0-9_]+[[:space:]]*=[[:space:]]*[0-9]' \
    "$UI_H" || true)"
if [ -n "$raw_steps" ]; then
    zt_fail "a type-scale step is a bare number instead of Z_TYPE(points) — that is a point value being spent as pixels" \
        "Z_FONT_X = Z_TYPE(<points>)" "$(echo "$raw_steps" | tr '\n' ' ')"
fi

# Every step must actually be one of ours (catches a step added with a hand-
# computed pixel value dressed up as a macro call).
n_steps="$(grep -cE '^[[:space:]]*Z_FONT_[A-Z0-9_]+[[:space:]]*=[[:space:]]*Z_TYPE\(' \
    "$UI_H" || true)"
if [ "${n_steps:-0}" -lt 8 ]; then
    zt_fail "the type scale looks truncated — fewer Z_TYPE() steps than the ramp has" \
        ">=8 steps" "${n_steps:-0}"
fi

# --- 3. No call site invents a size ----------------------------------------
# Font() takes a ZFont, and passing an integer literal (which C will happily
# accept) reintroduces the confusion one label at a time. The correct form always
# names a step.
offenders="$(grep -rnE 'Font\([0-9]' \
    --include=*.c --include=*.h \
    "$REPO_ROOT/system" "$REPO_ROOT/samples" "$REPO_ROOT/script" "$REPO_ROOT/sdk" \
    2>/dev/null || true)"
if [ -n "$offenders" ]; then
    zt_fail "a call site passes a raw number to Font() instead of a Z_FONT_* step" \
        "Font(Z_FONT_*, ...)" "$(echo "$offenders" | head -5 | tr '\n' ' ')"
fi

zt_done
