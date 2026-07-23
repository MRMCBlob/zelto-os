#!/usr/bin/env bash
# test_text_size_sim — Dynamic Type, on a running system.
#
# test_dynamic_type.c pins the LADDER (the arithmetic in z_font_units). This
# pins the SEAM: that the number it produces actually reaches the shaper in
# nineteen surfaces none of which know a setting exists, that the three surfaces
# which opted out really did, and that a change made while the system is running
# reaches a process that never asked to hear about it.
#
# HOW IT MEASURES. Not by eye and not by a pixel delta — ZELTO_PROBE_TAPS prints
# every string on the settled frame with the box the layout gave it
# ("probe text [proc] 'Wallpaper' x=.. y=.. w=.. h=.."), and a Text node's box IS
# its shaped size. So "the type got bigger" is the width of a NAMED STRING in a
# NAMED PROCESS going up, which is a number, and "the keyboard did not follow"
# is the same number staying put in the same run.
#
# EVERY ABSENCE HAS A POSITIVE CONTROL FROM THE SAME BOOT. The claim "the status
# bar did not change" is worthless from a boot where nothing changed — a crashed
# sim, a settings file that never loaded and a working opt-out all produce it. So
# each opt-out assertion is paired with a scaling surface measured in the SAME
# pair of boots.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-textsize.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One boot. $1 = tag, $2 = the seeded sys.text_size (empty = leave it unset, i.e.
# a device nobody has configured), rest = extra environment.
#
# The probe is NOT scoped to one app: this test compares a keyboard, a status bar
# and an app in the same boot, and each probe line names its own process.
run_boot() {
    local tag="$1" size="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    {
        printf 'sys.brightness\t5\n'
        [ -n "$size" ] && printf 'sys.text_size\t%s\n' "$size"
    } > "$dir/data/settings.conf"
    env HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=9 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PROBE_TAPS=1 "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# The WIDTH the layout gave one string in one process, off that boot's probe.
# Prints nothing when the string is not on the surface, which every caller below
# treats as a failure rather than as a zero (a missing string measuring 0 would
# make "it did not grow" trivially true).
probe_w() {   # <log> <process> <string>
    sed -n "s/.*probe text \[$2\] '$3' x=[-0-9]* y=[-0-9]* w=\([0-9]*\).*/\1/p" \
        "$1" | head -1
}
probe_h() {   # <log> <process> <string>
    sed -n "s/.*probe text \[$2\] '$3' x=[-0-9]* y=[-0-9]* w=[0-9]* h=\([0-9]*\).*/\1/p" \
        "$1" | head -1
}

# Assert a string exists in both logs and grew / stayed put between them.
# `mode` is "grew" or "same".
compare() {   # <name> <mode> <logA> <logB> <process> <string> <getter>
    local name="$1" mode="$2" la="$3" lb="$4" proc="$5" str="$6" get="$7"
    local a b
    a="$($get "$la" "$proc" "$str")"
    b="$($get "$lb" "$proc" "$str")"
    if [ -z "$a" ] || [ -z "$b" ]; then
        zt_fail "$name: '$str' was not found in [$proc] on both boots, so there is nothing to compare — every claim below it would be vacuous" \
            "the string on both frames" "default='${a:-absent}' largest='${b:-absent}' (see $la / $lb)"
        return
    fi
    case "$mode" in
      grew)
        if [ "$b" -le "$a" ]; then
            zt_fail "$name: '$str' in [$proc] did not get bigger at the largest text size — the seam did not reach this surface" \
                "> $a" "$b (see $lb)"
        fi ;;
      same)
        if [ "$b" -ne "$a" ]; then
            zt_fail "$name: '$str' in [$proc] CHANGED with the text size, but this surface opted out of Dynamic Type (fixed_type) — its height is an exclusive-zone contract or a touch target" \
                "$a (unchanged)" "$b (see $lb)"
        fi ;;
    esac
}

# --- the two boots -----------------------------------------------------------
# One unconfigured device and one at the top of the range, same recipe: Settings
# in front (an app that observes settings), the keyboard raised over it (a layer
# surface that opted out), and the status bar (the other opt-out) always up.
BOOT_ARGS=(SIM_APP=zelto-settings ZELTO_KBD_SHOW=1
           ZELTO_SETTINGS_SCREEN=accessibility)
LOG_DEF="$(run_boot default "" "${BOOT_ARGS[@]}")"
LOG_BIG="$(run_boot largest 6 "${BOOT_ARGS[@]}")"

# The boots happened at all. Without this every comparison below fails with a
# confusing message about a missing string instead of a clear one about a
# missing surface.
for pair in "default $LOG_DEF" "largest $LOG_BIG"; do
    set -- $pair
    if ! grep -q 'probe taps:' "$2"; then
        zt_fail "the $1 boot never reported a probe summary — the system did not come up, so nothing below is evidence" \
            "'zelto: probe taps: ...'" "absent (see $2)"
        zt_done
    fi
done

# --- 1. an app scales --------------------------------------------------------
# Settings is a plain xdg toplevel that passes Z_FONT_* to Font() and has no idea
# sys.text_size exists. Two strings, one at Large Title and one at Body, so a
# change that only reached one step of the ladder would show.
compare "Settings" grew "$LOG_DEF" "$LOG_BIG" zelto-settings "Accessibility" probe_w
compare "Settings" grew "$LOG_DEF" "$LOG_BIG" zelto-settings "Bold Text" probe_w
compare "Settings" grew "$LOG_DEF" "$LOG_BIG" zelto-settings "Bold Text" probe_h

# --- 2. the keyboard does NOT ------------------------------------------------
# A cap's letter is a label for a finger. If it scaled, the caps would grow and
# the bottom row would walk off the surface — the P48 bug with a new cause.
compare "the keyboard" same "$LOG_DEF" "$LOG_BIG" zelto-keyboard "space" probe_w
compare "the keyboard" same "$LOG_DEF" "$LOG_BIG" zelto-keyboard "space" probe_h

# TWO ASSERTIONS AND NOT THREE, and the missing one is worth recording because
# writing it is the obvious next move for anyone reading this file.
#
# The first draft also compared the keyboard's SURFACE SIZE across the two boots
# ("a taller keyboard shrinks every app on the screen"). It passed with
# .fixed_type deliberately removed — and so did a replacement that counted cap
# labels falling OFF the surface. Both were wrong about the mechanism: a cap is a
# Frame at ZELTO_KEY_H, so scaling does not grow the caps or the surface at all.
# It grows the LETTERS INSIDE THEM, which is exactly what the two `same` checks
# above measure (with the opt-out removed, 'space' goes 73x35 -> 102x49 in a cap
# built for 73x35). An assertion that cannot fail for the reason its own message
# gives is worse than no assertion, so those two are gone rather than kept as
# decoration.

# --- 3. the status bar does NOT ----------------------------------------------
# Its height IS its exclusive zone, and the shade, the dim scrim and the volume
# HUD all offset by ZELTO_BAR_H. The clock's STRING differs between two boots a
# minute apart, so this measures the line box (h), not the width.
bar_def="$(sed -n "s/.*probe text \[zelto-bar\] '[^']*' x=[-0-9]* y=[-0-9]* w=[0-9]* h=\([0-9]*\).*/\1/p" "$LOG_DEF" | head -1)"
bar_big="$(sed -n "s/.*probe text \[zelto-bar\] '[^']*' x=[-0-9]* y=[-0-9]* w=[0-9]* h=\([0-9]*\).*/\1/p" "$LOG_BIG" | head -1)"
if [ -z "$bar_def" ] || [ -z "$bar_big" ]; then
    zt_fail "the status bar drew no text on one of the boots, so its opt-out cannot be checked" \
        "a clock on both frames" "default='${bar_def:-absent}' largest='${bar_big:-absent}'"
else
    zt_expect_eq "$bar_def" "$bar_big" \
        "the status bar's type followed the text size, but the bar opted out — its height is a cross-process exclusive-zone contract (see $LOG_BIG)"
fi

# --- 4. a LIVE change reaches a surface that never subscribed ----------------
# The launcher calls z_settings_observe exactly never. It gets the new size
# because libzelto itself subscribes in every process (app.c) and re-reads both
# accessibility keys on any settings_changed. Driven through the BROKER by
# Settings' ZELTO_SETTINGS_SET hook after boot — not seeded into settings.conf —
# so what is under test is the fan-out and the rebuild, not the startup read.
LOG_LIVE="$(run_boot live "" SIM_APP=zelto-settings \
    ZELTO_SETTINGS_SET=sys.text_size:6 ZELTO_PROBE_AT=8000)"
compare "the launcher (live)" grew "$LOG_DEF" "$LOG_LIVE" \
    zelto-launcher "Notepad" probe_w

# The same run's negative control: the bar is in it too and must still not have
# moved. Without this, a "live change" that was really a broken probe would pass.
bar_live="$(sed -n "s/.*probe text \[zelto-bar\] '[^']*' x=[-0-9]* y=[-0-9]* w=[0-9]* h=\([0-9]*\).*/\1/p" "$LOG_LIVE" | head -1)"
if [ -n "$bar_live" ]; then
    zt_expect_eq "$bar_def" "$bar_live" \
        "a live text-size change resized the status bar's type, which opted out (see $LOG_LIVE)"
fi

# --- 5. a garbage value is clamped, not obeyed -------------------------------
# sys.text_size is a brokered string any process can write. A corrupt store must
# give a legible screen, so the value clamps to the top of the range — which
# means this boot must match the largest one, NOT the default one.
LOG_JUNK="$(run_boot junk 99 "${BOOT_ARGS[@]}")"
w_junk="$(probe_w "$LOG_JUNK" zelto-settings "Bold Text")"
w_big="$(probe_w "$LOG_BIG" zelto-settings "Bold Text")"
w_def="$(probe_w "$LOG_DEF" zelto-settings "Bold Text")"
if [ -z "$w_junk" ]; then
    zt_fail "the out-of-range boot did not draw the Accessibility screen, so clamping cannot be checked" \
        "'Bold Text' on the frame" "absent (see $LOG_JUNK)"
else
    zt_expect_eq "$w_big" "$w_junk" \
        "sys.text_size=99 must clamp to the largest supported size (it rendered at neither the largest '$w_big' nor, hopefully, the default '$w_def')"
fi

zt_done
