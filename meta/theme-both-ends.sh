#!/usr/bin/env bash
# Shoot the theme subset in BOTH appearances and check the appearance reached
# every surface in it.
#
# WHY THIS IS A SCRIPT AND NOT A TEST. It boots the shell 34 times and takes ~6
# minutes, which is a quarter of the whole suite for one claim — and the claim is
# a DESIGN review, which ends in a person looking at frames. What it automates is
# the half a person is bad at: noticing that one surface out of seventeen came
# back identical in both appearances.
#
# THAT IS THE FAILURE THIS PHASE IS MOST LIKELY TO SHIP. A wrong colour is
# usually visible; a surface the theme never reached is invisible, because nobody
# screenshots the appearance they are not using. A frame whose light and dark
# versions are the same is exactly that surface, and it is one subtraction away.
#
# Usage:
#   meta/theme-both-ends.sh              # build, shoot both, print the table
#   SKIP_BUILD=1 meta/theme-both-ends.sh
#   COMPARE_ONLY=1 meta/theme-both-ends.sh   # re-read what is already on disk
#
# COMPARE_ONLY exists so the check below can be NEGATIVE-TESTED without six
# minutes of booting: copy a dark frame over its light twin and this must fail
# naming it. An assertion nobody has watched fail is not an assertion.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build-host}"
DARK="$REPO_ROOT/out/shots-p54-dark"
LIGHT="$REPO_ROOT/out/shots-p54-light"

if [ "${COMPARE_ONLY:-0}" != "1" ]; then
    if [ "${SKIP_BUILD:-0}" != "1" ]; then
        ninja -C "$BUILD" || exit 1
    fi
    echo "==> dark"
    bash "$HERE/shots-theme-subset.sh" "$DARK" >/dev/null || exit 1
    echo "==> light"
    THEME=light bash "$HERE/shots-theme-subset.sh" "$LIGHT" >/dev/null || exit 1
fi

echo
printf '%-24s %10s %10s %9s  %s\n' shot dark light delta verdict
fail=0
for a in "$DARK"/*.png; do
    name="$(basename "$a" .png)"
    b="$LIGHT/$name.png"
    if [ ! -e "$b" ]; then
        printf '%-24s %10s %10s %9s  %s\n' "$name" - - - "MISSING in light"
        fail=1
        continue
    fi
    dm="$(python3 "$HERE/pngdiff.py" "$a" | sed -n 's/.*mean RGB = \([0-9.]*\) .*/\1/p')"
    lm="$(python3 "$HERE/pngdiff.py" "$b" | sed -n 's/.*mean RGB = \([0-9.]*\) .*/\1/p')"
    d="$(python3 "$HERE/pngdiff.py" "$a" "$b" \
         | sed -n 's/.*mean |delta| = \([0-9.]*\).*/\1/p')"
    # THE BAR IS DELIBERATELY LOW. A frame that is mostly WALLPAPER legitimately
    # moves very little — the picture does not follow the appearance, and P54
    # stage 4 made the marks on it follow the PICTURE rather than the palette. So
    # this is not looking for a big change, it is looking for a ZERO one.
    verdict=ok
    if awk "BEGIN{exit !($d < 0.5)}"; then
        verdict="THE APPEARANCE DID NOT REACH THIS SURFACE"
        fail=1
    elif awk "BEGIN{exit !($d < 2.0)}"; then
        verdict="small — confirm it is mostly wallpaper"
    fi
    printf '%-24s %10s %10s %9s  %s\n' "$name" "$dm" "$lm" "$d" "$verdict"
done

echo
if [ "$fail" -ne 0 ]; then
    echo "!! at least one surface did not follow the appearance"
    exit 1
fi
echo "every surface in the subset followed the appearance"
echo "contact sheets: $DARK and $LIGHT"
