#!/usr/bin/env bash
# test_safe_areas_shared — the screen's safe areas are defined once, in points.
#
# WHY THIS IS A TEST AND NOT A CODE REVIEW NOTE. The status bar, the home
# indicator and the keyboard each publish their height to the compositor as a
# layer-surface EXCLUSIVE ZONE, and zcomp shrinks every app window by the sum.
# Four other System-UI surfaces then position themselves against those numbers —
# the shade, the dim scrim and the volume HUD float below "the bar"; the launcher
# starts its grid under it and reserves the keyboard's strip while searching.
# That makes them a contract between five processes.
#
# Before P44 the contract was five copies of BAR_H, two of HOMEBAR_H and two of
# KBD_H, in files that are never opened together. Nothing fails when one moves:
# an overlay just lands on top of the clock, or an app window keeps a strip of
# dead space, and the screenshot still looks like a phone. This is exactly the
# drift P42 fixed for sys.brightness (test_settings_defaults_shared.sh), so it
# gets the same mechanism.
#
# The second half of the test is the unit. These numbers came off Apple's spec
# tables, which are in POINTS, into a layout that is in screen units — the same
# transcription error P43 found in the whole type scale, and they kept it a phase
# longer. So the lint also asserts they are still expressed as Z_PT(points): a
# bare integer here is the bug returning, and it is invisible in every review and
# every screenshot.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

SAFE="$REPO_ROOT/system/common/safe_areas.h"

if [ ! -f "$SAFE" ]; then
    zt_fail "system/common/safe_areas.h is missing" "present" "absent"
    zt_done
fi

# --- 1. The header defines the whole contract ------------------------------
for macro in ZELTO_BAR_H ZELTO_HOMEBAR_H ZELTO_KBD_H ZELTO_KEY_H; do
    if ! grep -q "define $macro" "$SAFE"; then
        zt_fail "safe_areas.h does not define $macro" "defined" "missing"
    fi
done

# --- 2. In POINTS, and at the values that were decided ---------------------
# The pair 44/34 is one device's safe-area spec (iPhone X-class), not two
# guesses; 42 is Apple's key cap, which is also why it clears the 44pt minimum
# touch target once its gaps are added. Pinned so a change is a deliberate one.
pt_of() {   # the point argument of a `#define <name> Z_PT(<n>)`, or ""
    sed -n "s/^#define $1[[:space:]]*Z_PT(\([0-9]*\)).*/\1/p" "$SAFE" | head -1
}
zt_expect_eq "44" "$(pt_of ZELTO_BAR_H)" \
    "ZELTO_BAR_H should be Z_PT(44) — Apple's top safe-area inset, in points"
zt_expect_eq "34" "$(pt_of ZELTO_HOMEBAR_H)" \
    "ZELTO_HOMEBAR_H should be Z_PT(34) — Apple's bottom safe-area inset"
zt_expect_eq "42" "$(pt_of ZELTO_KEY_H)" \
    "ZELTO_KEY_H should be Z_PT(42) — Apple's keyboard cap"

# The keyboard's height is DERIVED from what the strip holds, never declared:
# that is what stops a key growing and silently overflowing the surface.
if ! grep -A 3 "define ZELTO_KBD_H" "$SAFE" | grep -q "ZELTO_KEY_H"; then
    zt_fail "ZELTO_KBD_H should be derived from ZELTO_KEY_H/_GAP/_PAD, not declared" \
            "an expression over the key metrics" "a literal"
fi

# --- 3. Nobody redefines them locally --------------------------------------
# The failure mode this whole file exists for: a second copy of the number.
dupes="$(grep -rnE '^[[:space:]]*#define[[:space:]]+(BAR_H|HOMEBAR_H|KBD_H|KEY_H)\b' \
    "$REPO_ROOT/system" "$REPO_ROOT/samples" 2>/dev/null || true)"
if [ -n "$dupes" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "  local redefinition: $line" >&2
    done <<< "$dupes"
    n="$(printf '%s\n' "$dupes" | grep -c .)"
    zt_fail "a safe area is redefined locally instead of coming from safe_areas.h" \
            "0 sites" "$n site(s)"
fi

# --- 4. No raw number lands in a layer-surface geometry --------------------
# The two fields where one of these numbers is actually spent: the zone a
# surface reserves, and the offset another surface uses to clear it. A literal
# in either is a copy of the contract that no longer has a name.
#
# A ZERO is exempt and is not an oversight: `.exclusive_zone = 0` means "reserve
# nothing" (every overlay says it) and `.margin_top = 0` means "from the edge".
# Neither is a height copied from another process. `.height` is not checked at
# all — Rect(.height = 5) for the home-indicator pill is the same field name on a
# completely different kind of node, and a lint that cries about those gets
# switched off, which is worse than not having it.
raw="$(grep -rnE '\.(margin_top|exclusive_zone)[[:space:]]*=[[:space:]]*[1-9][0-9]*' \
    --include=*.c "$REPO_ROOT/system" 2>/dev/null || true)"
if [ -n "$raw" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "  raw geometry: $line" >&2
    done <<< "$raw"
    n="$(printf '%s\n' "$raw" | grep -c .)"
    zt_fail "a layer surface's geometry is a bare number, not a named safe area" \
            "0 sites" "$n site(s)"
fi

# --- 5. Every surface in the contract reads it from the header -------------
# Catches the lint above being satisfied by simply deleting a usage.
for f in system/bar/main.c system/homebar/main.c system/keyboard/main.c \
         system/shade/main.c system/dim/main.c system/volume/main.c \
         system/launcher/main.c; do
    if ! grep -q '#include "common/safe_areas.h"' "$REPO_ROOT/$f"; then
        zt_fail "$f no longer includes the shared safe areas" \
                'common/safe_areas.h' "not included"
    fi
done

# And the three that OWN a zone must still publish it.
grep -q 'exclusive_zone = ZELTO_BAR_H' "$REPO_ROOT/system/bar/main.c" ||
    zt_fail "the status bar no longer publishes ZELTO_BAR_H as its exclusive zone" \
            "exclusive_zone = ZELTO_BAR_H" "missing"
grep -q 'exclusive_zone = ZELTO_HOMEBAR_H' "$REPO_ROOT/system/homebar/main.c" ||
    zt_fail "the home indicator no longer publishes ZELTO_HOMEBAR_H" \
            "exclusive_zone = ZELTO_HOMEBAR_H" "missing"
grep -q 'z_layer_set_exclusive_zone(app, s->visible ? ZELTO_KBD_H : 0)' \
    "$REPO_ROOT/system/keyboard/main.c" ||
    zt_fail "the keyboard no longer reserves ZELTO_KBD_H while shown" \
            "z_layer_set_exclusive_zone(..., ZELTO_KBD_H : 0)" "missing"

zt_done
