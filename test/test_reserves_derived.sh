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

# The returned expression of `static float <name>(...) { return ...; }`.
#
# Two reserves stopped being macros in P46, and for a reason that matters to this
# lint: their parts are no longer compile-time constants. A line of text is as
# tall as the FACE says, so the App Library's header can only be derived once the
# app exists. That is the fix — the estimate the macro needed is gone — so the
# rule has to follow the reserve into a function rather than let it out of scope.
# The `return` and the `;` are stripped so what comes back is the EXPRESSION
# alone — otherwise the bare-literal check below never fires for a function, the
# word "return" being enough to make `return 177.0f;` look like an expression.
return_body() {   # <file> <name>
    awk -v name="$2" '
        $0 ~ "^static float " name "\\(" { inb = 1; next }
        inb {
            body = body $0
            if ($0 ~ /;/) {
                sub(/^[[:space:]]*return[[:space:]]*/, "", body)
                sub(/;.*$/, "", body)
                print body
                exit
            }
        }
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
    assert_expr "$file" "$macro" "$body" "$@"
}

# The same rule for a reserve that is a function.
assert_derived_fn() {   # <file> <fn> <part> [<part>...]
    local file="$1" fn="$2"; shift 2
    local body
    body="$(return_body "$file" "$fn")"
    assert_expr "$file" "$fn" "$body" "$@"
}

assert_expr() {   # <file> <name> <body> <part>...
    local file="$1" macro="$2" body="$3"; shift 3
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
assert_derived_fn "$LAUNCHER" lib_top GRID_GAP Z_FONT_TITLE lib_field_h
assert_derived_fn "$LAUNCHER" lib_field_h LIB_FIELD_PAD Z_FONT_BODY

# --- 2. Nothing ESTIMATES a line height -------------------------------------
# What section 2 used to be: a recomputation of LIB_TOP here in the test's own
# arithmetic, checked against 177 units measured by hand off a screenshot. It had
# to exist because the launcher's derivation ran through
# `LINE_H(font) = font * 1.31` — an estimate fitted to one face at one size — so
# "derived from its parts" did not imply "the right size", and the test needed a
# second, independent copy of the estimate to catch the drift.
#
# P46 deleted the estimate. lib_top() now calls z_line_height(), which is the
# SAME z_text_measure() call layout makes for the Text node it is reserving for,
# so reserve and content cannot disagree by construction and there is nothing
# left for an independent recomputation to disagree with. Verified once, on a
# 720x1440 sim boot: the launcher logged `library header: top=177.0` and
# ZELTO_PROBE_TAPS put the grid's first icon row at y=278 = GRID_TOP 101 + 177,
# exactly. (The old estimate came to 179.4 — 2.4 units of phantom reserve.)
#
# So what is left to guard is that the estimate does not come BACK. Any constant
# ratio applied to a font size is the bug, whatever it is called.
#
# Comments are stripped first — this file DESCRIBES the deleted estimate, and a
# lint that fires on its own explanation of why the estimate is gone teaches the
# next person to delete the explanation.
CODE="$(sed 's,//.*,,' "$LAUNCHER")"
EST_RE='(LINE_H|[0-9]+\.[0-9]+f?[[:space:]]*[*/][[:space:]]*(\(float\))?[[:space:]]*Z_FONT_|Z_FONT_[A-Z0-9_]+[[:space:]]*\*[[:space:]]*[0-9]+\.[0-9]+)'
if printf '%s\n' "$CODE" | grep -qE "$EST_RE"; then
    zt_fail "the launcher estimates a line height again instead of measuring it — z_line_height() asks the face, and the face is the only thing that knows" \
            "no constant ratio applied to a font size" \
            "$(printf '%s\n' "$CODE" | grep -nE "$EST_RE" | head -1)"
fi
# And the derivation must go through the measurement, not around it.
for fn in lib_top lib_field_h; do
    if ! return_body "$LAUNCHER" "$fn" | grep -q 'z_line_height'; then
        zt_fail "$fn() does not measure its text" "z_line_height(...)" \
                "$(return_body "$LAUNCHER" "$fn" | tr -s ' ')"
    fi
done

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
