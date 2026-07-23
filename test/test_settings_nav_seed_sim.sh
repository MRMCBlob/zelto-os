#!/usr/bin/env bash
# test_settings_nav_seed_sim — the Navigator's seeded push actually lands.
#
# WHY THIS EXISTS, and it is a flake investigation that came back with a
# different answer than expected.
#
# `34c-settings-wallpaper` has been 3-pass/2-fail over five full catalogue runs
# since before P49, identically on the pre-P49 tree. When it fails it photographs
# the Settings ROOT: the screen ZELTO_SETTINGS_SCREEN asked for was never built.
# Four shots in the catalogue depend on that hook, so a hook that silently does
# nothing 40% of the time is four shots that can photograph the wrong screen and
# report "ok".
#
# THE RECORDED CANDIDATE MECHANISM IS WRONG, and reading beats guessing here:
# z_navigator() sets nav->depth = 1 and overwrites stack[0] on its first call
# (sdk/src/navigation.c), so a z_nav_push arriving BEFORE that call is discarded
# — and settings' nav_seeded bool latches after one attempt, so it would never be
# retried. That is a real hazard and it is not this one. system/apps/settings/
# main.c builds `Navigator(app, .root = screen_root)` and THEN pushes, in that
# order, in the same body(), with a comment saying why. nav->inited is true
# before z_nav_push is ever reached.
#
# AND IT DOES NOT REPRODUCE. 23 boots, three configurations, zero failures: 14
# direct sim boots; 4 more at SHOT_DELAY=3 (deliberately capturing early, to
# catch a push whose transition had not settled); 5 through meta/shots.sh itself
# with ONLY=34c, so the marker check that reports the flake was the one doing the
# reporting. Whatever the full catalogue does to this shot — 33 boots of load
# ahead of it, an orphaned compositor — it is not a lost push, and the note in
# the P49/P50 memories saying otherwise is wrong.
#
# SO THIS FILE IS THE GUARD RATHER THAN THE FIX. There is no fix to make: a fix
# for an unreproduced mechanism is indistinguishable from nothing, which is the
# rule that sent this investigation to "disproven" instead of "probably fixed".
# What is worth having is the assertion the catalogue was implicitly making and
# could not check — every seeded screen is REACHED — run on every test run
# instead of during a 15-minute contact sheet.
#
# NEGATIVE-TESTED: dropping the z_nav_push in settings' seed block makes all
# three cases fail, each naming the screen it did not reach; pushing every screen
# to screen_root makes them fail on the string rather than on the surface.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="${BUILD:-$REPO_ROOT/build-host}"

if [ ! -x "$BUILD/compositor/zcomp" ]; then
    echo "SKIP: $BUILD/compositor/zcomp not built"
    exit 0
fi
if ! command -v grim >/dev/null 2>&1; then
    echo "SKIP: grim not installed; the simulator cannot complete a run"
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-navseed.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# <screen> <a string only that screen builds> <a string only the ROOT builds>
#
# The second string is the point. "the screen I asked for is present" and "the
# screen I did not ask for is absent" are different claims, and only the pair
# rules out the failure this test is named for: the root list also says
# "Wallpaper" (it is one of its six rows), so a check for the word alone passes
# on exactly the frame the flake produces.
seeded() {
    local screen="$1" want="$2" root_only="$3"
    local dir="$TMP/$screen"
    mkdir -p "$dir/data" "$dir/xdg"
    printf 'sys.brightness\t5\n' > "$dir/data/settings.conf"
    env HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=8 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP=os.zelto.settings \
        SIM_APP=zelto-settings ZELTO_SETTINGS_SCREEN="$screen" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1

    # The positive control: the app came up at all. Without it, a Settings that
    # crashed on launch satisfies "the root's own string is absent" perfectly.
    if ! grep -q 'probe taps:.*os.zelto.settings' "$dir/log"; then
        zt_fail "Settings never reported a probe summary when seeded onto '$screen' — the app did not come up, so neither assertion below is evidence" \
            "'probe taps: ... in os.zelto.settings'" "absent (see $dir/log)"
        return
    fi
    if ! grep -q "probe text \[zelto-settings\] '$want'" "$dir/log"; then
        zt_fail "ZELTO_SETTINGS_SCREEN=$screen did not reach that screen — the seeded z_nav_push was issued and the frame shows something else, which is the '34c photographed the root' failure" \
            "'$want' built by zelto-settings" "absent (see $dir/log)"
        return
    fi
    if grep -q "probe text \[zelto-settings\] '$root_only'" "$dir/log"; then
        zt_fail "ZELTO_SETTINGS_SCREEN=$screen built '$want' but the ROOT list is still on the surface too — the push has not finished, so a shot taken now photographs a transition rather than the screen it is named for" \
            "no '$root_only' on the settled frame" "present (see $dir/log)"
    fi
}

# The four screens the catalogue seeds, minus the two whose strings the root
# repeats verbatim. Each `want` is a string that exists ONLY on that screen.
seeded wallpaper "Shown on the Home and Lock screens." "Display & Sound"
seeded lock "Lock Now" "Display & Sound"
seeded accessibility "The quick brown fox." "Display & Sound"

zt_done
