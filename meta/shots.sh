#!/usr/bin/env bash
# Zelto OS screenshot harness — capture every System-UI view + state as a labelled
# PNG under out/shots/, then stitch a reviewable HTML contact sheet (out/shots/
# index.html). Built on meta/run-sim.sh: each shot boots the whole shell headless
# on WSLg with the right env / seeded settings / spawned overlays, then grim grabs
# a frame (see docs/tooling/simulator.md).
#
# Usage:
#   meta/shots.sh               # build once, then capture the whole catalogue
#   SKIP_BUILD=1 meta/shots.sh  # reuse build-host/ artifacts (fast re-run)
#   ONLY=home meta/shots.sh     # only shots whose name matches the substring ONLY
#
# HOW EACH STATE IS REACHED. Every shot is one boot of the sim with per-shot env.
# Two kinds:
#   - DETERMINISTIC (the vast majority): reached purely by an env test-hook
#     (grep ZELTO_ in the relevant system/*/main.c) or by pre-seeding the brokered
#     settings store ($ZELTO_DATA_DIR/settings.conf, TAB key\tvalue, loaded by
#     zsysd at boot). No input injection, so they are byte-stable across runs.
#   - SPAWN-DRIVEN: an extra binary is launched into the running sim (an app to
#     populate Recents, the consent modal, the pinger auto-post). Still no pointer
#     input — just process spawns run-sim.sh does for us (SIM_EXTRA / SIM_RECENTS /
#     SIM_CONSENT / SIM_APP + the auto-post/auto-allow hooks).
# There are NO flaky pointer-swipe shots here: every state that a real user reaches
# by a gesture (open the drawer, pull the shade, lift a home icon, raise the
# keyboard, pop the volume HUD) is instead reached by a deterministic test-hook
# added to that surface, so the catalogue reproduces exactly every run.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build-host}"
OUT="$REPO_ROOT/out/shots"
TMP="${SHOTS_TMP:-/tmp/zelto-shots}"
ONLY="${ONLY:-}"

mkdir -p "$OUT"
rm -rf "$TMP"; mkdir -p "$TMP"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> building host (x86_64)"
    [ -d "$BUILD" ] || meson setup "$BUILD" "$REPO_ROOT"
    ninja -C "$BUILD"
fi

# Gallery manifest rows ("name|caption"), filled as we go.
GALLERY=()
# Optional settings.conf body for the NEXT shot (TAB-separated key\tvalue lines),
# set by the caller just before run_shot and cleared by it.
SEED=""

# run_shot NAME CAPTION DELAY [ENV=VAL ...]
#   NAME     out/shots/NAME.png + gallery id
#   CAPTION  human description (how the state is reached)
#   DELAY    seconds to let the sim settle before grim (SHOT_DELAY)
#   ENV=VAL  extra environment for run-sim.sh (test-hooks, SIM_APP, SIM_EXTRA, ...)
# Pre-seed the brokered settings store by setting $SEED before the call.
run_shot() {
    local name="$1" caption="$2" delay="$3"; shift 3
    if [ -n "$ONLY" ] && [[ "$name" != *"$ONLY"* ]]; then SEED=""; return 0; fi

    local dir="$TMP/$name"
    local data="$dir/data" xdg="$dir/xdg"
    rm -rf "$dir"; mkdir -p "$data/apps" "$xdg"
    # A fresh data dir per shot => a clean home layout + prefs (no cross-shot
    # bleed). Seed the settings store before boot when the state needs it.
    if [ -n "$SEED" ]; then printf '%b' "$SEED" > "$data/settings.conf"; fi

    local png="$OUT/$name.png"
    rm -f "$png"
    echo "==> $name  (${delay}s)  — $caption"
    env "$@" \
        SKIP_BUILD=1 HEADLESS=1 SHOT="$png" SHOT_DELAY="$delay" \
        ZELTO_DATA_DIR="$data" SIM_RUNTIME_DIR="$xdg" \
        "$REPO_ROOT/meta/run-sim.sh" >"$dir/log" 2>&1 || true
    if [ -f "$png" ]; then echo "    ok"; else echo "    !! MISSING (see $dir/log)"; fi
    GALLERY+=("$name|$caption")
    SEED=""
}

# ===========================================================================
# HOME SCREEN (the launcher's bento grid — env hooks in system/launcher/main.c)
# ===========================================================================
run_shot 01-home-page1 "Home, page 1 (default layout: 3 widgets + 4 app icons)" 6

# Page 2 / carousel: force tiny 2-row pages so the default set overflows past one
# page, then settle on page 2 / freeze a flip mid-slide / show the overflow.
run_shot 02-home-page2 "Home, page 2 (ROWS_PER_PAGE=2 forces overflow; HOME_PAGE=1)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_PAGE=1
run_shot 03-home-flip-mid "Carousel flip mid-slide (page 0->1 frozen 7 frames in)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_PAGE_FROM=0 ZELTO_HOME_PAGE=1 \
    ZELTO_HOME_ANIM_FRAMES=7
run_shot 04-home-overflow "Overflowing layout + page dots (ROWS_PER_PAGE=2)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2

# Rearrange mode + direct manipulation.
run_shot 05-home-rearrange "Rearrange mode: raster, remove badges, Done bar" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3
run_shot 06-home-ghost-lift "Rearrange: an icon lifted as a ghost under the finger" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=760
run_shot 07-home-reflow-mid "Rearrange: neighbours reflowing mid-drag (frozen 6 frames)" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=300 ZELTO_HOME_ANIM_FRAMES=6
run_shot 08-home-landing-mid "Rearrange: released ghost mid snap-back (frozen 6 frames)" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=300 ZELTO_HOME_ANIM_FRAMES=6 \
    ZELTO_HOME_LANDING=1
run_shot 09-home-crosspage "Rearrange: ghost held in the right edge gutter on page 1" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_REARRANGE=1 ZELTO_HOME_PAGE=1 \
    ZELTO_HOME_HELD=4 ZELTO_HOME_GHOST_X=690 ZELTO_HOME_GHOST_Y=300

# App drawer (slid up).
run_shot 10-app-drawer "App drawer slid up (all installed apps)" 6 \
    ZELTO_HOME_DRAWER=1

# Press feedback (P31): freeze the global press spring over a tappable node so the
# touch-down highlight veil paints on a still frame (ZELTO_PRESS_X/Y in surface px,
# ZELTO_PRESS_AMT the spring value). The launcher is the full-screen surface here.
run_shot 40-home-press "Home app icon pressed (touch-down highlight veil)" 6 \
    ZELTO_PRESS_X=447 ZELTO_PRESS_Y=460
run_shot 41-settings-press "Settings: Wi-Fi toggle pressed (veil over PRIMARY chip)" 8 \
    SIM_APP=zelto-settings ZELTO_PRESS_X=611 ZELTO_PRESS_Y=101

# ===========================================================================
# SYSTEM OVERLAYS (shade / volume / keyboard / lock / recents / consent / banner)
# ===========================================================================
run_shot 11-shade-open "Pull-down shade: quick settings + notifications (open)" 6 \
    ZELTO_SHADE_OPEN=1
SEED='sys.volume\t7\n' run_shot 12-volume-hud "Volume rocker HUD (shown at level 7)" 6 \
    ZELTO_VOLUME_SHOW=1

run_shot 13-keyboard "On-screen keyboard (QWERTY) over Notepad" 8 \
    ZELTO_KBD_SHOW=1 SIM_APP=zelto-notepad
run_shot 14-keyboard-symbols "On-screen keyboard, symbols layer" 6 \
    ZELTO_KBD_SHOW=1 ZELTO_KBD_SYMBOLS=1

# Lock lifecycle: seed the broker so zelto-lock arms short idle timeouts; headless
# has no seat input, so it idles from boot into each phase deterministically.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t2\nsys.idle_lock_s\t999\nsys.idle_off_s\t9999\n' \
    run_shot 15-lock-dimmed "Pre-lock dim scrim (idle past idle_dim_s)" 7
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\n' \
    run_shot 16-lock-screen "Lock screen: clock + swipe-up-to-unlock" 9
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t6\n' \
    run_shot 17-lock-off "Screen-off scrim (idle past idle_off_s while locked)" 11

# Recents (task switcher): three apps left running, then the overlay on top.
run_shot 18-recents "Recents overview (3 running apps)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1
# Permission consent modal (spawned standalone with app_id + perm).
run_shot 19-consent "Permission consent dialog (Allow / Deny modal)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications"
# Notification banner: pinger auto-posts, consent auto-allows -> shade heads-up.
run_shot 20-banner "Heads-up notification banner (auto-posted + auto-granted)" 10 \
    SIM_APP=zelto-pinger ZELTO_PINGER_POST=1 ZELTO_CONSENT_AUTO=allow

# ===========================================================================
# STATUS BAR STATES (seed the brokered sys.* the bar reads; home behind it)
# ===========================================================================
SEED='sys.wifi\t1\nsys.airplane\t0\nsys.brightness\t5\n' \
    run_shot 21-bar-wifi-bright "Status bar: Wi-Fi on, brightness high" 6
SEED='sys.wifi\t0\nsys.airplane\t1\nsys.brightness\t2\n' \
    run_shot 22-bar-airplane "Status bar: airplane mode, brightness low" 6
SEED='sys.battery_pct\t8\nsys.battery_charging\t0\n' \
    run_shot 23-bar-lowbatt "Status bar: low battery (8%)" 6 ZELTO_FAKE_BATTERY=0
SEED='sys.battery_pct\t64\nsys.battery_charging\t1\n' \
    run_shot 24-bar-charging "Status bar: charging (64%)" 6 ZELTO_FAKE_BATTERY=0

# ===========================================================================
# SHIPPED APPS (auto-launched via SIM_APP)
# ===========================================================================
run_shot 30-app-cards    "App: Cards"    8 SIM_APP=zelto-cards
run_shot 31-app-notes    "App: Notes"    8 SIM_APP=zelto-notes
run_shot 32-app-notepad  "App: Notepad"  8 SIM_APP=zelto-notepad
run_shot 33-app-fetch    "App: Fetch"    8 SIM_APP=zelto-fetch
run_shot 34-app-settings "App: Settings" 8 SIM_APP=zelto-settings
run_shot 35-app-store    "App: Store"    8 SIM_APP=zelto-store
run_shot 36-app-hello    "App: Rows (SDK sample)" 8 SIM_APP=zelto-hello
run_shot 37-app-pinger   "App: Pinger"   8 SIM_APP=zelto-pinger

# ===========================================================================
# CONTACT SHEET (self-contained HTML gallery — no ImageMagick dependency)
# ===========================================================================
INDEX="$OUT/index.html"
{
    echo '<!doctype html><meta charset="utf-8"><title>Zelto OS — shot catalogue</title>'
    echo '<style>'
    echo 'body{background:#0b0f14;color:#e6ecf2;font:14px/1.4 system-ui,sans-serif;margin:0;padding:24px}'
    echo 'h1{font-size:20px;margin:0 0 4px}p.sub{color:#93a1b0;margin:0 0 20px}'
    echo '.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:18px}'
    echo '.card{background:#141a21;border:1px solid #3a4552;border-radius:12px;overflow:hidden}'
    echo '.card img{width:100%;display:block;background:#000}'
    echo '.card .cap{padding:10px 12px}'
    echo '.card .id{color:#4aa3ff;font-weight:600;font-size:12px}'
    echo '.card .desc{color:#93a1b0;font-size:12px;margin-top:2px}'
    echo '</style>'
    echo "<h1>Zelto OS — screenshot catalogue</h1>"
    echo "<p class=sub>$(date -u '+%Y-%m-%d %H:%MZ') · $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo '?') · ${#GALLERY[@]} states</p>"
    echo '<div class=grid>'
    for row in "${GALLERY[@]}"; do
        name="${row%%|*}"; cap="${row#*|}"
        [ -f "$OUT/$name.png" ] || continue
        echo "<div class=card><img src=\"$name.png\" loading=lazy>"
        echo "<div class=cap><div class=id>$name</div><div class=desc>$cap</div></div></div>"
    done
    echo '</div>'
} > "$INDEX"

echo
echo "==> wrote $(ls "$OUT"/*.png 2>/dev/null | wc -l) shots + $INDEX"
