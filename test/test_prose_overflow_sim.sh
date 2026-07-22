#!/usr/bin/env bash
# test_prose_overflow_sim — the surfaces that display text they did not write.
#
# THE CONCERN. A notification's title and body, a permission prompt's app name, a
# Store listing's description: none of these are written by the screen that shows
# them, and a Text node measures to ONE line however long that line is. The
# toolkit does not clip it, does not warn, and does not wrap — it paints straight
# through the edge of the card, off the panel, off the screen. That is what the
# share sheet did for two phases (test_share_sheet_sim), and P45 migrated these
# three surfaces to WrapText to prevent the same thing here.
#
# WHY THEY STILL NEEDED A TEST. None of them had ever been SHOWN a long string.
# WrapText was added, the frames were checked by eye against the strings that
# happened to be there — "Ping", "You have a new ping", an app id short enough to
# fit — and a wrap that never has to wrap is indistinguishable from no wrap at
# all. This boots each surface with prose long enough to need it.
#
# HOW OVERFLOW IS DETECTED. Not by eye and not by a delta. ZELTO_PROBE_TAPS
# (sdk/src/app.c) reports, for every Text node on the settled frame, whether it
# needs more room than the box it was given. The frames alone cannot show this:
# arrange() clamps a Text's frame to its parent's inner box, so an unbounded
# string ends up with a perfectly reasonable width and paints through it. measure()
# records the shaped width in text_w, and the probe compares the two.
#
# NEGATIVE-TESTED via test_share_sheet_sim, which shares the detector: reverting
# the share sheet's payload to a bare Text makes the same counter report 1.
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

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-prose.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# One boot, one surface. $1 = tag, $2 = the app_id/title to scope the probe to,
# the rest = extra environment.
run_boot() {
    local tag="$1" who="$2"; shift 2
    local dir="$TMP/$tag"
    mkdir -p "$dir" "$dir/data" "$dir/xdg"
    env HEADLESS=1 SKIP_BUILD=1 \
        SHOT="$dir/frame.png" SHOT_DELAY=9 \
        ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
        ZELTO_PROBE_TAPS=1 ZELTO_PROBE_APP="$who" "$@" \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir/log"
}

# Assert one surface: it drew text at all, and none of it wants more room than it
# was given. The scanned count is the positive control — "0 overflowing" out of 0
# text nodes is an empty screen, not a healthy one, and the two numbers are on the
# same log line so a test cannot read one without the other.
check() {   # <name> <log> <min_texts>
    local name="$1" log="$2" min="$3"
    local scanned over
    scanned="$(sed -n 's/.*text \([0-9]*\) scanned .*/\1/p' "$log" | head -1)"
    over="$(sed -n 's/.*text [0-9]* scanned \([0-9]*\) off-surface.*/\1/p' "$log" | head -1)"
    if [ -z "$scanned" ]; then
        zt_fail "$name never reported a probe summary — the surface did not come up, so nothing below is evidence" \
            "'probe taps: ... text N scanned M off-surface'" "absent (see $log)"
        return
    fi
    if [ "$scanned" -lt "$min" ]; then
        zt_fail "$name drew only $scanned text nodes, fewer than the $min it must have — it is not showing the content this test seeded" \
            ">= $min text nodes" "$scanned (see $log)"
        return
    fi
    zt_expect_eq "0" "$over" \
        "$name has text that needs more room than its box: $(grep -m1 'probe OVERFLOW' "$log" || echo '(see the log)') (see $log)"
}

# --- 1. The Notification Center ---------------------------------------------
# Three seeded notifications; the third carries a body sized to fill the sink's
# 192-byte field, which is six lines in this column. It is also the fix for the
# `11a-notification-center` catalogue shot, which photographed an EMPTY panel.
LOG_NC="$(run_boot nc shade_body ZELTO_SHADE_OPEN=nc ZELTO_BANNER_DEMO=3)"
check "the Notification Center" "$LOG_NC" 12
# The cards are really there — this is the 11a claim, and it is the one a delta
# can never make: an empty panel diffs against the wallpaper behind it just fine.
for want in Ping "Grocery list" "Update available"; do
    if ! grep -q "probe text '$want'" "$LOG_NC"; then
        zt_fail "the Notification Center is missing the '$want' card it was seeded with" \
            "a notification card titled '$want'" "absent (see $LOG_NC)"
    fi
done

# --- 2. The permission alert -------------------------------------------------
# The app id is the string the alert cannot control: zsysd passes whatever the
# manifest says, and an app with no display name is shown by its reverse-DNS id.
LONG_ID="an.extremely.long.reverse.dns.identifier.that.nobody.would.ever.type"
LOG_C="$(run_boot consent Permission SIM_CONSENT="$LONG_ID camera")"
check "the permission alert" "$LOG_C" 4

# --- 3. The Store ------------------------------------------------------------
LOG_S="$(run_boot store os.zelto.store SIM_APP=zelto-store)"
check "the Store" "$LOG_S" 4

zt_done
