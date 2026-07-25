#!/usr/bin/env bash
# test_palette_tokens — every colour in the OS comes from the palette, and every
# palette macro goes through the runtime seam.
#
# WHY THIS IS A TEST, and it is not the same argument as test_spacing_scale.
# A wrong GAP is invisible because 1.85x cramped looks like a style. A wrong
# COLOUR is usually the opposite — you see it immediately. Which is exactly why
# the dangerous ones in a theming phase are the invisible kind: a token that
# never got migrated, an ink hardcoded light, a veil that does nothing. Those are
# only wrong in the appearance nobody screenshots, and P54's whole risk is that
# light mode is that appearance.
#
# The five that were still raw when P54 started are the proof of the shape:
#   system/homebar/main.c   #f2f2f7 @ 0xd8 and #ffffff — the home indicator pill
#   system/lock/main.c      #f2f2f7 @ 0x4d — an unfilled passcode dot
#   system/launcher/main.c  #4aa3ff @ 0x30 and @ 0x45 — the drag drop target
# Three of them are the INK at an alpha, drawn directly on the wallpaper. In a
# light appearance every one of them is white-on-white, and every one of them
# would have looked perfect in the dark screenshots anybody took.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

GFX_H="$REPO_ROOT/sdk/include/zelto/gfx.h"
THEME_C="$REPO_ROOT/sdk/src/theme.c"

for f in "$GFX_H" "$THEME_C"; do
    if [ ! -f "$f" ]; then
        zt_fail "the palette is missing a half" "$f" "absent"
        zt_done
    fi
done

# --- 1. Every Z_COLOR_* macro goes through the runtime seam ----------------
# A macro that still expands to a literal z_rgba() is a token that cannot be
# themed, and it is invisible: it keeps its dark value in the light appearance
# and nothing about the code says so. This is the check that would have caught
# P51's four-of-twenty-nine seam being left ungeneralised for three phases.
#
# Z_COLOR_BACKGROUND is a back-compat alias for Z_COLOR_BG and expands to the
# other macro, which is the seam by transitivity.
#
# THE VALUE IS FIELD 3, and testing it positionally is the whole trick. The first
# version of this rule matched the line against "contains z_token(...) OR
# contains Z_COLOR_..." — and every one of these lines contains a Z_COLOR_ name,
# because that is what a #define of a Z_COLOR_ macro looks like. The alias
# carve-out excused all 29, the rule could not fail, and it took the negative
# test to say so. An absence proves nothing until something has been seen to
# fail.
stuck="$(awk '/^#define[[:space:]]+Z_COLOR_[A-Z0-9_]+[[:space:]]/ {
        if ($3 !~ /^z_token\(Z_TOKEN_[A-Z0-9_]+\)$/ && $3 !~ /^Z_COLOR_[A-Z0-9_]+$/) {
            print $2 " = " $3
        }
    }' "$GFX_H")"
if [ -n "$stuck" ]; then
    n="$(printf '%s\n' "$stuck" | wc -l | tr -d ' ')"
    zt_fail "a palette token is still a compile-time constant — it cannot follow the appearance, and it will be wrong only in the theme nobody screenshots" \
        "z_token(Z_TOKEN_*)" "$n: $(printf '%s\n' "$stuck" | head -3 | tr '\n' ' ')"
else
    echo "note: every Z_COLOR_* macro resolves through z_token()"
fi

# --- 1b. The enum and the macros are the same set --------------------------
# A Z_TOKEN_* with no macro is a value nothing can draw; a macro naming a token
# the enum does not have would not compile, so only the first direction needs
# checking. (Z_TOKEN_COUNT is the sentinel, not a colour.)
for tok in $(grep -oE '^[[:space:]]+Z_TOKEN_[A-Z0-9_]+' "$GFX_H" | tr -d ' ' \
             | sed 's/,$//' | grep -v '^Z_TOKEN_COUNT$'); do
    if ! grep -qE "z_token\($tok\)" "$GFX_H"; then
        zt_fail "$tok is in the enum but no Z_COLOR_* macro names it — a colour nothing can draw" \
            "a macro expanding to z_token($tok)" "none"
    fi
done

# --- 2. No raw colour outside the palette ---------------------------------
#
# THE CARVE-OUTS, each with its reason:
#
#   z_rgba(0, 0, 0, 0)   the FIXED-GAP idiom (a Frame holding a transparent
#     Rect — see the traps list in CLAUDE.md: a Spacer is not a fixed gap). It is
#     the absence of a colour, so it has nothing to theme.
#   the launcher's built-in wallpaper and its scrim bands
#     A wallpaper is an IMAGE. The gradient's stops and the caption scrim's
#     per-band alpha are picture, not chrome, and theming them would mean
#     retinting a photograph. Both build their ZColor from variables, so the scan
#     below does not see them — this note records that that is deliberate.
#   samples/hello
#     A SAMPLE APP with a palette of its own, which is a thing the SDK must let a
#     third-party app do. It is not System UI and is not scanned.
#     (It will look wrong in a light appearance. That is the sample's business
#     and is worth its own note in the SDK docs, not a lint failure here.)
#
# The rule is a HEX LITERAL: every one of the five that P54 found was written
# `z_rgba(0x..`, because that is how a person writes a colour they read off a
# design. A decimal triple that is not 0,0,0 is caught too.
scan_dirs="$REPO_ROOT/sdk $REPO_ROOT/system"
scan_re='z_rgba\([[:space:]]*(0x[0-9a-fA-F]|[0-9])'
hits="$(grep -rnE "$scan_re" --include='*.c' --include='*.h' $scan_dirs 2>/dev/null \
    | grep -vE '^[^:]*/sdk/include/zelto/gfx\.h:' \
    | grep -vE '^[^:]*/sdk/src/theme\.c:' \
    | grep -vE 'z_rgba\([[:space:]]*0,[[:space:]]*0,[[:space:]]*0,' \
    | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' \
    || true)"
if [ -n "$hits" ]; then
    n="$(printf '%s\n' "$hits" | wc -l | tr -d ' ')"
    zt_fail "a colour is written as a literal outside the palette — it cannot follow the appearance, and a light ink baked in here is white-on-white the moment the OS has a light theme" \
        "a Z_COLOR_* token (or z_fade of one)" "$n site(s): $(printf '%s\n' "$hits" | head -4 | tr '\n' ' ')"
else
    echo "note: no raw colour literals outside the palette and its carve-outs"
fi

# --- 3. POSITIVE CONTROL for the scan above -------------------------------
# Rule 2 asserts an ABSENCE, and a broken regex, a bad path and a clean tree all
# report the same thing. So run the identical pattern over a file that is KNOWN
# to contain it — and over the transparent form, which must NOT match, or the
# carve-out is doing the work of the rule.
ctl="$(mktemp)"
printf '%s\n' '    ZColor idle = z_rgba(0xf2, 0xf2, 0xf7, 0xd8);' \
              '    Rect(.color = z_rgba(0, 0, 0, 0));' > "$ctl"
ctl_hits="$(grep -rnE "$scan_re" "$ctl" 2>/dev/null || true)"
ctl_kept="$(printf '%s\n' "$ctl_hits" \
    | grep -vE 'z_rgba\([[:space:]]*0,[[:space:]]*0,[[:space:]]*0,' || true)"
rm -f "$ctl"
case "$ctl_kept" in
    *0xf2*) echo "note: positive control — the scan does see a raw #f2f2f7" ;;
    *) zt_fail "the raw-colour scan cannot see a raw colour — rule 2's clean result proves nothing" \
           "the control line matches" "no match" ;;
esac
if printf '%s\n' "$ctl_kept" | grep -q 'z_rgba(0, 0, 0, 0)'; then
    zt_fail "the transparent-gap carve-out does not exclude z_rgba(0,0,0,0) — rule 2 would fail on every fixed gap in the OS" \
        "excluded" "still matched"
else
    echo "note: negative control — the fixed-gap idiom is not flagged"
fi

zt_done
