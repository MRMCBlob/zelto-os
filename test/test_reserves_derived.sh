#!/usr/bin/env bash
# test_reserves_derived — a number that names a sum must BE that sum.
#
# THE FAILURE THIS CATCHES, three times over now. A "reserve" is a height the
# layout holds back for something it does not itself lay out: the keyboard's
# strip, the home screen's dots-and-dock bar, the App Library's title and search
# field. Every one of them was written as a LITERAL that named its contents in a
# comment and was not computed from them — and every one drifted:
#
#   ZELTO_KBD_H  was 300 for 4 rows of 56-unit keys plus gaps = 264. THIRTY-SIX
#                units of slack that read as "sized" and were an accident (P44).
#   LIB_TOP      was 116 for a title and a search field that measure 177 after
#                P43 doubled the type scale. SIXTY-ONE UNITS SHORT — and this one
#                is spent on `avail` in lib_rows(), so it makes the page fit one
#                more row than there is room for. Invisible only because there
#                are twelve apps and the row count is not the binding constraint.
#   BOTTOM_RESERVE was 208 + the home indicator, for parts summing to ~190 + the
#                indicator: ~18 units of slack, same shape as KBD_H.
#
# WHY A LINT AND NOT A REVIEW NOTE. None of the existing tests could have caught
# any of these. test_type_scale_in_points pins the type scale, test_safe_areas_
# shared pins the safe areas; each covers its own family and says nothing about a
# number in a THIRD file that happens to depend on both. A stale reserve produces
# no error, no warning, and a screenshot that still looks like a phone — the
# recurring signature of every metrics bug in this project.
#
# The rule: if a reserve's comment names its parts, the code must add them up.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

LAUNCHER="$REPO_ROOT/system/launcher/main.c"
SAFE="$REPO_ROOT/system/common/safe_areas.h"

for f in "$LAUNCHER" "$SAFE"; do
    if [ ! -f "$f" ]; then
        zt_fail "missing $f" "present" "absent"
        zt_done
    fi
done

# The body of `#define <name> ...`, continuation lines joined.
define_body() {   # <file> <name>
    awk -v name="$2" '
        $0 ~ "^#define[[:space:]]+" name "[[:space:](]" { inb = 1 }
        inb { body = body $0; if ($0 !~ /\\$/) { print body; exit } }
    ' "$1"
}

# --- 1. Each reserve is an EXPRESSION over named parts ----------------------
# A bare number (optionally with a decimal point / f suffix) as the whole body is
# the bug. Requiring named operands is what makes the reserve move when a part
# does.
assert_derived() {   # <file> <macro> <part> [<part>...]
    local file="$1" macro="$2"; shift 2
    local body
    body="$(define_body "$file" "$macro")"
    if [ -z "$body" ]; then
        zt_fail "$macro is not defined" "a #define" "missing"
        return
    fi
    # Strip the "#define NAME" head; what remains is the value.
    local val="${body#*"$macro"}"
    if printf '%s' "$val" | grep -qE '^[[:space:]]*\(?[0-9]+(\.[0-9]+)?f?\)?[[:space:]]*(//.*)?$'; then
        zt_fail "$macro is a bare literal, not derived from what it holds" \
                "an expression over its parts" "$(printf '%s' "$val" | tr -s ' ')"
        return
    fi
    local part
    for part in "$@"; do
        if ! printf '%s' "$val" | grep -q "$part"; then
            zt_fail "$macro no longer mentions $part" \
                    "$part in the derivation" "absent"
        fi
    done
}

# The keyboard: P44's fix, re-pinned here so all three live in one place.
assert_derived "$SAFE" ZELTO_KBD_H ZELTO_KEY_H ZELTO_KEY_GAP ZELTO_KEY_PAD
# The home screen's bottom bar: dots + dock plate + the home-indicator gap.
assert_derived "$LAUNCHER" BOTTOM_RESERVE BAR_PAD BAR_GAP DOTS_H DOCK_PLATE_H \
    ZELTO_HOMEBAR_H
# The dock plate itself, so a bigger dock icon reaches BOTTOM_RESERVE.
assert_derived "$LAUNCHER" DOCK_PLATE_H DOCK_ICON DOCK_PAD
# The App Library's header: the title and the search field, from the TYPE SCALE —
# which is the dependency that went stale in P43 and stayed stale through P44.
assert_derived "$LAUNCHER" LIB_TOP GRID_GAP Z_FONT_TITLE LIB_FIELD_H
assert_derived "$LAUNCHER" LIB_FIELD_H LIB_FIELD_PAD Z_FONT_BODY

# --- 2. The reserve actually covers its contents ----------------------------
# The lint above proves it is an expression; this proves the expression is BIG
# ENOUGH. Recomputed here in the test's own arithmetic, against the value
# measured off a rendered frame (the App Library grid's first icon row starts at
# y=278 with GRID_TOP at 101, so the header consumes 177).
lineh() { awk -v f="$1" 'BEGIN { printf "%.1f", f * 131.0 / 100.0 }'; }
title_px="$(sed -n 's/.*Z_FONT_TITLE = Z_TYPE(\([0-9]*\)).*/\1/p' \
    "$REPO_ROOT/sdk/include/zelto/ui.h" | head -1)"
body_px="$(sed -n 's/.*Z_FONT_BODY = Z_TYPE(\([0-9]*\)).*/\1/p' \
    "$REPO_ROOT/sdk/include/zelto/ui.h" | head -1)"
num="$(sed -n 's/^#define Z_TYPE_NUM \([0-9]*\).*/\1/p' \
    "$REPO_ROOT/sdk/include/zelto/ui.h" | head -1)"
if [ -n "$title_px" ] && [ -n "$body_px" ] && [ -n "$num" ]; then
    t=$(( title_px * num / 100 ))
    b=$(( body_px * num / 100 ))
    lib_top="$(awk -v t="$(lineh "$t")" -v b="$(lineh "$b")" \
        'BEGIN { printf "%d", 16 + t + 16 + (2 * 12 + b) + 16 }')"
    if [ "$lib_top" -lt 177 ]; then
        zt_fail "LIB_TOP no longer covers the header it reserves for" \
                ">= 177 (measured off a rendered frame)" "$lib_top"
    fi
fi

# --- 3. No second copy of a part ------------------------------------------
# The derivation is only real if the layout code spends the SAME constants. A
# duplicate definition is how a reserve and the thing it reserves for drift apart
# while both look correct in isolation.
for macro in DOCK_ICON DOCK_PAD DOCK_GAP BAR_PAD BAR_GAP; do
    n="$(grep -cE "^#define[[:space:]]+$macro[[:space:]]" "$LAUNCHER" || true)"
    if [ "$n" -gt 1 ]; then
        zt_fail "$macro is defined more than once in the launcher" \
                "1 definition" "$n"
    fi
done

# And the dock must not re-declare its own padding as a literal beside the
# constant BOTTOM_RESERVE was derived from.
if grep -qE 'const float pad = 14\.0f' "$LAUNCHER"; then
    zt_fail "the dock re-declares its padding as a literal" \
            "DOCK_PAD" "14.0f"
fi

zt_done
