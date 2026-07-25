#!/usr/bin/env bash
# test_screenshot_capture_sim — the system screenshot path, end to end, and the
# one thing it must refuse to do.
#
# WHAT IS UNDER TEST. P53's capture chord: the compositor forks zelto-shot, which
# copies the output through wlr-screencopy, writes a full-size PNG and a
# thumbnail into the shared photo library, and posts a notification. Every step
# of that is new, and two of them are the kind that fail silently —
#
#   * a PNG that decodes but is the wrong SIZE (a region, a stale mode, a
#     landscape default) is a perfectly valid file, so the frame's dimensions are
#     asserted against the output's, not merely its existence;
#   * a thumbnail that never got written leaves a library whose grid is blank,
#     and nothing in the writer's exit code says so.
#
# AND THE REFUSAL. A screenshot of a LOCKED phone writes the whole screen into a
# library anyone holding the handset can browse. The compositor already refuses
# to photograph a backgrounded WINDOW under a lock (P42/P43,
# test_capture_lock_suppression); this is the same guard on the more serious
# path, and it is guarded in the compositor rather than in zelto-shot because a
# client applying a rule to itself is a claim, not a control.
#
# WHY THE LOCKED RUN TAKES TWO SHOTS. "Nothing was written while locked" is also
# what a boot that never worked at all reports. So the locked run fires the chord
# TWICE — once before the lock engages and once after — and asserts one saved
# photo and one suppression from the SAME process. That is a positive control
# from the same run rather than an appeal to the run above it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="${BUILD:-$REPO_ROOT/build-host}"

if [ ! -x "$BUILD/compositor/zcomp" ]; then
    echo "SKIP: $BUILD/compositor/zcomp not built"
    exit 0
fi
if [ ! -x "$BUILD/system/shot/zelto-shot" ]; then
    echo "SKIP: $BUILD/system/shot/zelto-shot not built"
    exit 0
fi
if ! command -v grim >/dev/null 2>&1; then
    echo "SKIP: grim not installed; the simulator cannot complete a run"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not installed; cannot read a PNG header"
    exit 0
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/zelto-shot-test.XXXXXX")"
# Keep the logs AND the written PNGs when it fails — the evidence a failure
# points at must outlive the failure. (test_capture_lock_suppression learned this
# the hard way in P52: it failed once inside a full-suite run and had already
# deleted the only means of finding out why.) The notice goes to STDERR because
# run-tests.sh reproduces only the ZT_FAIL lines from stdout.
trap '[ "${ZT_FAILURES:-0}" -eq 0 ] && rm -rf "$TMP" \
      || echo "kept the simulator logs and captures: $TMP" >&2' EXIT

SIM_W=720
SIM_H=1440

# One simulator boot. $1 = tag, $2 = settings.conf body, $3 = ZCOMP_SHOT_AT list,
# $4 = how long the run lasts. Echoes the run directory.
run_boot() {
    local tag="$1" seed="$2" shots="$3" secs="$4"
    local dir="$TMP/$tag"
    mkdir -p "$dir/data" "$dir/xdg"
    printf '%b\n' "$seed" > "$dir/data/settings.conf"

    HEADLESS=1 SKIP_BUILD=1 \
    SHOT="$dir/frame.png" SHOT_DELAY="$secs" SIM_SIZE="${SIM_W}x${SIM_H}" \
    ZELTO_DATA_DIR="$dir/data" SIM_RUNTIME_DIR="$dir/xdg" \
    ZCOMP_SHOT_AT="$shots" \
    ZELTO_SHOT_BIN="$BUILD/system/shot/zelto-shot" \
    ZELTO_CONSENT_BIN=/bin/true \
        bash "$REPO_ROOT/meta/run-sim.sh" > "$dir/log" 2>&1
    echo "$dir"
}

# The library the boot wrote into. z_path_media puts it at <data>/media.
lib_dir()   { echo "$1/data/media/photos"; }
thumb_dir() { echo "$1/data/media/thumbs"; }

count_png() {
    local d="$1" n=0
    if [ -d "$d" ]; then
        n="$(find "$d" -maxdepth 1 -name '*.png' | wc -l | tr -d ' ')"
    fi
    echo "$n"
}

# Width/height straight out of the PNG IHDR — 8 bytes of signature, 4 of length,
# 4 of type, then w and h as big-endian u32. Read from the FILE rather than from
# the writer's own log, so a writer that logs one size and encodes another is
# caught rather than believed.
png_size() {
    python3 - "$1" <<'PY'
import struct, sys
with open(sys.argv[1], 'rb') as f:
    head = f.read(24)
if len(head) < 24 or head[:8] != b'\x89PNG\r\n\x1a\n':
    print("notapng")
else:
    w, h = struct.unpack('>II', head[16:24])
    print(f"{w}x{h}")
PY
}

# ---------------------------------------------------------------------------
# Run A — unlocked. The whole path, and the control for everything below.
# ---------------------------------------------------------------------------
DIR_A="$(run_boot unlocked 'sys.lock_enabled\t0\nsys.brightness\t5' '9000' 16)"
LOG_A="$DIR_A/log"

if ! grep -q "shot: capturing" "$LOG_A"; then
    zt_fail "the capture chord never reached the compositor — nothing below this can mean anything" \
        "'shot: capturing' present" "absent (see $LOG_A)"
    zt_done
fi
if ! grep -q "zelto-shot: saved" "$LOG_A"; then
    zt_fail "the compositor forked the screenshot service but nothing was saved" \
        "'zelto-shot: saved <id> WxH' present" "absent (see $LOG_A)"
    zt_done
fi

LIB_A="$(lib_dir "$DIR_A")"
n_a="$(count_png "$LIB_A")"
zt_expect_eq "1" "$n_a" "one chord must write exactly one photo (see $LIB_A)"

photo="$(find "$LIB_A" -maxdepth 1 -name '*.png' | head -1)"
if [ -z "$photo" ]; then
    zt_fail "no photo file in the library" "one .png" "none (see $LIB_A)"
    zt_done
fi

# THE SIZE, from the file. A capture of the wrong region — or of QEMU's old
# 1280x800 default, which every on-target frame silently used from P6 to P45 —
# produces a completely valid PNG.
got_size="$(png_size "$photo")"
zt_expect_eq "${SIM_W}x${SIM_H}" "$got_size" \
    "the screenshot is not the size of the screen (see $photo)"

# The id is the FILENAME, which is the library's whole index. Fixed width is what
# makes a reverse sort chronological; a photo whose name is not that is a photo
# the grid will order wrongly.
stem="$(basename "$photo" .png)"
case "$stem" in
    [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9]) ;;
    *) zt_fail "the photo's id is not the fixed-width <epoch_ms>-<seq> the index depends on" \
           "13 digits, a dash, 2 digits" "$stem" ;;
esac

# The thumbnail is a SEPARATE FILE by design (see system/common/photos.h), and a
# missing one is invisible until a grid is blank.
THUMBS_A="$(thumb_dir "$DIR_A")"
if [ ! -f "$THUMBS_A/$stem.png" ]; then
    zt_fail "no thumbnail was written beside the photo" \
        "$THUMBS_A/$stem.png" "absent"
else
    tsize="$(png_size "$THUMBS_A/$stem.png")"
    tw="${tsize%x*}"
    # It must be SMALLER than the original — a "thumbnail" the size of the frame
    # is the sharing bug photos.h exists to avoid, and it would look fine.
    if [ "${tw:-99999}" -ge "$SIM_W" ]; then
        zt_fail "the thumbnail was not downscaled — the grid would decode full frames" \
            "< ${SIM_W} wide" "$tsize"
    fi
fi

# The notification. It is posted under the Photos app's identity, because
# zelto-shot has no window and no app loop of its own.
if ! grep -q "notify_post app=os.zelto.photos" "$LOG_A"; then
    zt_fail "the screenshot was saved but nothing told the user" \
        "'[zsysd] notify_post app=os.zelto.photos' present" "absent (see $LOG_A)"
fi

# ---------------------------------------------------------------------------
# Run B — locked. Two chords in one boot: one before the lock, one after.
# ---------------------------------------------------------------------------
DIR_B="$(run_boot locked \
    'sys.lock_enabled\t1\nsys.idle_dim_s\t1\nsys.idle_lock_s\t6\nsys.idle_off_s\t9999\nsys.brightness\t5' \
    '3000 14000' 20)"
LOG_B="$DIR_B/log"

# The in-run positive control: the FIRST chord, fired at 3s while the screen was
# still unlocked, must have produced a photo. Without this the assertion below is
# satisfied by any boot that simply never worked.
if ! grep -q "zelto-shot: saved" "$LOG_B"; then
    zt_fail "the locked run captured nothing at all, including before the lock — it cannot distinguish 'suppressed' from 'never worked'" \
        "'zelto-shot: saved' present (the pre-lock control)" "absent (see $LOG_B)"
    zt_done
fi
# And the lock must actually have engaged, or run B is run A with extra steps.
if ! grep -q "shot: suppressed (screen held by a modal layer)" "$LOG_B"; then
    zt_fail "the locked run never suppressed a capture — either the screen did not lock within the run, or the guard is not on this path" \
        "'shot: suppressed (screen held by a modal layer)'" "absent (see $LOG_B)"
    zt_done
fi

# THE ASSERTION. Two chords, one photo: the second wrote nothing.
LIB_B="$(lib_dir "$DIR_B")"
n_b="$(count_png "$LIB_B")"
zt_expect_eq "1" "$n_b" \
    "two capture chords fired and the screen was locked for the second — a second file means the lock guard did not hold (see $LIB_B)"

saved_b="$(grep -c "zelto-shot: saved" "$LOG_B" 2>/dev/null || true)"
zt_expect_eq "1" "${saved_b:-0}" \
    "the screenshot service ran while the screen was locked (see $LOG_B)"

zt_done
