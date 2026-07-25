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

# WHAT THE PICTURE MUST CONTAIN — set EXPECT (an extended regex) or NOMARKER (a
# reason) before each run_shot. One of the two is REQUIRED; the lint at the foot
# of this file fails the run if a shot declares neither.
#
# WHY. P45 found `11a-notification-center` photographing an EMPTY Notification
# Center: the recipe opened the panel and never posted a notification, so the
# frame was a panel saying "No notifications" under a name promising cards. Its
# delta was a healthy 6.94%, entirely from the blurred wallpaper behind it. That
# is the whole problem with judging a catalogue by deltas — A SHOT OF THE WRONG
# SCREEN HAS A PERFECTLY HEALTHY DELTA — and it was caught by eye, once, out of
# 74 frames.
#
# So each shot now states what it claims to show, and the same boot that takes
# the picture checks it. Every libzelto surface dumps its laid-out text under
# ZELTO_PROBE_TAPS (sdk/src/app.c), so EXPECT is matched against the strings
# THE SURFACE ACTUALLY BUILT, not against pixels: a screen that renders the
# wrong content fails even if it renders it beautifully.
#
# NOMARKER is for the shots whose claim genuinely is not textual — a press veil,
# a frame frozen mid-slide, a status-bar glyph. Those need the pixel checks
# instead (meta/pngdiff.py --expect-box against a control at the same state);
# writing the reason down is what stops "no marker" from becoming the default.
# MUSTNOT is the other half of the same question. Several frames are named for a
# state whose evidence is what is NOT on screen: home page 2 is "the page without
# the widgets on it", and a filtered App Library is "the list with Store missing".
# A positive marker cannot express either, and both are exactly the kind of shot
# that quietly reverts to photographing page 1.
#
# AND THE PIXEL HALF (P47). NOMARKER is honest and it is also where the audit
# STOPPED: twenty shots ended up with a written excuse and no check at all. The
# tool to check them has existed since P41 (meta/pngdiff.py --expect-box, which
# answers "did the thing at (x,y) actually change, and is that the strongest
# change on screen") and was never wired in — it was something a person ran by
# hand, which is the same category of unverified as the reason it replaced.
#
#   PIXEL='<control-shot> <region> [min-delta]'
#     Compare this frame against an EARLIER shot in the catalogue and require a
#     real, localised change. <region> is one of:
#       all       the whole frame; the change must be big, but not localised
#                 (a dim scrim and a screen-off scrim change everything)
#       spot      the change must be SMALL overall and CONCENTRATED: the
#                 strongest tile at least <min-delta> times the whole-frame mean.
#                 That is what a press veil IS, stated without saying where — for
#                 the surfaces where "where" cannot be known (see below)
#       @ns:NAME  the change must be inside the SCREEN BOX of the layer surface
#                 whose namespace is NAME, and be the strongest change on the
#                 frame. This is the real version of what `spot` approximates.
#
#     WHERE THE BOX COMES FROM, and why this is now possible (P49). Everything
#     below about `@Label` is still true — a client is never told where the
#     compositor put its surface, and the probe's frames are in SURFACE
#     coordinates. What changed is that P48 made the ONE PROCESS THAT DOES KNOW
#     say so: ZELTO_SURFACE_LOG=1 (set on every shot's boot, above) makes zcomp
#     print
#
#         [zcomp] surface ns='keyboard' layer=2 screen=0,966 720x474 exclusive=474
#
#     for every layer surface it arranges. `@ns:` reads that line out of THIS
#     SHOT'S OWN LOG — not a number pasted into this file — and hands the box to
#     pngdiff --expect-box. So the check aims wherever the compositor actually put
#     the surface on the boot that took the picture, which is the thing P47 wanted
#     and could not have. It is strictly stronger than `spot`: `spot` says "the
#     change was concentrated somewhere", this says "the change was concentrated
#     THERE".
#
#     THE NAMESPACE IS THE BODY FUNCTION'S NAME (the SDK passes app->title), so
#     it is `kbd_body`, `shade_body`, `lock_body` — and `bar_body` is BOTH the
#     status bar and the home bar, two processes that named their body the same
#     thing. `@ns:bar_body` would therefore be ambiguous, which is another reason
#     the status-bar shots keep the NOMARKER + LOGSAYS pair they were given in
#     P47 rather than being "upgraded" here.
#
#     AND IT ONLY WORKS FOR LAYER SURFACES. An ordinary app is an xdg toplevel
#     and zcomp logs nothing for it — so the launcher, Settings and the apps are
#     out of reach of this, and 41-press-tile keeps `spot`. That is a smaller gap
#     than it sounds: a toplevel's box is the usable area, which is the one
#     geometry a test can already reason about.
#
#     THE AUDIT THIS MADE POSSIBLE, and its result: of the sixteen NOMARKER shots,
#     exactly ONE could stop being a heuristic. That is a real answer rather than
#     a disappointing one, and the reasons divide cleanly:
#       - 48-key-press -> `@ns:kbd_body`. Upgraded. The keyboard is a 720x474
#         strip at the bottom and the veil is inside it, so the box says
#         something `spot` could not.
#       - the VOLUME HUD (12, 42, 52, 53, 54), the DIM scrim and the SCREEN-OFF
#         scrim are layer surfaces whose box is 720x1359 — the whole area under
#         the status bar. The HUD is a small card drawn inside a full-height
#         surface, so `@ns:vol_body` would be `all` wearing a box. Their claim is
#         a POSITION within that surface, which the compositor's line cannot
#         speak to. They keep what they had.
#       - the STATUS-BAR shots are `bar_body`, which is ambiguous (above), and
#         were measured and refused in P47 anyway.
#       - 03-home-flip and 41-press-tile are the LAUNCHER, an xdg toplevel, which
#         zcomp does not log at all.
#     So the mechanism is right and its reach is one shot wide today. It gets
#     wider the moment a layer surface is sized to the thing it draws.
#
#     WHAT IT STILL CANNOT DO, and the keyboard is the example. A surface that
#     SLIDES or RESIZES has a different box at capture time than at the arrange
#     that logged it: the keyboard is logged at 966 and at 1071 on the same boot
#     as the suggestion strip appears, and it is logged again mid-slide. The last
#     line for a namespace is the box it settled at, which is right for a settled
#     screen and wrong for a shot taken during a transition — so a transition shot
#     keeps `spot`. `@ns:` takes the LAST line, and says so here rather than
#     pretending the ambiguity is not there.
#
#     THERE IS NO `@Label` REGION EITHER, and the attempt to build one is why
#     `spot` exists. The idea was right — take the box from the frame the PROBE
#     reports for a named control, so a press-veil check aims wherever the layout
#     put the thing instead of at a pasted coordinate. It cannot work. The probe
#     reports SURFACE coordinates and a PNG is the SCREEN, and a Wayland client is
#     never told where the compositor put its surface. It is not only the layer
#     surfaces: the LAUNCHER is 720x1359 on a 720x1440 screen, because the status
#     bar's exclusive zone pushes it down 81 units. The check was written, and it
#     PASSED on the launcher for one run — the box was 81 units too high and
#     pngdiff's localisation test has 40px tiles and slack at the edges, so a
#     wrong box agreed with the right answer. A guard that refused non-full-screen
#     surfaces is what turned that into a failure, and then there was nothing left
#     for it to run on.
#
#     THERE IS NO `bar` REGION, and there was: a check on the status-bar strip was
#     written, run, and deleted on the evidence. Two measurements killed it. The
#     mean delta inside the whole 720x81 strip when a Wi-Fi glyph changes is
#     0.128, and when a cellular level changes 0.334 — the glyphs are a few dozen
#     pixels in a strip of 58,320 — so any threshold that passes is also passed by
#     nothing happening. Worse, the CLOCK is in that same strip and moves between
#     boots of a 20-minute catalogue run, so the box has a permanent, larger
#     signal in it that has nothing to do with what the shot is about. That is the
#     noise-floor mistake P45 found in the whole-frame deltas, one box down.
#     The status-bar shots keep their NOMARKER reason; the settings behind their
#     glyphs are asserted by test_settings_broker_sim and test_power_services_sim.
#
#     @Label ONLY WORKS ON A FULL-SCREEN SURFACE, and the first attempt to use it
#     on the keyboard is how that was found. The probe reports frames in SURFACE
#     coordinates; the PNG is the SCREEN. For the launcher those are the same
#     thing. For a bottom-anchored layer surface 369 units tall they are not, and
#     the box came out 1000 units above the pixels it was describing — a check
#     that would have failed forever while looking like a real one. A client has
#     no way to learn where the compositor put its layer surface, so this is
#     refused rather than guessed, and `spot` exists for those cases.
#   PIXELMEAN='<max mean RGB>'
#     This frame's mean RGB must be BELOW this. For the frames whose whole claim
#     is that the screen went dark, where "it changed" is true of a crash too.
#
# Both are independent of EXPECT/NOMARKER: a shot may have a marker AND a pixel
# check. The lint at the foot of the file counts a failure of either.
#
# AND THE THIRD CHANNEL: LOGSAYS (P47). A surface with almost no text cannot be
# asked what is on it — EXPECT matches laid-out STRINGS, and the status bar's
# content is a drawn radio, a drawn plane and a drawn battery cell. A pixel check
# does not work there either (see the note on the deleted `bar` region below), so
# those five shots were the last ones verified by nothing.
#
# LOGSAYS='<extended regex>' is matched against the shot's whole boot log, which
# is where a surface that draws marks can SAY what it drew:
#
#     [bar] marks radio=cellular:4 wifi=on lock=off battery=100 charging=no
#
# It is a weaker claim than EXPECT and the difference matters: EXPECT reads the
# laid-out tree, LOGSAYS reads what a surface reports about itself. What it
# catches is every way these shots have actually been wrong — a seed not
# arriving, the bar not observing the broker, airplane not overriding the radios.
# What it cannot catch is the bar deciding correctly and then drawing nothing.
EXPECT=""
MUSTNOT=""
LOGSAYS=""
NOMARKER=""
PIXEL=""
PIXELMEAN=""
MARKERLESS=()
MARKER_FAIL=0

# run_shot NAME CAPTION DELAY [ENV=VAL ...]
#   NAME     out/shots/NAME.png + gallery id
#   CAPTION  human description (how the state is reached)
#   DELAY    seconds to let the sim settle before grim (SHOT_DELAY)
#   ENV=VAL  extra environment for run-sim.sh (test-hooks, SIM_APP, SIM_EXTRA, ...)
# Pre-seed the brokered settings store by setting $SEED before the call; install a
# script package first by setting $ZAP.
# "<process>|<string>" for every string a shot's surfaces laid out ON SCREEN.
#
# Off-screen strings are dropped, and that is what makes a marker mean something:
# the home carousel builds EVERY page and slides the strip, so "App Library" is in
# the tree of every home shot whether or not that page is the one being
# photographed. With the offscreen lines filtered out, a marker asserts the thing
# is in the PICTURE rather than merely in the tree.
probe_says() {
    grep "zelto: probe text " "$1" | grep -v ' offscreen$' |
        sed -n "s/.*zelto: probe text \[\([^]]*\)\] '\(.*\)' x=.*/\1|\2/p"
}

run_shot() {
    local name="$1" caption="$2" delay="$3"; shift 3
    # Register the shot in the gallery BEFORE the ONLY filter. The contact sheet is
    # the review artifact, and it lists whatever PNGs are on disk (missing ones are
    # skipped when it is written) — so a filtered re-run of one shot must still
    # emit the WHOLE sheet. Registering after the filter meant `ONLY=x meta/shots.sh`
    # rewrote index.html with a single card and threw the catalogue away.
    GALLERY+=("$name|$caption")
    # Declared before the ONLY filter, so a filtered run cannot hide a shot that
    # never said what it contains.
    local expect="$EXPECT" mustnot="$MUSTNOT" nomarker="$NOMARKER"
    local pixel="$PIXEL" pixelmean="$PIXELMEAN" logsays="$LOGSAYS"
    EXPECT=""; MUSTNOT=""; NOMARKER=""; PIXEL=""; PIXELMEAN=""; LOGSAYS=""
    if [ -z "$expect" ] && [ -z "$mustnot" ] && [ -z "$nomarker" ] &&
       [ -z "$logsays" ]; then
        echo "    !! $name declares neither EXPECT nor NOMARKER"
        MARKERLESS+=("$name")
        MARKER_FAIL=1
    fi
    # ONLY is an EXTENDED REGEX matched against the shot name, not a substring.
    # A bare word is still a valid regex that matches as a substring, so every
    # existing `ONLY=home` invocation means what it always did — but a review
    # subset ("re-shoot these fifteen and diff them", P54 stage 0) is a set of
    # unrelated names, and fifteen sequential full-catalogue passes to reach
    # them is three hours to answer a question worth fifteen minutes.
    if [ -n "$ONLY" ] && [[ ! "$name" =~ $ONLY ]]; then SEED=""; ZAP=""; return 0; fi

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
            ZELTO_PROBE_TAPS=1 ZELTO_PROBE_AT="$(( delay * 1000 - 250 ))" \
            ZELTO_SURFACE_LOG=1 \
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

    # Is the thing the NAME promises actually in the picture? Matched against the
    # strings the surfaces built on this very boot, so "ok" stops meaning "a PNG
    # exists".
    #
    # The probe samples 250ms before grim, not a second before. That gap matters
    # more than it sounds: at a second out, the Settings drill-downs were still
    # MID-PUSH (the Navigator draws BOTH screens during a transition, so the root
    # list was legitimately on screen) and the script demo's notification had not
    # reached the shade yet. Three shots were reported as showing the wrong thing
    # while their PNGs were correct. A description of a frame has to be taken when
    # the frame is.
    if [ -n "$expect" ] && [ -s "$png" ]; then
        # Matched against "<process>|<string>" lines, so a marker can name WHICH
        # surface has to say it. The launcher's widget and the lock screen both
        # draw a clock; "a clock is on screen" is not the claim "the lock screen
        # is up".
        if probe_says "$dir/log" | grep -Eq "$expect"; then
            echo "    contains: $expect"
        else
            echo "    !! $name does NOT contain what it claims: /$expect/"
            echo "       (see $dir/log — grep \"probe text\" for what it DOES say)"
            MARKERLESS+=("$name: missing /$expect/")
            MARKER_FAIL=1
        fi
    fi
    if [ -n "$mustnot" ] && [ -s "$png" ]; then
        if probe_says "$dir/log" | grep -Eq "$mustnot"; then
            echo "    !! $name shows what it must NOT: /$mustnot/"
            MARKERLESS+=("$name: shows /$mustnot/")
            MARKER_FAIL=1
        else
            echo "    absent (as claimed): $mustnot"
        fi
    fi

    # What a MARK-DRAWING surface says it drew. See the note over LOGSAYS above.
    if [ -n "$logsays" ] && [ -s "$png" ]; then
        if grep -Eq "$logsays" "$dir/log"; then
            echo "    reported: $logsays"
        else
            echo "    !! $name did not report what it claims: /$logsays/"
            echo "       (see $dir/log)"
            MARKERLESS+=("$name: no log line matching /$logsays/")
            MARKER_FAIL=1
        fi
    fi

    # THE PIXEL HALF. See the note over PIXEL above.
    if [ -n "$pixelmean" ] && [ -s "$png" ]; then
        local mean
        mean="$(python3 "$REPO_ROOT/meta/pngdiff.py" "$png" |
                sed -n 's/.*mean RGB = \([0-9.]*\) \([0-9.]*\) \([0-9.]*\).*/\1 \2 \3/p' |
                awk '{ print ($1 + $2 + $3) / 3.0 }')"
        if [ -z "$mean" ]; then
            echo "    !! $name: could not read a mean RGB"
            MARKERLESS+=("$name: no mean RGB"); MARKER_FAIL=1
        elif awk -v m="$mean" -v x="$pixelmean" 'BEGIN { exit !(m < x) }'; then
            echo "    dark: mean RGB $mean < $pixelmean"
        else
            echo "    !! $name is not dark: mean RGB $mean, must be < $pixelmean"
            MARKERLESS+=("$name: mean RGB $mean >= $pixelmean"); MARKER_FAIL=1
        fi
    fi
    if [ -n "$pixel" ] && [ -s "$png" ]; then
        local ctrl_name region mind ctrl
        # shellcheck disable=SC2086
        set -- $pixel
        ctrl_name="$1"; region="$2"; mind="${3:-2.0}"
        ctrl="$OUT/$ctrl_name.png"
        if [ ! -s "$ctrl" ]; then
            # Not a failure: ONLY= runs one shot and its control is not on disk.
            # Silence here would be worse than either — it would make a filtered
            # run look like a passing one.
            echo "    .. pixel check skipped: control $ctrl_name.png not present"
        else
            local args=""
            case "$region" in
                all|spot) args="" ;;
                @ns:*)
                    # THE BOX COMES OUT OF THIS BOOT'S OWN LOG. See the @ns: note
                    # at the head of this file: zcomp is the only process that
                    # knows where a layer surface ended up, and ZELTO_SURFACE_LOG
                    # makes it say so. The LAST line for the namespace is the box
                    # it settled at.
                    # THE LAST NON-EMPTY LINE, and "non-empty" is not pedantry:
                    # zcomp arranges a layer surface before it has a buffer, so
                    # the log carries `screen=0,966 0x0` lines for the same
                    # namespace, and a hidden surface's last line is one of them.
                    # A 0x0 box would make --expect-box pass or fail on nothing.
                    local ns box
                    ns="${region#@ns:}"
                    box="$(sed -n "s/.*\[zcomp\] surface ns='$ns' .*screen=\([0-9]*\),\([0-9]*\) \([1-9][0-9]*\)x\([1-9][0-9]*\).*/\1 \2 \3 \4/p" \
                           "$dir/log" | tail -1)"
                    if [ -z "$box" ]; then
                        echo "    !! $name: no [zcomp] surface line for ns='$ns' in this boot's log"
                        MARKERLESS+=("$name: no surface box for $ns"); MARKER_FAIL=1
                        SEED=""; ZAP=""; return 0
                    fi
                    echo "    surface '$ns' is at $box (from this boot's zcomp log)"
                    # shellcheck disable=SC2086
                    args="--expect-box $box --min-delta $mind"
                    ;;
                *)
                    echo "    !! $name: unknown PIXEL region '$region'"
                    MARKERLESS+=("$name: bad PIXEL region"); MARKER_FAIL=1
                    SEED=""; ZAP=""; return 0 ;;
            esac
            # THE ASSIGNMENT GOES IN THE `if`, and that is not style. This file
            # runs under `set -e`, where `out="$(cmd)"` with a failing cmd kills
            # the whole run — and pngdiff FAILING is the normal, expected outcome
            # this code exists to report. Written the obvious way it took the
            # catalogue down at shot 48 of 77, silently and with status 0, which
            # is the worst of both: no report and no failure either.
            local out rc
            if out="$(python3 "$REPO_ROOT/meta/pngdiff.py" "$ctrl" "$png" $args 2>&1)"; then
                rc=0
            else
                rc=$?
            fi
            local whole
            whole="$(echo "$out" | sed -n 's/.*mean |delta| = \([0-9.]*\).*/\1/p' | head -1)"
            if [ "$region" = spot ]; then
                # A press veil is a SMALL, CONCENTRATED change: a wash of alpha
                # over one control on an otherwise identical screen. Both halves
                # matter — "something changed" is also true of a different screen,
                # and "the strongest tile is strong" is also true of a scrolled
                # list. The ratio between them is what only a veil produces, and
                # it needs no coordinate to state.
                local top
                top="$(echo "$out" | sed -n 's/^    ([ 0-9]*,[ 0-9]*)  \([0-9.]*\)$/\1/p' | head -1)"
                if [ -z "$whole" ] || [ -z "$top" ]; then
                    echo "    !! $name: could not read a delta from pngdiff"
                    MARKERLESS+=("$name: no delta"); MARKER_FAIL=1
                elif awk -v t="$top" -v w="$whole" -v m="$mind" \
                        'BEGIN { exit !(w > 0 && t / w >= m) }'; then
                    echo "    a localised change vs $ctrl_name (peak $top, frame $whole)"
                else
                    echo "    !! $name: the change vs $ctrl_name is not a localised one (peak ${top:-?}, frame ${whole:-?}; needs peak >= $mind x frame)"
                    MARKERLESS+=("$name: change not concentrated vs $ctrl_name")
                    MARKER_FAIL=1
                fi
            elif [ "$region" = all ]; then
                # No localisation to test — a scrim changes the whole screen —
                # so the claim is only that the change is BIG. Judged here rather
                # than by pngdiff, which has no --expect-whole.
                if [ -n "$whole" ] && awk -v d="$whole" -v m="$mind" 'BEGIN { exit !(d >= m) }'; then
                    echo "    differs from $ctrl_name by $whole (>= $mind)"
                else
                    echo "    !! $name is not different enough from $ctrl_name: |delta| ${whole:-?} < $mind"
                    MARKERLESS+=("$name: |delta| ${whole:-?} vs $ctrl_name < $mind")
                    MARKER_FAIL=1
                fi
            elif [ "$rc" = 0 ]; then
                echo "    changed where it should, vs $ctrl_name ($region)"
            else
                echo "    !! $name: the change vs $ctrl_name is not in $region"
                echo "$out" | sed 's/^/       /' | tail -6
                MARKERLESS+=("$name: change not localised to $region")
                MARKER_FAIL=1
            fi
        fi
    fi
    SEED=""
    ZAP=""
}

# ===========================================================================
# HOME SCREEN (the launcher's bento grid — env hooks in system/launcher/main.c)
# ===========================================================================
EXPECT='zelto-launcher\|All clear' \
run_shot 01-home-page1 "Home, page 1 (default seed: 3 widgets + 6 app icons)" 6

# Page 2 / carousel: force tiny 2-row pages so the default set overflows past one
# page, then settle on page 2 / freeze a flip mid-slide / show the overflow.
EXPECT='zelto-launcher\|Notepad' MUSTNOT='zelto-launcher\|All clear' \
run_shot 02-home-page2 "Home, page 2 (ROWS_PER_PAGE=2 forces overflow; HOME_PAGE=1)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_PAGE=1
NOMARKER='a flip is a POSITION, not content: both pages are built either way and the shot is of the strip part-way between them. Verified by pixel diff against 02, not by a string.' \
PIXEL='02-home-page2 all 2.0' \
run_shot 03-home-flip-mid "Carousel flip mid-slide (page 0->1 frozen 7 frames in)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_PAGE_FROM=0 ZELTO_HOME_PAGE=1 \
    ZELTO_HOME_ANIM_FRAMES=7
EXPECT='zelto-launcher\|All clear' MUSTNOT='zelto-launcher\|Store' \
run_shot 04-home-overflow "Overflowing layout + page dots (ROWS_PER_PAGE=2)" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2

# Rearrange mode + direct manipulation.
EXPECT='zelto-launcher\|Done' \
run_shot 05-home-rearrange "Rearrange mode: raster, remove badges, Done bar" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3
EXPECT='zelto-launcher\|Done' \
run_shot 06-home-ghost-lift "Rearrange: an icon lifted as a ghost under the finger" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=760
EXPECT='zelto-launcher\|Done' \
run_shot 07-home-reflow-mid "Rearrange: neighbours reflowing mid-drag (frozen 6 frames)" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=300 ZELTO_HOME_ANIM_FRAMES=6
EXPECT='zelto-launcher\|Done' \
run_shot 08-home-landing-mid "Rearrange: released ghost mid snap-back (frozen 6 frames)" 6 \
    ZELTO_HOME_REARRANGE=1 ZELTO_HOME_HELD=3 \
    ZELTO_HOME_GHOST_X=360 ZELTO_HOME_GHOST_Y=300 ZELTO_HOME_ANIM_FRAMES=6 \
    ZELTO_HOME_LANDING=1
EXPECT='zelto-launcher\|Done' MUSTNOT='zelto-launcher\|All clear' \
run_shot 09-home-crosspage "Rearrange: ghost held in the right edge gutter on page 1" 6 \
    ZELTO_HOME_ROWS_PER_PAGE=2 ZELTO_HOME_REARRANGE=1 ZELTO_HOME_PAGE=1 \
    ZELTO_HOME_HELD=4 ZELTO_HOME_GHOST_X=690 ZELTO_HOME_GHOST_Y=300

# App Library: the LAST page of the home carousel (the swipe-up drawer is gone).
# Reached by paging, so the shot just settles the carousel on that page.
EXPECT='zelto-launcher\|Store' \
run_shot 10-app-library "App Library: last carousel page (every installed app)" 6 \
    ZELTO_HOME_PAGE=1
# Searching: the field is focused, so the system keyboard is up, the page has
# inset itself by KBD_H (the compositor does not shrink the home window) and the
# dots + dock have stood down.
EXPECT='zelto-launcher\|App Library' MUSTNOT='zelto-launcher\|Store' \
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
NOMARKER='a touch-down VEIL is a wash of alpha over a tile, not a string; the PIXEL check is what says it landed — as a SHAPE (small overall, concentrated in one place) rather than a place, because no client can learn where the compositor put its surface. See the PIXEL note at the head of this file.' \
PIXEL='01-home-page1 spot 20' \
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
EXPECT='zelto-settings\|Network' \
run_shot 41-settings-press "Settings: a detail row pressed (touch-down veil)" 8 \
    SIM_APP=zelto-settings ZELTO_PRESS_X=360 ZELTO_PRESS_Y=183

# ===========================================================================
# SYSTEM OVERLAYS (shade / volume / keyboard / lock / recents / consent / banner)
# ===========================================================================
# The top edge now hosts TWO pull-downs, split left/right (P40 stage 2), so it
# takes two shots: the right half brings down the Control Center, the left half
# the Notification Center.
EXPECT='zelto-shade\|Airplane' \
run_shot 11-control-center "Control Center: round toggle grid (pulled from top right)" 6 \
    ZELTO_SHADE_OPEN=cc
# ZELTO_BANNER_DEMO=3 seeds the cards. Without it this shot opened the panel and
# photographed "No notifications" — a real state, but not the one the frame is
# NAMED for, and its 6.94% delta came entirely from the blurred wallpaper behind
# an empty panel. No delta threshold can catch that; only asking what the picture
# is supposed to contain can. (test_prose_overflow_sim asserts the same three
# cards are present, so this shot cannot quietly empty again.)
EXPECT='zelto-shade\|Grocery list' \
run_shot 11a-notification-center "Notification Center: clock + cards (pulled from top left)" 6 \
    ZELTO_SHADE_OPEN=nc ZELTO_BANNER_DEMO=3
NOMARKER='the HUD is a glyph and a level bar; it draws no text at all. Its value is asserted instead by test_power_services_sim, off the line zelto-volume logs; that it is ON SCREEN is the PIXEL check against the same home screen without it.' \
PIXEL='01-home-page1 all 0.4' \
SEED='sys.volume\t7\n' run_shot 12-volume-hud "Volume rocker HUD (shown at level 7)" 6 \
    ZELTO_VOLUME_SHOW=1

EXPECT='zelto-keyboard\|space' \
run_shot 13-keyboard "On-screen keyboard (QWERTY) over Notepad" 8 \
    ZELTO_KBD_SHOW=1 SIM_APP=zelto-notepad
EXPECT='zelto-keyboard\|ABC' MUSTNOT='zelto-keyboard\|q' \
run_shot 14-keyboard-symbols "On-screen keyboard, symbols layer" 6 \
    ZELTO_KBD_SHOW=1 ZELTO_KBD_SYMBOLS=1

# Lock lifecycle: seed the broker so zelto-lock arms short idle timeouts; headless
# has no seat input, so it idles from boot into each phase deterministically.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t2\nsys.idle_lock_s\t999\nsys.idle_off_s\t9999\n' \
# The dim scrim's LEVEL is asserted by test_power_services_sim, off the line
# zelto-dim logs. What that test cannot say is that the scrim reached the SCREEN,
# which is this frame's whole claim — so it is measured against 01-home-page1,
# the same home screen undimmed. (A dim shot that quietly stopped dimming would
# otherwise be a perfectly good picture of a home screen.)
NOMARKER='a dim scrim is alpha over the screen behind it; nothing is added to the tree. The dim LEVEL is asserted by test_power_services_sim off zelto-dims own log line; that it reached the screen is the PIXEL check.' \
PIXEL='01-home-page1 all 6' \
    run_shot 15-lock-dimmed "Pre-lock dim scrim (idle past idle_dim_s)" 7
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\n' \
EXPECT='zelto-lock\|' \
    run_shot 16-lock-screen "Lock screen: display clock high, notification cards" 9 \
    ZELTO_LOCK_NOTIFS=3
# With a passcode set the swipe reveals the keypad instead of unlocking, so the
# lock screen carries the one line of warning it otherwise does without.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\nsys.passcode\t1234\n' \
EXPECT='zelto-lock\|' \
    run_shot 16a-lock-passcode-hint "Lock screen with a passcode set (swipe reveals the keypad)" 9 \
    ZELTO_LOCK_NOTIFS=2
# The unlock gesture itself: the whole plate lifted 1:1 with the finger and fading
# as it rises. Frozen by ZELTO_LOCK_DRAG (px, negative = up) — the drag IS the
# transition, so a shot after the release would show an unlocked screen instead.
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t9999\n' \
EXPECT='zelto-lock\|' \
    run_shot 16b-lock-unlock-drag "Lock screen lifted toward the unlock swipe (frozen -160px)" 9 \
    ZELTO_LOCK_NOTIFS=3 ZELTO_LOCK_DRAG=-160
SEED='sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t3\nsys.idle_off_s\t6\n' \
# 17 IS THE ONE THAT MOST NEEDED THIS. P46 rescued it from being MISSING on every
# run (run-sim.sh's blank-frame guard rejects a flat fill, and this shot's whole
# subject IS a flat fill), and then never established that the frame it finally
# produced was CORRECT — a black PNG is also what a crashed compositor, a failed
# capture and a boot that never got anywhere produce. Two checks settle it: the
# frame is nearly black, AND it is a long way from 16-lock-screen, which is the
# same boot recipe stopped one phase earlier. Neither alone would do: "dark" is
# true of a dead sim, and "different from the lock screen" is true of the home
# screen.
NOMARKER='the screen-off scrim is opaque black over everything; a marker would assert the presence of something this frame exists to hide. Checked as pixels instead: PIXELMEAN + PIXEL below.' \
PIXELMEAN='6' PIXEL='16-lock-screen all 8' \
    run_shot 17-lock-off "Screen-off scrim (idle past idle_off_s while locked)" 11     ALLOW_FLAT=1

# App Switcher: three apps left running, then the overlay on top. Plus its two
# gestures, frozen: the deck paged between cards, and the centred card lifted
# toward the flick-up close.
EXPECT='zelto-recents\|' \
run_shot 18-switcher "App Switcher: card deck (3 running apps)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1
EXPECT='zelto-recents\|' \
run_shot 18a-switcher-paging "App Switcher paged between two cards (frozen 1.5)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1 \
    ZELTO_SWITCHER_SCROLL=1.5
EXPECT='zelto-recents\|' \
run_shot 18b-switcher-close "App Switcher: centred card flicked up to close (frozen)" 9 \
    SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1 \
    ZELTO_SWITCHER_LIFT=-140
# Permission consent modal (spawned standalone with app_id + perm).
EXPECT='zelto-consent\|Allow' \
run_shot 19-consent "Permission consent dialog (Allow / Deny modal)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications"
# The SHARE SHEET. It had no shot at all before P41 — a whole system surface with
# zero coverage, which is exactly how it stayed a desktop "Open with..." dialog
# through five design phases. Spawned standalone over a sharing app with the
# candidate app_ids zsysd would have resolved, plus the ZELTO_SHARE_* pair zsysd
# passes it in the environment so the preview row has something to preview.
# 10s, not 8. This is the only shot in the catalogue that shows the share sheet
# SETTLED — 19b and 19c pin it mid-motion with ZELTO_SHARE_* and are deterministic
# — so it is the only one whose marker depends on an entrance spring having
# finished. At 8s it passed twice and then failed with every string reported
# `offscreen` at y=1481 on a 1440-tall screen: the sheet was still below the
# bottom edge when the probe fired 250ms before the capture. A marker that flakes
# is worse than no marker, because the next person spends the run after it
# looking for a layout bug that is not there.
EXPECT='zelto-chooser\|Zelto OS design tokens' \
run_shot 19a-share-sheet "Share sheet: preview, target row, actions (bottom sheet)" 10 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens"
EXPECT='zelto-chooser\|Zelto OS design tokens' \
run_shot 19b-share-sheet-enter "Share sheet mid rise + backdrop fade (frozen 0.5)" 8 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens" \
    ZELTO_CHOOSER_ENTER=0.5
EXPECT='zelto-chooser\|Zelto OS design tokens' \
run_shot 19c-share-sheet-dismiss "Share sheet dragged down toward dismissal (frozen)" 8 \
    SIM_APP=zelto-notes \
    SIM_CHOOSER="os.zelto.notes os.zelto.store os.zelto.notepad" \
    ZELTO_SHARE_MIME=text/plain ZELTO_SHARE_PAYLOAD="Zelto OS design tokens" \
    ZELTO_CHOOSER_DRAG=120
# Notification banner: pinger auto-posts, consent auto-allows -> shade heads-up.
EXPECT='zelto-shade\|' \
run_shot 20-banner "Heads-up notification banner (auto-posted + auto-granted)" 10 \
    SIM_APP=zelto-pinger ZELTO_PINGER_POST=1 ZELTO_CONSENT_AUTO=allow

# ---------------------------------------------------------------------------
# TRANSITION FREEZE-FRAMES (P32): each transient surface's entrance pinned mid-
# flight via a ZELTO_*_ENTER=<0..1> hook, so the slide+fade is shot-verifiable.
# ---------------------------------------------------------------------------
SEED='sys.volume\t7\n' \
NOMARKER='as 12: the volume HUD carries no text, and this frame is about how far up it has slid — a POSITION, which only a pixel comparison against the settled HUD can speak to.' \
PIXEL='12-volume-hud all 0.15' \
    run_shot 42-volume-enter "Volume HUD mid slide+fade entrance (frozen 0.5)" 6 \
    ZELTO_VOLUME_ENTER=0.5
EXPECT='zelto-consent\|Allow' \
run_shot 43-consent-enter "Consent modal mid slide-up+fade entrance (frozen 0.5)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications" ZELTO_CONSENT_ENTER=0.5
# Deterministic: ZELTO_BANNER_DEMO fabricates the banner in the shade sink, so the
# entrance is verifiable without the flaky post->consent->grant->deliver dance.
EXPECT='zelto-shade\|Ping' \
run_shot 44-banner-enter "Heads-up banner mid slide+fade entrance (frozen 0.5)" 6 \
    ZELTO_BANNER_DEMO=1 ZELTO_BANNER_ENTER=0.5
EXPECT='zelto-launcher\|' \
run_shot 45-toast-enter "Launcher toast mid slide-up+fade entrance (frozen 0.5)" 6 \
    ZELTO_TOAST_ENTER=0.5

# State-change cross-fades (P32 item 2): a toggle's on/off fill animates instead
# of hard-swapping — pinned mid cross-fade via ZELTO_QS_ANIM. Plus the P31 press
# flash verified over a bottom-nav button (ZELTO_PRESS_APP scopes it to the nav).
EXPECT='zelto-shade\|Airplane' \
run_shot 46-qs-crossfade "Control Center toggles mid on/off cross-fade (frozen 0.5)" 6 \
    ZELTO_SHADE_OPEN=cc ZELTO_QS_ANIM=0.5
# NB the ZELTO_SETTINGS_SCREEN=network. P42 turned Settings into a drill-down, so
# the root screen is now an index of rows with chevrons and NO toggles on it at
# all — this shot kept its hook, kept resolving, and quietly went back to
# photographing a screen with nothing on it that could cross-fade. A toggle shot
# has to name the screen the toggles moved to.
EXPECT='zelto-settings\|' \
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
#
# P48: 140 -> 246. The SUGGESTION STRIP (ZELTO_SUGGEST_H, 105) sits above the keys
# now, so the whole grid slid down by that much: the home row ('g', centre of the
# grid) is at surface-local 207..284, centre 246. Measured off ZELTO_PROBE_TAPS on
# the same boot the strip is up (empty field, so the strip is present but blank).
# The X stays: 'g' is still at 331..389, centre 360.
EXPECT='zelto-keyboard\|space' \
# UPGRADED FROM `spot` TO A REAL BOX (P49 item 4). `spot` says "the change was
# small and concentrated SOMEWHERE", which was the honest best a client-side probe
# could manage — the probe reports surface coordinates and a PNG is the screen.
# zcomp now prints where it put the surface (ZELTO_SURFACE_LOG, set on every
# shot's boot), so the check reads the box out of THIS boot's own log and asks
# whether the change is inside the keyboard. The min-delta is low because it is a
# mean over the WHOLE 720x474 surface and the veil is one 59x77 cap of it — about
# 1.3% of the box — so a threshold sized for the peak would never be reached.
PIXEL='13-keyboard @ns:kbd_body 0.15' \
run_shot 48-key-press "Keyboard: a key pressed (touch-down highlight veil)" 8 \
    ZELTO_KBD_SHOW=1 SIM_APP=zelto-notepad \
    ZELTO_PRESS_APP=kbd_body ZELTO_PRESS_X=360 ZELTO_PRESS_Y=246

# App-open continuity (P32 item 3b): the tapped tile drifts toward centre while the
# rest of home fades — the launch hand-off, frozen mid-flight.
EXPECT='zelto-launcher\|All clear' \
run_shot 49-home-launch "App-open cue: tapped tile drifts + home fades (frozen 0.6)" 6 \
    ZELTO_HOME_LAUNCH=1

# In-app Navigator push, unified on the STANDARD token with a coordinated slide +
# cross-fade (P32 item 3a), frozen mid-push.
EXPECT='zelto-hello\|' \
run_shot 50-nav-push "Navigator push: detail slides + cross-fades in (frozen 0.45)" 8 \
    SIM_APP=zelto-hello ZELTO_NAV_PUSH=0.45

# Reduce Motion (P32 item 4): with sys.reduce_motion=1 the entrance spring is
# collapsed, so the SAME ENTER=0.5 request that half-fades the HUD in shot 42 now
# shows it fully seated — a still A/B proof that springs are suppressed.
SEED='sys.volume\t7\nsys.reduce_motion\t1\n' \
NOMARKER='as 12/42: no text on the HUD. The whole claim is that the entrance spring is COLLAPSED, which is a position, and it is read against 42 as an A/B pair — now measured rather than eyeballed.' \
PIXEL='42-volume-enter all 0.15' \
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
EXPECT='zelto-settings\|' \
run_shot 52-scroll-overpull "Scroll edge rubber-band: list over-pulled off the top" 8 \
    SIM_APP=zelto-settings ZELTO_SCROLL_OVERPULL=160
# Swipe-to-dismiss transient surfaces, each dragged partway toward dismissal.
SEED='sys.volume\t7\n' \
NOMARKER='as 12: no text on the HUD; the claim is the drag offset, measured against the undragged HUD.' \
PIXEL='12-volume-hud all 0.15' \
    run_shot 53-volume-dismiss "Volume HUD dragged up toward swipe-dismiss (frozen)" 6 \
    ZELTO_VOLUME_SHOW=1 ZELTO_VOLUME_DRAG=-70
EXPECT='zelto-shade\|Ping' \
run_shot 54-banner-dismiss "Heads-up banner dragged up toward swipe-dismiss (frozen)" 6 \
    ZELTO_BANNER_DEMO=1 ZELTO_BANNER_DRAG=-64
EXPECT='zelto-launcher\|' \
run_shot 55-toast-dismiss "Launcher toast dragged down toward swipe-dismiss (frozen)" 6 \
    ZELTO_TOAST_ENTER=1 ZELTO_TOAST_DRAG=60
# Consent: dragged down partway (a downward flick past threshold maps to Deny).
EXPECT='zelto-consent\|Allow' \
run_shot 56-consent-drag "Consent modal dragged down toward flick-to-Deny (frozen)" 7 \
    SIM_CONSENT="os.zelto.pinger notifications" ZELTO_CONSENT_DRAG=90
# Interruptible Navigator back-swipe: the top screen dragged partway back, the
# incoming screen sliding under it (ZELTO_NAV_BACK=<0..1> = how far the finger is).
EXPECT='zelto-hello\|' \
run_shot 57-nav-back-swipe "Navigator back-swipe tracking the finger (frozen 0.5)" 8 \
    SIM_APP=zelto-hello ZELTO_NAV_BACK=0.5
# Control Center over-pulled past fully-open, resisting with the rubber-band.
EXPECT='zelto-shade\|Airplane' \
run_shot 58-shade-overpull "Control Center pulled past open, rubber-banding at the limit (frozen)" 6 \
    ZELTO_SHADE_PULL=1.25
# Reduce Motion A/B: the release animation collapses, but the 1:1 drag itself is
# intact — the SAME volume drag as 53, still shown at the frozen finger position.
SEED='sys.volume\t7\nsys.reduce_motion\t1\n' \
NOMARKER='as 53: the HUD has no text and the claim is where the finger left it. The A/B against 53 is the point of the shot, so it is the pixel control too.' \
PIXEL='12-volume-hud all 0.15' \
    run_shot 60-reduce-drag "Reduce Motion: drag tracks finger, release would snap (vs 53)" 6 \
    ZELTO_VOLUME_SHOW=1 ZELTO_VOLUME_DRAG=-70
# Hit-test on a MOVING subtree: the Control Center is frozen OVER-PULLED (slid
# down past open by the rubber-band) and the press freeze-hook is aimed at a
# toggle AT ITS LIVE offset position. The press veil landing on the (opaque,
# translated) disc proves hit_test uses the offset frame — a moving subtree stays
# tappable where it visually is, not at its un-shifted layout home. It aims at an
# OFF toggle (Lock, row 2 centre): the veil is a white wash, so on an ON toggle —
# a near-white PRIMARY disc — there would be nothing to see.
EXPECT='zelto-shade\|Lock' \
run_shot 61-hit-test-moving "Hit-test while moving: press lands on the offset Lock toggle" 6 \
    ZELTO_SHADE_PULL=1.18 ZELTO_PRESS_APP=shade_body \
    ZELTO_PRESS_X=360 ZELTO_PRESS_Y=232

# ===========================================================================
# STATUS BAR STATES (seed the brokered sys.* the bar reads; home behind it)
# ===========================================================================
SEED='sys.wifi\t1\nsys.airplane\t0\nsys.signal\t4\nsys.brightness\t5\n' \
NOMARKER='the bars radios and battery are GLYPHS (system/common/glyphs.h) and its only text is the clock, so EXPECT has nothing to match; a PIXEL check on the strip was written and DELETED on the evidence (a glyph moves the 720x81 box by 0.13-0.33 and the clock in the same box moves it more). LOGSAYS is the third channel: the bar reports the marks it built.' \
LOGSAYS='\[bar\] marks radio=cellular:4 wifi=on .* charging=no' \
    run_shot 21-bar-wifi-bright "Status bar: full cellular + Wi-Fi, brightness high" 6
# Weak cellular: the unlit bars stay drawn (TEXT_FAINT), so the mark keeps its
# silhouette at every level instead of shrinking.
SEED='sys.wifi\t1\nsys.airplane\t0\nsys.signal\t1\nsys.brightness\t5\n' \
NOMARKER='as 21: a cellular level is four drawn bars, not a string.' \
LOGSAYS='\[bar\] marks radio=cellular:1 wifi=on' \
    run_shot 21a-bar-signal-low "Status bar: one cellular bar lit of four" 6
# Airplane mode REPLACES the bars with the plane (the radios are off, so a signal
# reading beside it would state the opposite of the truth).
# Wi-Fi is seeded ON here, deliberately. It used to be seeded off, and that made
# the shot unable to fail the thing it is named for: with the radio already down,
# "airplane mode turns the radios off" and "airplane mode does nothing" produce
# the same frame and the same marks. Found by negative-testing the LOGSAYS channel
# — removing the override from the bar changed nothing, because there was nothing
# to override. Seeded on, `wifi=off` can only come from the override.
SEED='sys.wifi\t1\nsys.airplane\t1\nsys.brightness\t2\n' \
NOMARKER='as 21: airplane mode replaces the bars with a drawn plane. The mark is the claim AND so is the absence of the other one: radio=airplane can only be reported by the branch that drops the cellular glyph, and wifi=off proves airplane overrode the radios rather than merely adding a plane beside them.' \
LOGSAYS='\[bar\] marks radio=airplane wifi=off' \
    run_shot 22-bar-airplane "Status bar: airplane mode, brightness low" 6
SEED='sys.battery_pct\t8\nsys.battery_charging\t0\n' \
NOMARKER='as 21: the battery is a drawn cell; the percentage is not printed in the bar, which is exactly why the bar has to report it.' \
LOGSAYS='\[bar\] marks .* battery=8 charging=no' \
    run_shot 23-bar-lowbatt "Status bar: low battery (8%)" 6 ZELTO_FAKE_BATTERY=0
SEED='sys.battery_pct\t64\nsys.battery_charging\t1\n' \
NOMARKER='as 21: charging is a bolt drawn inside the battery cell.' \
LOGSAYS='\[bar\] marks .* battery=64 charging=yes' \
    run_shot 24-bar-charging "Status bar: charging (64%)" 6 ZELTO_FAKE_BATTERY=0

# ===========================================================================
# SHIPPED APPS (auto-launched via SIM_APP)
# ===========================================================================
EXPECT='zelto-cards\|' \
run_shot 30-app-cards    "App: Cards"    8 SIM_APP=zelto-cards
EXPECT='zelto-notes\|' \
run_shot 31-app-notes    "App: Notes"    8 SIM_APP=zelto-notes
EXPECT='zelto-notepad\|Add note' \
run_shot 32-app-notepad  "App: Notepad"  8 SIM_APP=zelto-notepad
EXPECT='zelto-fetch\|' \
run_shot 33-app-fetch    "App: Fetch"    8 SIM_APP=zelto-fetch
EXPECT='zelto-settings\|Network' \
run_shot 34-app-settings "App: Settings, root list (drill-down rows)" 8 \
    SIM_APP=zelto-settings
# The DETAIL screens behind the root's chevrons (P42). Settings used to be one
# flat scroll, so this pair used to be "the top of the list" and "the list
# scrolled to its end" (ZELTO_SCROLL_TO=1500) — a shot that reviewed the same
# screen twice. Now each subject is its own pushed screen, reached by an env hook
# rather than a tap so the catalogue stays reproducible.
EXPECT='zelto-settings\|Brightness Boost' \
run_shot 34a-settings-display "Settings: Display & Sound (brightness SLIDER)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=display
EXPECT='zelto-settings\|Dim After' \
run_shot 34b-settings-lock "Settings: Lock Screen detail (toggles + steppers)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock
EXPECT='zelto-settings\|Shown on the Home and Lock screens\.' \
run_shot 34c-settings-wallpaper "Settings: Wallpaper picker detail screen" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=wallpaper
EXPECT='zelto-store\|' \
run_shot 35-app-store    "App: Store"    8 SIM_APP=zelto-store
EXPECT='zelto-hello\|' \
run_shot 36-app-hello    "App: Rows (SDK sample)" 8 SIM_APP=zelto-hello
EXPECT='zelto-pinger\|' \
run_shot 37-app-pinger   "App: Pinger"   8 SIM_APP=zelto-pinger
# A Zelto Script app: one shared runtime binary, so it is launched by .js path
# (SIM_SCRIPT) rather than by binary name (SIM_APP).
EXPECT='zelto-script\|' \
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
EXPECT='zelto-script\|' \
run_shot 70-script-pan "Script: card dragged by onPan (mid-drag, finger-tracked)" 7 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_DRAG="180 418 520 418 6000"

# onLongPress: a hold in place past the threshold toggles "Pinned" (and suppresses
# the tap the release would otherwise have produced).
EXPECT='zelto-script\|' \
run_shot 71-script-longpress "Script: onLongPress pinned the card (tap suppressed)" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 418 900"

# Navigator: a pushed screen, with its own hook state and the props it was pushed
# with. Back (edge-swipe / Escape) pops it without the script's help.
EXPECT='zelto-script\|' \
run_shot 72-script-nav "Script: Navigator pushed a second screen" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 916 100"

# TextField: tapping the field focuses it and raises the system on-screen keyboard
# (P21) — the app handles no keys at all.
EXPECT='zelto-keyboard\|space' \
run_shot 73-script-textfield "Script: TextField focused, on-screen keyboard up" 12 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="360 557 100"

# Networking: the script awaits the `network` grant (the system consent modal runs
# on the live loop), then fetches over the async state machine. SIM_NET=1 serves
# the endpoint locally; the card shows the real 200 + body.
EXPECT='zelto-script\|' \
run_shot 74-script-net "Script: fetch() after awaiting the network grant" 14 \
    SIM_SCRIPT="$JSDEMO" SIM_NET=1 ZELTO_CONSENT_AUTO=allow \
    ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="190 697 100"

# Notifications: posted by the script through the same broker a C app uses, with an
# action button that routes back to it. Captured as the heads-up banner.
# The press is LATE on purpose: a heads-up banner dwells for a few seconds and
# then stands down into the Notification Center, so tapping Notify early enough
# to be comfortable means photographing the screen after the banner has gone.
EXPECT='zelto-shade\|' \
run_shot 75-script-notify "Script: notification posted (heads-up banner + action)" 13 \
    SIM_SCRIPT="$JSDEMO" ZELTO_CONSENT_AUTO=allow \
    ZELTO_TAP_APP=os.zelto.jsdemo ZELTO_TAP_LABEL=Notify ZELTO_TAP_AT=11000

# Settings: the script writes sys.mute and the broker echoes the change back to its
# observer, which recolours the row — the same live fan-out the shade gets.
EXPECT='zelto-script\|' \
run_shot 76-script-settings "Script: wrote sys.mute, observer echoed it back live" 11 \
    SIM_SCRIPT="$JSDEMO" ZCOMP_INPUT_DELAY=6500 ZCOMP_HOLD="527 758 100"

# Packaging: the Greeter is not in the image at all. It is packaged as a signed
# .zap and installed onto the persistent disk first (ZAP=), so the boot below knows
# the app only from what the installer left there — a script app running from
# /var/zelto, whose code was signature- and hash-verified before it ever ran. The
# tap lands on its tile, which exists only because the install worked.
#
# BY LABEL, NOT BY COORDINATE (P46). This held ZCOMP_HOLD="447 460", and the
# catalogue audit found the Greeter's tile is at x=239 y=560 — so the hold landed
# on nothing, the app never launched, and the shot photographed a home screen
# with a tile on it under a name promising the app RUNNING. ZELTO_TAP_LABEL asks
# the layout where "Greeter" is, so it moves when the tile does.
ZAP="$REPO_ROOT/system/apps/greeter/zelto-greeter.app:$REPO_ROOT/system/apps/greeter/greeter.js" \
EXPECT='zelto-script\|' \
run_shot 77-script-installed "Script: installed from a signed .zap, running from /var/zelto" 12 \
    ZELTO_TAP_APP=os.zelto.launcher ZELTO_TAP_LABEL=Greeter ZELTO_TAP_AT=6000

# THE SUGGESTION STRIP HAS A FRAME NOW (P49 item 5). It shipped in P48 as new,
# visible, user-facing UI and the catalogue never photographed it working:
# 13-keyboard raises the keyboard with ZELTO_KBD_SHOW and no focused field, so
# there is no word being typed, no suggestions, and the one state of the strip
# that proves nothing. These two need a real focused field and a PART-TYPED word
# still under the cursor at capture — SIM_APP=zelto-notepad with a ZELTO_KBD_TAP
# that stops mid-word.
#
# ZELTO_NOTEPAD_ECHO=1 IS WHAT FOCUSES THE FIELD, and it is not optional here.
# Without a focused field the compositor never activates the input method, the
# keyboard never gets its show handshake, and ZELTO_KBD_TAP never arms — the shot
# then comes out as a perfectly ordinary keyboard with a BLANK STRIP, which is
# exactly the frame 13-keyboard already is and the one these two exist to stop
# being the only one. (That is how the first run of these shots failed, and the
# EXPECT is what caught it: the PNG looked fine.) NOAUTOCAP goes with it so the
# words in the strip are the ones the caption names rather than "Friend".
#
# ASSERTED BY EXPECT, not by pixels. Unlike a press veil the strip is made of
# TEXT, so the strongest claim available is the one that reads the laid-out
# strings — and the string named is the one in the LAST slot filled, which can
# only be there if all three were.
#
# AND THE WORD NAMED COMES FROM THE BOOT, NOT FROM A DESK CALCULATION. The first
# version of these two named "fire" and "tea", which is what z_lm_candidates
# returns with NO substitution callback. The real keyboard supplies one — the
# LAYOUT's adjacency cost (kbd_subst_cost) — and it reranks the third slot to
# "from" and "try", because 'r' sits next to 'e' on this grid and a near-miss
# costs half a substitution. The EXPECT caught that; the PNG looked perfectly
# healthy, which is the whole reason these declare a marker at all.
EXPECT='zelto-keyboard\|from' \
run_shot 78-keyboard-suggest "Keyboard: the suggestion strip mid-word (friend / frien / from)" 10 \
    SIM_APP=zelto-notepad ZELTO_NOTEPAD_ECHO=1 ZELTO_NOTEPAD_NOAUTOCAP=1 \
    ZELTO_KBD_TAP="f,r,i,e,n"

# ...and the PENDING AUTOCORRECTION, which is a second state of the same strip
# and the one that changes what the space bar will do. "teh" is not a word and is
# not a live prefix of one, so the boundary WOULD correct it — the strip says so
# by filling and tinting slot 0 (Z_COLOR_PRIMARY on Z_COLOR_SURFACE_4), the way
# iOS bolds the word it is about to apply. The middle slot is the literal, which
# is how the correction is rejected.
EXPECT='zelto-keyboard\|try' \
run_shot 79-keyboard-suggest-pending "Keyboard: a pending autocorrection (the / teh / try)" 10 \
    SIM_APP=zelto-notepad ZELTO_NOTEPAD_ECHO=1 ZELTO_NOTEPAD_NOAUTOCAP=1 \
    ZELTO_KBD_TAP="t,e,h"

# --- Dynamic Type (P50) ------------------------------------------------------
# THE CATALOGUE AT THE LARGEST TEXT SIZE, and it is a CHOSEN SUBSET rather than a
# second pass over all 79. The reasoning, because "we did some of them" is not a
# decision unless the rule is written down:
#
# A second full pass costs ~20 minutes and doubles a run that is already 15, and
# it would spend nearly all of that photographing surfaces where the answer is
# structurally known — an app whose whole screen is a Spacer and three centred
# labels cannot clip, and a shot of a mid-flight spring at a bigger text size is
# a picture of the same spring. What the largest size can actually break is a
# FIXED BOX HOLDING TEXT, so the subset is exactly those: every surface the P50
# overflow audit flagged, plus the grouped inset list (the densest fixed-height
# rows in the OS), plus the two surfaces that must NOT change.
#
# The audit itself is not a screenshot and does not live here: test/
# test_text_size_overflow_sim.sh boots every surface at the largest size with the
# probe on and fails on a measured overflow. These shots are the design review of
# the same states — what the audit cannot say is whether the result looks like a
# phone.
TS_BIG='sys.text_size	6
'
TS_SMALL='sys.text_size	0
'
# P51 added five ACCESSIBILITY steps above TS_BIG. TS_AX is the top of the range
# (AX5) and TS_AX_LAST_FLAT is the last step that does NOT reflow (AX1) — the two
# exist as a PAIR, because the phase's claim is about a break and a break needs
# both sides of it in the sheet.
TS_AX='sys.text_size	11
'
TS_AX_LAST_FLAT='sys.text_size	7
'

# The new screen, at the default size first: a slider made of the thing it
# changes, plus the two toggles. Reduce Motion has existed since P31 and this is
# the first time it has been photographable, because it is the first time it has
# been reachable.
EXPECT='zelto-settings\|Accessibility' run_shot 34d-settings-accessibility "Settings: Accessibility (text size slider, Bold Text, Reduce Motion)" 8     SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=accessibility

# The grouped inset list at both ends of the STANDARD range. 44pt rows hold Body,
# and Body runs 26 units at the smallest step to 42 at the largest — the row is
# the same 81 either way (the touch-target floor still wins across the standard
# range), which is a claim worth being able to LOOK at. Past it, see 87/88.
SEED="$TS_BIG" EXPECT='zelto-settings\|Accessibility' run_shot 80-type-largest-settings "Text Size at the largest step: the grouped list" 8     SIM_APP=zelto-settings
SEED="$TS_SMALL" EXPECT='zelto-settings\|Accessibility' run_shot 81-type-smallest-settings "Text Size at the smallest step (Caption2 floors)" 8     SIM_APP=zelto-settings

# The two surfaces the audit found CLIPPING, both now derived from the face.
# 82: the consent alert's buttons were a bare 46 against a 61-unit line box, in
# the one dialog where reading the two answers is the entire point — and the card
# itself was 44 units taller than the number the compositor blurred behind it.
SEED="$TS_BIG" EXPECT='zelto-consent\|Allow' run_shot 82-type-largest-consent "Text Size largest: consent alert (buttons derived from the face)" 7     SIM_CONSENT="os.zelto.pinger notifications"
# 83: the switcher's card headers were 30 units holding a 35-unit Subhead line,
# from the day the deck shipped, at every text size.
SEED="$TS_BIG" EXPECT='zelto-recents\|' run_shot 83-type-largest-switcher "Text Size largest: App Switcher card headers" 9     SIM_APP=zelto-notes SIM_EXTRA="zelto-cards zelto-fetch" SIM_RECENTS=1

# 84: the home grid, where an app NAME comes out of a manifest into a 158-unit
# cell. Ellipsized now — "andemu Demo" wants 206 at this size.
SEED="$TS_BIG" EXPECT='zelto-launcher\|Notepad' run_shot 84-type-largest-home "Text Size largest: home grid (manifest names ellipsized)" 6

# 85 IS THE OPT-OUT SHOT and it is the most important one here, because it is the
# only one whose subject is something NOT happening. The keyboard is photographed
# at the largest text size over a Notepad that IS scaling: the caps must be
# identical to 13-keyboard while the app behind them is not. A keyboard that
# followed the setting would grow its caps and walk the bottom row off its own
# surface, which is the P48 bug with a new cause.
SEED="$TS_BIG" EXPECT='zelto-keyboard\|space' run_shot 85-type-largest-keyboard "Text Size largest: the keyboard does NOT scale (cf. 13)" 8     ZELTO_KBD_SHOW=1 SIM_APP=zelto-notepad

# 86: Bold Text, which is the same seam's other term — a floor on WEIGHT applied
# where the shaper is set, so measure and paint cannot disagree.
SEED='sys.bold_text	1
' EXPECT='zelto-settings\|Accessibility' run_shot 86-type-bold-settings "Bold Text on: every Regular weight raised to Semibold" 8     SIM_APP=zelto-settings

# ===========================================================================
# THE ACCESSIBILITY SIZES (P51) — and 87/88 are ONE ARGUMENT IN TWO PICTURES
# ===========================================================================
# The five AX steps are not a bigger number of the same kind. Past AX2 a label
# and its control stop fitting side by side, so a row is no longer a HEIGHT — it
# is a LAYOUT DECISION, and z_text_size_reflows() is the one predicate that makes
# it. The break was measured on this exact screen: at AX1 the widest stepper
# row's '+' key ends at x=671 on a 720 screen, and at AX2 it starts at 701.
#
# 88 IS THE CONTROL FOR 87. A reflowed screen on its own only shows that
# something changed; the pair shows WHERE, and that everything below the break
# still looks like the list it was. Read them together or neither says anything.
#
# The EXPECTs come from the boots themselves, which matters more here than
# anywhere else in the file: at AX5 'Passcode (1234)' is no longer one string —
# it wraps, and the probe reports 'Passcode' and '(1234)' as two Texts. An EXPECT
# written at a desk would have been asserting on a string that no longer exists.
SEED="$TS_AX" EXPECT='zelto-settings\|Dim After' \
run_shot 87-ax-settings-reflow "AX5: the row reflows — label wrapped, control beneath it" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock
SEED="$TS_AX_LAST_FLAT" EXPECT='zelto-settings\|Screen Off After' \
run_shot 88-ax-settings-last-flat "AX1: the last step that does NOT reflow (cf. 87)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=lock

# 89: the home grid at AX5. A widget's 2x1 bento cell is 298 units and its
# weekday measures 371, its '100%' 324, its 'All clear' 379 — none of them prose
# that could wrap or an identifier that could ellipsize. So a widget takes the
# whole row (entry_span), which is the same reflow in the only shape a grid has.
# 'All clear', not the weekday. The EXPECT here used to be 'Thursday' — which is
# the day P51 wrote it on, so the claim passed one day in seven and failed the
# other six. (Found in P52's re-shoot, on a Friday: the surface rendered
# perfectly and the assertion about it was what had rotted.) 'All clear' is on
# the same widget row, is one of the strings that MOTIVATED the reflow — it
# measures 379 units against a 298-unit cell — and does not depend on when the
# catalogue is run. The standing rule is that an EXPECT is computed from a run
# rather than at a desk; this one was, and the run happened to be on a Thursday.
SEED="$TS_AX" EXPECT='zelto-launcher\|All clear' \
run_shot 89-ax-home-widgets "AX5: home widgets go full-width (a glance needs the row)" 6

# 90: the screen that SETS the size, at the size it sets. Its slider legend is
# two 'A's at fixed STEPS — which is not two fixed SIZES: Font() routes through
# z_font_units() like everything else, so the caps grow and the row grows with
# them. What a fixed step preserves is the CONTRAST between them, which is the
# whole information content of a legend.
SEED="$TS_AX" EXPECT='zelto-settings\|TEXT SIZE' \
run_shot 90-ax-accessibility "AX5: Accessibility (the legend keeps its ratio, the row gives)" 8 \
    SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN=accessibility

# 91: Increase Contrast, on the surface that motivated it. Z_COLOR_TEXT_FAINT is
# the chevron on every row here and the disabled detail column, and it measured
# 2.67:1 on a card where AA wants 4.5. Photographed at the SMALLEST text size,
# deliberately: small grey type on a grey card is the two accessibility problems
# compounding, and it is the frame the setting exists for.
SEED='sys.increase_contrast	1
sys.text_size	0
' EXPECT='zelto-settings\|Accessibility' \
run_shot 91-increase-contrast "Increase Contrast on, at the smallest text size (cf. 81)" 8 \
    SIM_APP=zelto-settings

# ===========================================================================
# P53 — the content pipeline: capture, the notification, the library.
# ===========================================================================

# 92 photographs THE SCREENSHOT ITSELF. The capture chord fires at 5s; what is on
# screen at 9s is the heads-up banner carrying a thumbnail of the very frame that
# was captured. This is the only place in the OS where a notification shows a
# PICTURE rather than the poster's icon, and the two are deliberately both
# present — the icon says who, the thumbnail says which.
EXPECT='zelto-shade\|Screenshot saved' \
run_shot 92-screenshot-banner "Screenshot saved: the heads-up banner with its thumbnail" 9 \
    ZCOMP_SHOT_AT=5000

# 93/94 are the Photos app, and 93 needs a POPULATED library while run_shot gives
# every shot a fresh data dir on purpose (no cross-shot bleed). Taking the
# screenshots inside the same boot does not work either: a heads-up banner has no
# dwell timer, so three captures leave three cards sitting over the grid that is
# meant to be the subject.
#
# So the library is pointed at a directory prepared here, through the same
# $ZELTO_PHOTOS_ROOT override the tests use. WHAT IS IN THE PICTURES IS NOT THE
# SUBJECT: this shot is about the GRID — a cell derived from the column count and
# the spacing scale, three columns, a centre crop — and that is the same layout
# whatever the images are. The thumbs directory gets the same files, because a
# missing thumbnail falls back to the full image and the fallback is not what is
# being reviewed here.
PHOTOLIB="$TMP/photolib"
mkdir -p "$PHOTOLIB/photos" "$PHOTOLIB/thumbs"
pl_i=0
for wp in "$REPO_ROOT"/resources/wallpaper/*.png; do
    [ -f "$wp" ] || continue
    [ "$pl_i" -ge 5 ] && break
    # A valid library id: 13 digits of epoch-ms then a 2-digit sequence, which is
    # what makes a reverse sort chronological (system/common/photos.h).
    pl_id="$(printf '17849%08d-00' "$((10000000 + pl_i))")"
    cp "$wp" "$PHOTOLIB/photos/$pl_id.png"
    cp "$wp" "$PHOTOLIB/thumbs/$pl_id.png"
    pl_i=$((pl_i + 1))
done

EXPECT='zelto-photos\|5 photos' \
run_shot 93-photos-grid "Photos: the library grid (3 columns, cell derived, centre-cropped)" 8 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB"

# 94: the empty library. Worth its own frame because it is the state a new phone
# is in, and because it is the one screen here made of PROSE — the sentence under
# "No photos" is a WrapText, which is what the AX audit will exercise.
EXPECT='zelto-photos\|No photos' MUSTNOT='zelto-photos\|5 photos' \
run_shot 94-photos-empty "Photos: nothing captured yet (the empty state)" 8 \
    SIM_APP=zelto-photos

# 95/96: the viewer and its one irreversible action. Reached by the env hook
# (ZELTO_PHOTOS_VIEW), never by a tap coordinate; the delete card by the label
# sequence, which is also the only way to photograph a screen that exists for one
# tap and then goes away.
#
# The position counter is the marker rather than a filename: "2 of 5" says the
# viewer is on a specific photo of a known library, which a picture alone cannot.
EXPECT='zelto-photos\|2 of 5' \
run_shot 95-photos-viewer "Photos: the full-size viewer (swipe between, three actions)" 9 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB" ZELTO_PHOTOS_VIEW=1

EXPECT='zelto-photos\|Delete photo?' \
run_shot 96-photos-delete "Photos: the delete confirmation (the only irreversible action)" 12 \
    SIM_APP=zelto-photos ZELTO_PHOTOS_ROOT="$PHOTOLIB" ZELTO_PHOTOS_VIEW=1 \
    ZELTO_TAP_LABEL=Delete ZELTO_TAP_APP=os.zelto.photos ZELTO_TAP_AT=9000

# 97: the camera. The preview is a COLOUR-BAR TEST SIGNAL and that is
# deliberate — there is no camera in the simulator, so the source is synthetic
# (sdk/src/camera.c), and an honest fake should look unmistakably like a test
# pattern rather than like a photograph the OS took of something.
#
# LOGSAYS carries the other half of this frame: the green in-use dot in the
# status bar. The dot is DRAWN, so it has no string for EXPECT to match, and it
# is 10 units across — well under the noise a glyph or the clock already puts
# into the 720x81 strip, so a pixel check could not see it either. The bar's own
# marks line is the mechanism P47 added for exactly this case.
EXPECT='zelto-camera\|Camera' LOGSAYS='camera=in-use:[1-9]' \
run_shot 97-camera "Camera: the live preview, its shutter, and the in-use dot" 10 \
    SIM_APP=zelto-camera ZELTO_CONSENT_BIN=/bin/true

# 98: the same screen with the permission REFUSED. The state an app has to
# handle gracefully and the one nobody photographs.
EXPECT='zelto-camera\|Camera access denied' run_shot 98-camera-denied "Camera: permission denied (no stream is opened)" 10     SIM_APP=zelto-camera ZELTO_CONSENT_BIN=/bin/false

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

# The catalogue's own credibility check. A missing PNG is already reported per
# shot; this is the other failure, the one that used to be invisible: a frame
# that exists, diffs healthily, and is not a picture of what it is called.
if [ "$MARKER_FAIL" != 0 ]; then
    echo
    echo "!! ${#MARKERLESS[@]} shot(s) did not show what they claim:"
    for m in "${MARKERLESS[@]}"; do echo "     $m"; done
    echo "   A shot of the wrong screen has a perfectly healthy delta. Fix the"
    echo "   recipe (seed the state it needs), or fix the claim."
    exit 1
fi
