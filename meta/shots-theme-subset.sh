#!/usr/bin/env bash
# The THEME SUBSET of the screenshot catalogue — the shots P54 re-takes.
#
# TWO JOBS, ONE LIST.
#   Stage 0 needs a BEFORE/AFTER pair to prove the palette seam moved zero
#   pixels; Stage 5 needs a light-mode pass that is a CHOSEN SUBSET rather than
#   a second full catalogue (the P50 precedent — 96 shots twice is not a review,
#   it is an hour nobody reads).
#   The two want the same thing: one frame per DISTINCT COLOUR SITUATION in the
#   OS, not one per screen. So the list is chosen by which tokens a shot is the
#   only witness for:
#
#     01-home-page1       wallpaper + icon captions + the homebar pill
#     10-app-library      MATERIAL_SHEET, the one full-screen frosted surface
#     11-control-center   chips: PRIMARY fill, ON_PRIMARY ink, MATERIAL_REGULAR
#     13-keyboard         SURFACE_4 (the raised key cap) and MATERIAL_EDGE
#     16-lock-screen      MATERIAL_THICK + z_scrim over the wallpaper
#     18-switcher         cards over the wallpaper: SHADOW at ELEV_3
#     19-consent          Z_COLOR_SCRIM behind a PANEL alert
#     20-banner           a notification card floating over the wallpaper
#     24-bar-charging     Z_COLOR_SUCCESS in its INK role
#     30-app-cards        the app-chrome card + BORDER hairlines
#     33-app-fetch        prose: TEXT / TEXT_MUTED / TEXT_FAINT together
#     34-app-settings     the grouped inset list — the most-drawn screen there is
#     40-home-press       Z_COLOR_PRESS, the veil that a light theme deletes
#     91-increase-contrast the three tokens that already had two values
#     95-photos-viewer    a photo on BG, and a toolbar over it
#     96-photos-delete    Z_COLOR_DANGER as a fill, with z_on_fill's ink
#
# Usage:
#   meta/shots-theme-subset.sh out/shots-before     # capture into a named dir
#   THEME=light meta/shots-theme-subset.sh out/shots-light
#
# It does NOT build (the caller decides; never build while a sim loop runs).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
DEST="${1:?usage: shots-theme-subset.sh <dest-dir>}"
case "$DEST" in /*) ;; *) DEST="$REPO_ROOT/$DEST" ;; esac

SUBSET='^(01-home-page1|10-app-library|11-control-center|13-keyboard|16-lock-screen|18-switcher|19-consent|20-banner|24-bar-charging|30-app-cards|33-app-fetch|34-app-settings|40-home-press|91-increase-contrast|95-photos-viewer|96-photos-delete)$'

SKIP_BUILD=1 ONLY="$SUBSET" "$HERE/shots.sh"

mkdir -p "$DEST"
for png in "$REPO_ROOT"/out/shots/*.png; do
    [ -e "$png" ] || continue
    name="$(basename "$png" .png)"
    if [[ "$name" =~ $SUBSET ]]; then cp "$png" "$DEST/"; fi
done
echo "==> $(ls -1 "$DEST"/*.png 2>/dev/null | wc -l) frames in $DEST"
