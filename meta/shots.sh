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
# by a gesture (page to the App Library, pull the shade, lift a home icon, raise the
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

# Prune PNGs from shots this file no longer defines. Without this a shot that is
# deleted or renamed leaves its last capture on disk forever, and the next person
# to open out/shots/ reviews a surface that no longer exists. (The contact sheet
# only lists what ran, so the stale file is invisible there — which is worse.)
if [ -z "$ONLY" ]; then
    for png in "$OUT"/*.png; do
        [ -e "$png" ] || continue
        name="$(basename "$png" .png)"
        # NB: not anchored to the line start — a shot may be invoked as
        # `SEED='...' run_shot 12-volume-hud ...` on one line, and anchoring here
        # deletes a live shot's capture (which then silently reappears only
        # because the same run re-takes it, so the bug hides itself).
        grep -q "run_shot $name " "$HERE/shots.sh" || {
            echo "==> pruning stale shot $name"; rm -f "$png"; }
    done
fi

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
# Optional "<manifest.app>:<entry.js>" for the NEXT shot: package it as a signed
# .zap and install it into that shot's data dir before booting, so the shot can
# photograph an app that exists ONLY because it was installed.
ZAP=""

# run_shot NAME CAPTION DELAY [ENV=VAL ...]
#   NAME     out/shots/NAME.png + gallery id
#   CAPTION  human description (how the state is reached)
#   DELAY    seconds to let the sim settle before grim (SHOT_DELAY)
#   ENV=VAL  extra environment for run-sim.sh (test-hooks, SIM_APP, SIM_EXTRA, ...)
# Pre-seed the brokered settings store by setting $SEED before the call; install a
# script package first by setting $ZAP.
run_shot() {
    local name="$1" caption="$2" delay="$3"; shift 3
    # Register the shot in the gallery BEFORE the ONLY filter. The contact sheet is
    # the review artifact, and it lists whatever PNGs are on disk (missing ones are
    # skipped when it is written) — so a filtered re-run of one shot must still
    # emit the WHOLE sheet. Registering after the filter meant `ONLY=x meta/shots.sh`
    # rewrote index.html with a single card and threw the catalogue away.
    GALLERY+=("$name|$caption")
    if [ -n "$ONLY" ] && [[ "$name" != *"$ONLY"* ]]; then SEED=""; ZAP=""; return 0; fi

    local dir="$TMP/$name"
    local data="$dir/data" xdg="$dir/xdg"
    rm -rf "$dir"; mkdir -p "$data/apps" "$xdg"
    # A fresh data dir per shot => a clean home layout + prefs (no cross-shot
    # bleed). Seed the settings store before boot when the state needs it.
    #
    # BRIGHTNESS. sys.brightness defaults to 3 of 5, and zelto-dim then paints a
    # 96/255 BLACK scrim over everything below the status bar — so an unseeded shot
    # photographs the whole OS at 62% brightness and every colour in the catalogue
    # comes out muddy (this silently applied to every shot ever taken here). The
    # catalogue is a DESIGN review, so shoot at full brightness by default; a shot
    # that is specifically about dimming seeds sys.brightness itself, and its own
    # SEED wins (this only fills in the key when the caller did not set it).
    if [ -n "$SEED" ] && [[ "$SEED" != *"sys.brightness"* ]]; then
        SEED="$SEED\nsys.brightness\t5"
    elif [ -z "$SEED" ]; then
        SEED="sys.brightness\t5"
    fi
    printf '%b\n' "$SEED" > "$data/settings.conf"

    # ZAP=<manifest>:<entry.js> — package and INSTALL a script app into this
    # shot's data dir before booting it, so the boot that gets photographed knows
    # the app only from what the installer left on disk (the app is nowhere in the
    # image). The install must happen after the wipe above, hence here.
    if [ -n "$ZAP" ]; then
        local zap_manifest="${ZAP%%:*}" zap_entry="${ZAP#*:}"
        local zap_file="$dir/app.zap"
        "$REPO_ROOT/meta/mkzap.sh" "$zap_manifest" "$zap_entry" "$zap_file" \
            >"$dir/zap.log" 2>&1
        ZELTO_DATA_DIR="$data" \
        ZELTO_TRUSTED_KEY="$REPO_ROOT/meta/keys/trusted.pub" \
        ZELTO_SCRIPT_BIN="$BUILD/script/zelto-script" \
            "$BUILD/system/installer/zelto-install" "$zap_file" \
            >>"$dir/zap.log" 2>&1 \
            || echo "    !! install failed (see $dir/zap.log)"
    fi

    local png="$OUT/$name.png"
    rm -f "$png"
    echo "==> $name  (${delay}s)  — $caption"
    # Boot, and if the boot did not produce a usable frame, boot again.
    #
    # run-sim.sh now exits non-zero when the capture failed or came back blank
    # (rather than exiting 0 with no PNG), so a bad boot is detectable here — and
    # a catalogue with silent holes in it is a design review nobody can trust. One
    # retry is enough for a transient; a shot that fails twice is a real defect in
    # that surface and should be read as one, so it is reported and left missing.
    local attempt
    for attempt in 1 2; do
        env "$@" \
            SKIP_BUILD=1 HEADLESS=1 SHOT="$png" SHOT_DELAY="$delay" \
            ZELTO_DATA_DIR="$data" SIM_RUNTIME_DIR="$xdg" \
            "$REPO_ROOT/meta/run-sim.sh" >"$dir/log.$attempt" 2>&1 || true
        if [ -s "$png" ]; then break; fi
        [ "$attempt" = 1 ] && echo "    .. no frame; re-booting this shot once"
    done
    cp -f "$dir/log.$attempt" "$dir/log" 2>/dev/null || true
    if [ -s "$png" ]; then
        [ "$attempt" = 1 ] && echo "    ok" || echo "    ok (on retry $attempt)"
    else
        echo "    !! MISSING after $attempt boots (see $dir/log)"
    fi
    SEED=""
    ZAP=""
}

# ===========================================================================
# HOME SCREEN (the launcher's bento grid — env hooks in system/launcher/main.c)
# ===========================================================================
run_shot 01-home-page1 "Home, page 1 (default seed: 3 widgets + 6 app icons)" 6

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

# App Library: the LAST page of the home carousel (the swipe-up drawer is gone).
# Reached by paging, so the shot just settles the carousel on that page.
run_shot 10-app-library "App Library: last carousel page (every installed app)" 6 \
    ZELTO_HOME_PAGE=1
# Searching: the field is focused, so the system keyboard is up, the page has
# inset itself by KBD_H (the compositor does not shrink the home window) and the
# dots + dock have stood down.
run_shot 10a-app-library-search "App Library search: filtered, keyboard raised" 7 \
    ZELTO_HOME_PAGE=1 ZELTO_HOME_SEARCH=not

# Press feedback (P31): freeze the global press spring over a tappable node so the
# touch-down highlight veil paints on a still frame (ZELTO_PRESS_X/Y in surface px,
# ZELTO_PRESS_AMT the spring value). The launcher is the full-screen surface here.
#
# P44: +41 on y. The launcher gets the whole output, so these are SCREEN
# coordinates -- but the grid starts at GRID_TOP = ZELTO_BAR_H + 20, and the safe
# areas moved the status bar from 40 units to 81. Every icon on the home screen
# dropped by exactly that 41. Measured on the re-shot frame: the Notepad tile now
# spans y 449..549, so 501 is its centre.
run_shot 40-home-press "Home app icon pressed (touch-down highlight veil)" 6 \
    ZELTO_PRESS_X=447 ZELTO_PRESS_Y=501
# The Airplane Mode SWITCH — row 1 of the first card after P41's regrouping.
#
# TWO traps here, both of which photograph as "nothing happened":
#  1. It must be an OFF switch. The press veil is a WHITE wash and an ON switch
#     wears the near-white Z_COLOR_PRIMARY track, so a press on Wi-Fi (which
#     defaults ON) lands white-on-white. Shot 61 aims at the OFF Lock toggle for
#     exactly this reason.
#  2. ZELTO_PRESS_X/Y are SURFACE-local, and an app's surface starts BELOW the
#     40px status bar — unlike the launcher (shot 40), which zcomp hands the whole
#     output. So an app-targeted press is screen_y - BAR_H. Reading a y off a
#     screenshot and pasting it here silently misses by 40px.
#
# P42 retarget: Settings is a DRILL-DOWN now, so its first card no longer holds
# the Airplane switch — it holds four detail rows. Aiming at the "Network" row
# keeps both traps satisfied: a list row is dark SURFACE, so the white veil shows
# on it (unlike the near-white track of an ON switch).
#
# P43 retarget, and the THIRD trap: a coordinate aimed at a row is really aimed at
# everything ABOVE that row. The type rescale grew the "Settings" large title from
# 40px to 74px, which pushed the whole card down 38px, and 140 -- correct in P42 --
# landed in the gap just above the card. It photographed a perfectly plausible
# Settings screen with no veil on it at all, and the only thing that moved between
# it and the baseline was the status-bar clock ticking over.
#
# P44 retarget, and the FOURTH time this shot has moved. BOTH terms changed:
# ZELTO_BAR_H went 40 -> 81 (so the app surface starts 41 lower on screen) and a
# settings row went 76 -> 81 (ROW_H is the row's TOTAL height now, and it is
# Apple's 44pt converted rather than 44 spent as pixels). MEASURED off a re-shot
# frame rather than derived: the card's top edge is screen y=224 and its rows are
# exactly 81 tall (separators at 305, 387, 469), so the Network row is 224..305,
# centre 264 on screen and 264-81 = 183 in the app's surface.
#
# Note how close that is to P43's 181 -- and that 181 would now land at screen
# 221, three pixels ABOVE the card, photographing no veil at all. A number that is
# nearly right is exactly how this shot has rotted every phase since P41. The rule
# is unchanged and is the only thing that catches it: MEASURE, never look.
#   meta/pngdiff.py 34-app-settings.png 41-settings-press.png --expect-box 40 224 640 81
run_shot 41-settings-press "Settings: a detail row pressed (touch-down veil)" 8 \
    SIM_APP=zelto-settings ZELTO_PRESS_X=360 ZELTO_PRESS_Y=183

# ===========================================================================
# SYSTEM OVERLAYS (shade / volume / keyboard / lock / recents / consent / banner)
# ===========================================================================
# The top edge now hosts TWO pull-downs, split left/right (P40 stage 2), so it
# takes two shots: the right half brings down the Control Center, the left half
# the Notification Center.
run_shot 11-control-center "Control Center: round toggle grid (pulled from top right)" 6 \
    ZELTO_SHADE_OPEN=cc
run_shot 11a-notification-center "Notification Center: clock + cards (pulled from top left)" 6 \
    ZELTO_SHADE_OPEN=nc
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
    run_shot 16-lock-screen "Lock screen: display clock high, notification cards" 9 \
    ZELTO_LOCK_NOTIFS=3
# With a passcode set the swipe reveals the keypad instead of unlocking, so the
# lock screen carries the one line of warning it otherwise does without.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\nsys.passcode\t1234\n' \
    run_shot 16a-lock-passcode-hint "Lock screen with a passcode set (swipe reveals the keypad)" 9 \
    ZELTO_LOCK_NOTIFS=2
# The unlock gesture itself: the whole plate lifted 1:1 with the finger and fading
# as it rises. Frozen by ZELTO_LOCK_DRAG (px, negative = up) — the drag IS the
# transition, so a shot after the release would show an unlocked screen instead.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\n' \
    run_shot 16b-lock-unlock-drag "Lock screen lifted toward the unlock swipe (frozen -160px)" 9 \
    ZELTO_LOCK_NOTIFS=3 ZELTO_LOCK_DRAG=-160
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t6\n' \
    run_shot 17-lock-off "Screen-off scrim (idle past idle_off_s while locked)" 11

# App Switcher: three apps left running, then the overlay on top. Plus its two
# gestures, frozen: the deck paged between cards, and the centred card lifted
# toward the flick-up close.
run_shot 18-switcher "App Switcher: card deck (3 running apps)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1
run_shot 18a-switcher-paging "App Switcher paged between two cards (frozen 1.5)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1 \
    ZELTO_SWITCHER_SCROLL=1.5
run_shot 18b-switcher-close "App Switcher: centred card flicked up to close (frozen)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1 \
    ZELTO_SWITCHER_LIFT=-140
# Permission consent modal (spawned standalone with app_id + perm).
run_shot 19-consent "Permission consent dialog (Allow / Deny modal)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications"
# The SHARE SHEET. It had no shot at all before P41 — a whole system surface with
# zero coverage, which is exactly how it stayed a desktop "Open with..." dialog
# through five design phases. Spawned standalone over a sharing app with the
# candidate app_ids zsysd would have resolved, plus the ZELTO_SHARE_* pair zsysd
# passes it in the environment so the preview row has something to preview.
run_shot 19a-share-sheet "Share sheet: preview, target row, actions (bottom sheet)" 8 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens"
run_shot 19b-share-sheet-enter "Share sheet mid rise + backdrop fade (frozen 0.5)" 8 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens" \
    ZELTO_CHOOSER_ENTER=0.5
run_shot 19c-share-sheet-dismiss "Share sheet dragged down toward dismissal (frozen)" 8 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens" \
    ZELTO_CHOOSER_DRAG=120
# Notification banner: pinger auto-posts, consent auto-allows -> shade heads-up.
run_shot 20-banner "Heads-up notification banner (auto-posted + auto-granted)" 10 \
    SIM_APP=zelto-pinger ZELTO_PINGER_POST=1 ZELTO_CONSENT_AUTO=allow

# ---------------------------------------------------------------------------
# TRANSITION FREEZE-FRAMES (P32): each transient surface's entrance pinned mid-
# flight via a ZELTO_*_ENTER=<0..1> hook, so the slide+fade is shot-verifiable.
# ---------------------------------------------------------------------------
SEED='sys.volume\t7\n' \
    run_shot 42-volume-enter "Volume HUD mid slide+fade entrance (frozen 0.5)" 6 \
    ZELTO_VOLUME_ENTER=0.5
run_shot 43-consent-enter "Consent modal mid slide-up+fade entrance (frozen 0.5)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications" ZELTO_CONSENT_ENTER=0.5
# Deterministic: ZELTO_BANNER_DEMO fabricates the banner in the shade sink, so the
# entrance is verifiable without the flaky post->consent->grant->deliver dance.
run_shot 44-banner-enter "Heads-up banner mid slide+fade entrance (frozen 0.5)" 6 \
    ZELTO_BANNER_DEMO=1 ZELTO_BANNER_ENTER=0.5
run_shot 45-toast-enter "Launcher toast mid slide-up+fade entrance (frozen 0.5)" 6 \
    ZELTO_TOAST_ENTER=0.5

# State-change cross-fades (P32 item 2): a toggle's on/off fill animates instead
# of hard-swapping — pinned mid cross-fade via ZELTO_QS_ANIM. Plus the P31 press
# flash verified over a bottom-nav button (ZELTO_PRESS_APP scopes it to the nav).
run_shot 46-qs-crossfade "Control Center toggles mid on/off cross-fade (frozen 0.5)" 6 \
    ZELTO_SHADE_OPEN=cc ZELTO_QS_ANIM=0.5
# NB the ZELTO_SETTINGS_SCREEN=network. P42 turned Settings into a drill-down, so
# the root screen is now an index of rows with chevrons and NO toggles on it at
# all — this shot kept its hook, kept resolving, and quietly went back to
# photographing a screen with nothing on it that could cross-fade. A toggle shot
# has to name the screen the toggles moved to.
run_shot 47-settings-toggle "Settings toggles mid on/off cross-fade (frozen 0.5)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=network ZELTO_QS_ANIM=0.5
# Press feedback on a SYSTEM OVERLAY rather than on an app. This used to aim at
# the bottom nav bar's Back button — but P40 deleted system/nav/ for the home
# gesture, so ZELTO_PRESS_APP=nav_body matched no surface and the shot silently
# captured a bare home screen for two phases. The keyboard is the right heir: it
# is a system surface, it is a dense field of identical targets where "which one
# did I hit" is the whole question, and its caps are the one place in the OS where
# the press veil has to read at a glance. Coordinates are KEYBOARD-LOCAL (the
# surface is KBD_H tall, bottom-anchored above the home indicator): the home row's
# "g", centre of the grid.
#
# P44: 114 -> 140. The keyboard strip is ZELTO_KBD_H now (369, derived from four
# ZELTO_KEY_H caps + gaps + padding) where it was a flat 300 with 56-unit caps --
# the caps stood 30pt tall, under the 44pt touch-target minimum. The home row's
# band is surface-local 102..179, so its centre is 140. Verified by measurement,
# not by assuming the old number still landed on a key.
run_shot 48-key-press "Keyboard: a key pressed (touch-down highlight veil)" 8 \
    ZELTO_KBD_SHOW=1 SIM_APP=zelto-notepad \
    ZELTO_PRESS_APP=kbd_body ZELTO_PRESS_X=364 ZELTO_PRESS_Y=140

# App-open continuity (P32 item 3b): the tapped tile drifts toward centre while the
# rest of home fades — the launch hand-off, frozen mid-flight.
run_shot 49-home-launch "App-open cue: tapped tile drifts + home fades (frozen 0.6)" 6 \
    ZELTO_HOME_LAUNCH=1

# In-app Navigator push, unified on the STANDARD token with a coordinated slide +
# cross-fade (P32 item 3a), frozen mid-push.
run_shot 50-nav-push "Navigator push: detail slides + cross-fades in (frozen 0.45)" 8 \
    SIM_APP=zelto-hello ZELTO_NAV_PUSH=0.45

# Reduce Motion (P32 item 4): with sys.reduce_motion=1 the entrance spring is
# collapsed, so the SAME ENTER=0.5 request that half-fades the HUD in shot 42 now
# shows it fully seated — a still A/B proof that springs are suppressed.
SEED='sys.volume\t7\nsys.reduce_motion\t1\n' \
    run_shot 51-reduce-motion "Reduce Motion: volume entrance collapsed (vs 42)" 6 \
    ZELTO_VOLUME_ENTER=0.5

# ---------------------------------------------------------------------------
# GESTURE / INTERRUPTIBLE MOTION FREEZE-FRAMES (P33): every gesture-reachable
# mid-drag / past-limit / interrupted-handoff state, pinned by a deterministic
# ZELTO_*_DRAG / _OVERPULL / _BACK hook (grep them in the surface's main.c). No
# injected pointer input — the drag amount is frozen so the frame is byte-stable.
# ---------------------------------------------------------------------------
# Rubber-band at a scroll edge: the Settings list over-pulled off the top, the
# gap resisting with the diminishing-returns curve (ZELTO_SCROLL_OVERPULL px).
run_shot 52-scroll-overpull "Scroll edge rubber-band: list over-pulled off the top" 8 \
    SIM_APP=zelto-settings ZELTO_SCROLL_OVERPULL=160
# Swipe-to-dismiss transient surfaces, each dragged partway toward dismissal.
SEED='sys.volume\t7\n' \
    run_shot 53-volume-dismiss "Volume HUD dragged up toward swipe-dismiss (frozen)" 6 \
    ZELTO_VOLUME_SHOW=1 ZELTO_VOLUME_DRAG=-70
run_shot 54-banner-dismiss "Heads-up banner dragged up toward swipe-dismiss (frozen)" 6 \
    ZELTO_BANNER_DEMO=1 ZELTO_BANNER_DRAG=-64
run_shot 55-toast-dismiss "Launcher toast dragged down toward swipe-dismiss (frozen)" 6 \
    ZELTO_TOAST_ENTER=1 ZELTO_TOAST_DRAG=60
# Consent: dragged down partway (a downward flick past threshold maps to Deny).
run_shot 56-consent-drag "Consent modal dragged down toward flick-to-Deny (frozen)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications" ZELTO_CONSENT_DRAG=90
# Interruptible Navigator back-swipe: the top screen dragged partway back, the
# incoming screen sliding under it (ZELTO_NAV_BACK=<0..1> = how far the finger is).
run_shot 57-nav-back-swipe "Navigator back-swipe tracking the finger (frozen 0.5)" 8 \
    SIM_APP=zelto-hello ZELTO_NAV_BACK=0.5
# Control Center over-pulled past fully-open, resisting with the rubber-band.
run_shot 58-shade-overpull "Control Center pulled past open, rubber-banding at the limit (frozen)" 6 \
    ZELTO_SHADE_PULL=1.25
# Reduce Motion A/B: the release animation collapses, but the 1:1 drag itself is
# intact — the SAME volume drag as 53, still shown at the frozen finger position.
SEED='sys.volume\t7\nsys.reduce_motion\t1\n' \
    run_shot 60-reduce-drag "Reduce Motion: drag tracks finger, release would snap (vs 53)" 6 \
    ZELTO_VOLUME_SHOW=1 ZELTO_VOLUME_DRAG=-70
# Hit-test on a MOVING subtree: the Control Center is frozen OVER-PULLED (slid
# down past open by the rubber-band) and the press freeze-hook is aimed at a
# toggle AT ITS LIVE offset position. The press veil landing on the (opaque,
# translated) disc proves hit_test uses the offset frame — a moving subtree stays
# tappable where it visually is, not at its un-shifted layout home. It aims at an
# OFF toggle (Lock, row 2 centre): the veil is a white wash, so on an ON toggle —
# a near-white PRIMARY disc — there would be nothing to see.
run_shot 61-hit-test-moving "Hit-test while moving: press lands on the offset Lock toggle" 6 \
    ZELTO_SHADE_PULL=1.18 ZELTO_PRESS_APP=shade_body \
    ZELTO_PRESS_X=360 ZELTO_PRESS_Y=232

# ===========================================================================
# STATUS BAR STATES (seed the brokered sys.* the bar reads; home behind it)
# ===========================================================================
SEED='sys.wifi\t1\nsys.airplane\t0\nsys.signal\t4\nsys.brightness\t5\n' \
    run_shot 21-bar-wifi-bright "Status bar: full cellular + Wi-Fi, brightness high" 6
# Weak cellular: the unlit bars stay drawn (TEXT_FAINT), so the mark keeps its
# silhouette at every level instead of shrinking.
SEED='sys.wifi\t1\nsys.airplane\t0\nsys.signal\t1\nsys.brightness\t5\n' \
    run_shot 21a-bar-signal-low "Status bar: one cellular bar lit of four" 6
# Airplane mode REPLACES the bars with the plane (the radios are off, so a signal
# reading beside it would state the opposite of the truth).
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
run_shot 34-app-settings "App: Settings, root list (drill-down rows)" 8 \
    SIM_APP=zelto-settings
# The DETAIL screens behind the root's chevrons (P42). Settings used to be one
# flat scroll, so this pair used to be "the top of the list" and "the list
# scrolled to its end" (ZELTO_SCROLL_TO=1500) — a shot that reviewed the same
# screen twice. Now each subject is its own pushed screen, reached by an env hook
# rather than a tap so the catalogue stays reproducible.
run_shot 34a-settings-display "Settings: Display & Sound (brightness SLIDER)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=display
run_shot 34b-settings-lock "Settings: Lock Screen detail (toggles + steppers)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock
run_shot 34c-settings-wallpaper "Settings: Wallpaper picker detail screen" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=wallpaper
run_shot 35-app-store    "App: Store"    8 SIM_APP=zelto-store
run_shot 36-app-hello    "App: Rows (SDK sample)" 8 SIM_APP=zelto-hello
run_shot 37-app-pinger   "App: Pinger"   8 SIM_APP=zelto-pinger
# A Zelto Script app: one shared runtime binary, so it is launched by .js path
# (SIM_SCRIPT) rather than by binary name (SIM_APP).
run_shot 38-app-jsdemo   "App: JS Demo (Zelto Script)" 9 \
    SIM_SCRIPT="$REPO_ROOT/system/apps/jsdemo/jsdemo.js"

# ===========================================================================
# ZELTO SCRIPT: PARITY WITH C (P35)
# Each of these drives a binding that a script app could not reach before —
# gestures, motion, text input, navigation, and the brokered system APIs — so the
# shot is of the FEATURE WORKING, not of a layout that merely mentions it.
#
# Unlike the frozen gesture shots above, these use real input: zcomp scripts the
# press-and-hold that wlrctl cannot (ZCOMP_DRAG / ZCOMP_HOLD, seat.c), driving the
# same seat path a finger does. ZCOMP_INPUT_DELAY waits for the app to map.
# ===========================================================================
JSDEMO="$REPO_ROOT/system/apps/jsdemo/jsdemo.js"

# onPan: the card dragged sideways, caught MID-drag (it springs home on release,
# so a shot after the release would show nothing). Proves the JS closure is
# driving the spring 1:1 from the finger.
run_shot 70-script-pan "Script: card dragged by onPan (mid-drag, finger-tracked)" 7 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_DRAG="180 418 520 418 6000"

# onLongPress: a hold in place past the threshold toggles "Pinned" (and suppresses
# the tap the release would otherwise have produced).
run_shot 71-script-longpress "Script: onLongPress pinned the card (tap suppressed)" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 418 900"

# Navigator: a pushed screen, with its own hook state and the props it was pushed
# with. Back (edge-swipe / Escape) pops it without the script's help.
run_shot 72-script-nav "Script: Navigator pushed a second screen" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 916 100"

# TextField: tapping the field focuses it and raises the system on-screen keyboard
# (P21) — the app handles no keys at all.
run_shot 73-script-textfield "Script: TextField focused, on-screen keyboard up" 12 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 557 100"

# Networking: the script awaits the `network` grant (the system consent modal runs
# on the live loop), then fetches over the async state machine. SIM_NET=1 serves
# the endpoint locally; the card shows the real 200 + body.
run_shot 74-script-net "Script: fetch() after awaiting the network grant" 14 \
    SIM_SCRIPT="$JSDEMO" SIM_NET=1 ZELTO_CONSENT_AUTO=allow \
    ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="190 697 100"

# Notifications: posted by the script through the same broker a C app uses, with an
# action button that routes back to it. Captured as the heads-up banner.
run_shot 75-script-notify "Script: notification posted (heads-up banner + action)" 13 \
    SIM_SCRIPT="$JSDEMO" ZELTO_CONSENT_AUTO=allow \
    ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="524 697 100"

# Settings: the script writes sys.mute and the broker echoes the change back to its
# observer, which recolours the row — the same live fan-out the shade gets.
run_shot 76-script-settings "Script: wrote sys.mute, observer echoed it back live" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="527 758 100"

# Packaging: the Greeter is not in the image at all. It is packaged as a signed
# .zap and installed onto the persistent disk first (ZAP=), so the boot below knows
# the app only from what the installer left there — a script app running from
# /var/zelto, whose code was signature- and hash-verified before it ever ran. The
# tap lands on its tile, which exists only because the install worked.
ZAP="$REPO_ROOT/system/apps/greeter/zelto-greeter.app:$REPO_ROOT/system/apps/greeter/greeter.js" \
run_shot 77-script-installed "Script: installed from a signed .zap, running from /var/zelto" 12 \
    ZCOMP_INPUT_DELAY=5000 ZCOMP_HOLD="447 460 100"

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
