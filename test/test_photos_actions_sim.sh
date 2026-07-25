#!/usr/bin/env bash
# test_photos_actions_sim — the three things the viewer can DO to a photo, and
# the one of them that is irreversible.
#
# WHAT IS UNDER TEST. Stage 2 of P53: delete-with-confirmation and
# set-as-wallpaper, driven by LABEL (ZELTO_TAP_LABEL) rather than by coordinates,
# which rot. Both are asserted on state that outlives the boot — a file that is
# gone, a brokered key that changed — rather than on a log line saying the
# handler ran, because a handler that runs and does nothing is the failure mode.
#
# WHY THE CONFIRMATION IS THE POINT. Delete is the only irreversible action in
# this OS with no undo behind it: there is no trash, the bytes are unlinked. So
# the test asserts BOTH halves — that one tap alone does NOT delete (the
# confirmation is a real gate, not decoration), and that confirming does. A
# confirmation nobody has proven blocks anything is a dialog, not a safeguard.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="${BUILD:-$REPO_ROOT/build-host}"

if [ ! -x "$BUILD/system/apps/photos/zelto-photos" ]; then
    echo "SKIP: $BUILD/system/apps/photos/zelto-photos not built"
    exit 0
fi
if ! command -v grim >/dev/null 2>&1; then
    echo "SKIP: grim not installed; the simulator cannot complete a run"
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-photos-act.XXXXXX")"
trap '[ "${ZT_FAILURES:-0}" -eq 0 ] && rm -rf "$TMP" \
      || echo "kept the simulator logs: $TMP" >&2' EXIT

# One boot against a FRESH library, so each case starts from the same three
# photos and a deletion in one cannot leak into the next.
# $1 = tag, $2 = comma-separated labels to tap (ZELTO_TAP_LABEL sequence), $3 = run seconds. Echoes the dir.
run_boot() {
    local tag="$1" labels="$2" secs="$3"
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/lib/photos" "$dir/lib/thumbs"
    printf 'sys.lock_enabled\t0\nsys.brightness\t5\n' > "$dir/data/settings.conf"

    local i=0 wp id
    for wp in "$REPO_ROOT"/resources/wallpaper/*.png; do
        [ -f "$wp" ] || continue
        [ "$i" -ge 3 ] && break
        id="$(printf '17849%08d-00' "$((10000000 + i))")"
        cp "$wp" "$dir/lib/photos/$id.png"
        cp "$wp" "$dir/lib/thumbs/$id.png"
        i=$((i + 1))
    done

    # ZELTO_TAP_APP scopes the sequence to Photos. Without it every surface in
    # the boot gets the same label hook — the launcher reported "NO SUCH CONTROL"
    # and, worse, a label this app shares with another surface would be pressed on
    # whichever one built first.
    #
    # 9s, not 6: the app maps around six seconds into a sim boot, and a tap that
    # lands the same 100ms presses the grid it has not finished replacing. The
    # probe dumps a second after the last tap, so the confirmation card it
    # describes is the one on screen.
    HEADLESS=1 SKIP_BUILD=1 SIM_SIZE=720x1440 \
    SHOT="$dir/frame.png" SHOT_DELAY="$secs" \
    ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
    ZELTO_PHOTOS_ROOT="$dir/lib" ZELTO_PHOTOS_VIEW=0 \
    ZELTO_TAP_LABEL="$labels" ZELTO_TAP_AT=9000 ZELTO_TAP_APP=os.zelto.photos \
    ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP=os.zelto.photos ZELTO_PROBE_AT=11500 \
    SIM_APP=zelto-photos \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir"
}

count_png() { find "$1" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l | tr -d ' '; }

# --- 1. The confirmation GATES the delete ----------------------------------
# Tap "Delete" and stop. The card is up; nothing has been unlinked yet.
DIR_G="$(run_boot gated "Delete" 14)"
if ! grep -q "probe text \[zelto-photos\] 'Delete photo?'" "$DIR_G/log"; then
    zt_fail "tapping Delete never raised the confirmation — the rest of this test cannot tell a working gate from a screen that was never reached" \
        "the confirmation card on screen" "absent (see $DIR_G/log)"
    zt_done
fi
zt_expect_eq "3" "$(count_png "$DIR_G/lib/photos")" \
    "a single Delete tap removed a photo before it was confirmed (see $DIR_G/lib/photos)"

# --- 2. Confirming deletes, and takes the thumbnail with it ----------------
DIR_D="$(run_boot deleted "Delete,Delete Photo" 16)"
if ! grep -q "zelto-photos: deleting" "$DIR_D/log"; then
    zt_fail "the confirmed delete never ran — the second tap did not reach the card's Delete button" \
        "'zelto-photos: deleting <id>'" "absent (see $DIR_D/log)"
    zt_done
fi
gone_id="$(sed -n 's/.*zelto-photos: deleting \([0-9-]*\).*/\1/p' "$DIR_D/log" | head -1)"
zt_expect_eq "2" "$(count_png "$DIR_D/lib/photos")" \
    "confirming did not remove the photo (see $DIR_D/lib/photos)"
if [ -n "$gone_id" ] && [ -f "$DIR_D/lib/photos/$gone_id.png" ]; then
    zt_fail "the deleted photo is still on disk" "$gone_id.png gone" "present"
fi
# The thumbnail is derived data: one outliving its photo shows a picture the
# library says does not exist.
if [ -n "$gone_id" ] && [ -f "$DIR_D/lib/thumbs/$gone_id.png" ]; then
    zt_fail "the thumbnail outlived the photo it belongs to" \
        "thumbs/$gone_id.png gone" "present (see $DIR_D/lib/thumbs)"
fi

# --- 3. Set as wallpaper writes the brokered key --------------------------
# The proof that this is a LIBRARY and not a directory Photos owns: a second,
# unrelated subsystem (P25's wallpaper) consumes a photo out of it.
DIR_W="$(run_boot wallpaper "Wallpaper" 14)"
if ! grep -q "zelto-photos: wallpaper set to" "$DIR_W/log"; then
    zt_fail "the Wallpaper button never ran — nothing below is evidence" \
        "'zelto-photos: wallpaper set to <path>'" "absent (see $DIR_W/log)"
    zt_done
fi
# Asserted on the PERSISTED store, not on the app's own claim: zsysd writes
# settings.conf, and that file is what the launcher and lock screen read after a
# reboot. A handler that logged and did not reach the broker fails here.
if ! grep -q "sys.wallpaper" "$DIR_W/data/settings.conf" 2>/dev/null; then
    zt_fail "sys.wallpaper never reached the settings store" \
        "a sys.wallpaper line in settings.conf" "absent (see $DIR_W/data/settings.conf)"
elif ! grep -q "sys.wallpaper.*$(basename "$DIR_W")/lib/photos" "$DIR_W/data/settings.conf"; then
    zt_fail "sys.wallpaper was written but does not point into the photo library" \
        "a path under lib/photos" "$(grep sys.wallpaper "$DIR_W/data/settings.conf")"
fi

zt_done
