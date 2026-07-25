#!/usr/bin/env bash
# Zelto OS desktop simulator — run zcomp + the full System UI nested on your
# desktop session (WSLg, or any Wayland/X11 session), with NO VM and NO aarch64
# emulation. The host x86_64 build (build-host/) runs at native speed; zcomp
# opens as one phone-shaped window (or headless) and every Zelto surface — bar,
# launcher, keyboard, apps — connects to *it*, so zcomp itself provides
# layer-shell / input-method / foreign-toplevel to its clients. This is the fast
# inner-loop counterpart to meta/run-qemu.sh (which boots the real aarch64 image).
#
# Usage:
#   meta/run-sim.sh                    # build + run in a window on the desktop
#   SKIP_BUILD=1 meta/run-sim.sh       # run existing build-host/ artifacts (no build)
#   SIM_APP=zelto-notepad meta/run-sim.sh          # also auto-launch an app
#   HEADLESS=1 SHOT=out/sim.png meta/run-sim.sh    # boot headless, grab a PNG, exit
#
# Headless capture + input use the wlr-screencopy / virtual-pointer /
# virtual-keyboard globals zcomp advertises (server.c/seat.c): `grim` takes the
# screenshot, `wlrctl`/`wtype` inject taps + keystrokes — the desktop analog of
# run-qemu.sh's QMP screendump + input-send-event. See docs/tooling/simulator.md.
#
# Gestures that must PRESS AND HOLD (a drag, a long-press) cannot be driven by
# wlrctl, which only clicks. zcomp scripts those itself, through the same seat
# path a real device takes (compositor/src/seat.c):
#
#   ZCOMP_DRAG="x0 y0 x1 y1 [ms]"  press, glide, release  (pan / swipe)
#   ZCOMP_HOLD="x y [ms]"          press, hold, release   (long-press; a short
#                                  hold is also just a TAP at a chosen moment)
#   ZCOMP_INPUT_DELAY=ms           when to start (default 6500 — after the app maps)
#
# Env: BUILD (build-host), WLR_RENDERER (gles2|pixman fallback), SHOT_DELAY (7),
#      ZELTO_DATA_DIR (/tmp/zelto-sim/data), SIM_APP (extra app to launch),
#      SIM_SCRIPT (.js app to launch) + SIM_SCRIPT_ID, SIM_NET=1 (local HTTP
#      endpoint for the networking demo).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build-host}"
SYS="$BUILD/system"
SHOT_DELAY="${SHOT_DELAY:-7}"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> building host (x86_64)"
    if [ ! -d "$BUILD" ]; then
        meson setup "$BUILD" "$REPO_ROOT"
    fi
    ninja -C "$BUILD"
fi

# Software renderer by default: the GLES2/EGL path needs a real GPU device, which
# the virtual backends don't have; pixman runs on the CPU (native x86 — plenty fast
# for this UI). Override WLR_RENDERER=gles2 for the windowed path on a GPU.
export WLR_RENDERER="${WLR_RENDERER:-pixman}"

# Phone-shaped output. The virtual output takes any custom size (zcomp reads
# ZCOMP_OUTPUT_SIZE), so simulate a portrait handset by default. Override with
# SIM_SIZE=WxH (e.g. SIM_SIZE=1080x2340, or a landscape 1280x720).
export ZCOMP_OUTPUT_SIZE="${SIM_SIZE:-720x1440}"

# zcomp + every Zelto client live in a PRIVATE runtime dir, so zcomp's own socket
# is deterministically wayland-0 there and can never collide with (or clobber) the
# desktop's wayland-0. The windowed path reaches the parent compositor by ABSOLUTE
# socket path instead, which is orthogonal to that private dir.
orig_xdg="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR="${SIM_RUNTIME_DIR:-/tmp/zelto-sim/xdg}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"

# Reap a previous run's compositor BEFORE touching its socket, and WAIT for it to
# actually be gone.
#
# The order matters and used to be wrong. libwayland unlinks the socket path (and
# its .lock) when the display is destroyed — so a zcomp that is still shutting
# down will delete whatever file now sits at that path. If we clear the stale
# sockets first and start the new compositor while the old one is still on its way
# out, the old one's exit unlinks the NEW compositor's socket: every client
# launched after that point dies with "cannot connect to Wayland display", and
# grim burns all six of its retries against a socket that is never coming back.
# The run still "succeeds" — it just produces a missing or half-populated frame.
#
# So: signal, poll until the process is really gone, escalate to KILL, and only
# then remove socket files.
reap_stale_zcomp() {
    pgrep -f "$BUILD/compositor/zcomp" >/dev/null 2>&1 || return 0
    echo "==> reaping a stale zcomp from a previous run"
    pkill -f "$BUILD/compositor/zcomp" 2>/dev/null || true
    for _ in $(seq 1 40); do            # up to 10s of graceful exit
        pgrep -f "$BUILD/compositor/zcomp" >/dev/null 2>&1 || return 0
        sleep 0.25
    done
    pkill -9 -f "$BUILD/compositor/zcomp" 2>/dev/null || true
    for _ in $(seq 1 20); do
        pgrep -f "$BUILD/compositor/zcomp" >/dev/null 2>&1 || return 0
        sleep 0.25
    done
    echo "!! a previous zcomp will not die; this run may collide with it"
}
reap_stale_zcomp
rm -f "$XDG_RUNTIME_DIR"/wayland-*       # clear a prior run's stale sockets

if [ "${HEADLESS:-0}" = "1" ]; then
    export WLR_HEADLESS_OUTPUTS=1
    unset WAYLAND_DISPLAY DISPLAY        # force the headless backend (don't nest)
    BACKENDS="headless"
else
    # Resolve the desktop's Wayland socket to an absolute path (WSLg's real one
    # lives under /mnt/wslg if the session symlink is missing/stale).
    pdisp="${WAYLAND_DISPLAY:-wayland-0}"
    case "$pdisp" in /*) PARENT_SOCK="$pdisp";; *) PARENT_SOCK="$orig_xdg/$pdisp";; esac
    [ -S "$PARENT_SOCK" ] || PARENT_SOCK=/mnt/wslg/runtime-dir/wayland-0
    export WAYLAND_DISPLAY="$PARENT_SOCK"
    # Nest in the desktop's Wayland session. WSLg's X11 (Xwayland) lacks DRI3/shm
    # for wlroots' x11 backend, so Wayland is the path; SIM_BACKEND=x11 to force it.
    BACKENDS="${SIM_BACKEND:-wayland}"
fi

export ZELTO_FONT="${ZELTO_FONT:-$REPO_ROOT/sdk/assets/fonts/Satoshi-Variable.ttf}"
# No /dev/vda here — apps' persistent storage lives under a host temp dir instead
# of the phone's /var/zelto (libzelto reads $ZELTO_DATA_DIR, storage.c).
export ZELTO_DATA_DIR="${ZELTO_DATA_DIR:-/tmp/zelto-sim/data}"
mkdir -p "$ZELTO_DATA_DIR/apps"

# App icons (P24). On the device build-initramfs.sh installs each app's icon to a
# system path and the manifests carry that absolute icon=; here the apps run from
# the repo, so point the shared Placeholder fallback + the rewritten per-app
# icon= at the in-repo resources/ tree instead (the ZELTO_RECENTS_BIN idiom).
export ZELTO_PLACEHOLDER_ICON="${ZELTO_PLACEHOLDER_ICON:-$REPO_ROOT/resources/app-icons/Placeholder.png}"

# Wallpapers (P25). On the device build-initramfs.sh installs them to a system dir
# and the launcher seeds sys.wallpaper to the first one; here the apps run from the
# repo, so point the wallpaper dir at the in-repo resources/ tree (same idiom).
# The launcher/lock/settings read $ZELTO_WALLPAPER_DIR via system/common/wallpaper.h.
export ZELTO_WALLPAPER_DIR="${ZELTO_WALLPAPER_DIR:-$REPO_ROOT/resources/wallpaper}"

# P23 power source: the host has no phone battery, so drive a fake drain by
# default (the status-bar battery glyph then shows a real level and slowly
# discharges). Off with ZELTO_FAKE_BATTERY=0; tune the cadence with
# ZELTO_BATTERY_TICK_MS (default 5000). ZELTO_VOLUME_MS widens the HUD dwell for
# a reliable screenshot.
export ZELTO_FAKE_BATTERY="${ZELTO_FAKE_BATTERY:-1}"

# P53 screenshots: the compositor forks the capture service by absolute path, on
# the device /usr/bin/zelto-shot. Uninstalled here, so point it at the build-host
# binary (the ZELTO_RECENTS_BIN / ZELTO_CONSENT_BIN idiom). Drive a capture with
# ZCOMP_SHOT_AT="ms [ms ...]"; the photos land under $ZELTO_DATA_DIR/media.
export ZELTO_SHOT_BIN="${ZELTO_SHOT_BIN:-$SYS/shot/zelto-shot}"

# P38 sensor/location source: the host has no phone sensors, so zsysd synthesises
# them from these ZELTO_SIM_* values (a real device port fills them from a HAL).
# Override any of them to script a reading — e.g. ZELTO_SIM_LOCATION="48.85,2.35"
# to move the GPS fix, matching `zelto simulator set location` in the docs.
export ZELTO_SIM_LOCATION="${ZELTO_SIM_LOCATION:-52.5200,13.4050}"
export ZELTO_SIM_ORIENTATION="${ZELTO_SIM_ORIENTATION:-30,2,1}"
export ZELTO_SIM_ACCEL="${ZELTO_SIM_ACCEL:-0.6,0.2,9.78}"
export ZELTO_SIM_GYRO="${ZELTO_SIM_GYRO:-0.02,0.00,0.03}"
export ZELTO_SIM_MAG="${ZELTO_SIM_MAG:-0,-30,-40}"
export ZELTO_SIM_LIGHT="${ZELTO_SIM_LIGHT:-320}"
rm -f "$XDG_RUNTIME_DIR/zsysd.sock"     # drop a stale broker socket from a prior run

# ZELTO_PROBE_AT is per-process, which is wrong for a surface that starts LATE
# (an app launched by a scripted tap ten seconds in). Convert it once, here, into
# one absolute moment every client shares — a process that starts after it dumps
# as soon as it has a tree. See the note in sdk/src/app.c.
if [ -n "${ZELTO_TAP_AT:-}" ] && [ -z "${ZELTO_TAP_EPOCH:-}" ]; then
    export ZELTO_TAP_EPOCH="$(awk -v ms="$ZELTO_TAP_AT"         'BEGIN { printf "%.3f", systime() + ms / 1000.0 }')"
fi
if [ -n "${ZELTO_PROBE_AT:-}" ] && [ -z "${ZELTO_PROBE_EPOCH:-}" ]; then
    export ZELTO_PROBE_EPOCH="$(awk -v ms="$ZELTO_PROBE_AT"         'BEGIN { srand(); printf "%.3f", systime() + ms / 1000.0 }')"
fi

PIDS=()
cleanup() { kill "${ZPID:-}" "${PIDS[@]}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# Note existing sockets so we can spot the one zcomp creates for its clients.
list_sockets() { ls "$XDG_RUNTIME_DIR"/wayland-[0-9]* 2>/dev/null | grep -v '\.lock$' || true; }

# Launch zcomp on one backend and wait for the wayland-N socket its clients use
# (whatever appeared since `before`; add_socket_auto picks the first free name).
# Sets SIM_WL + ZPID on success; returns 1 if that backend can't come up (e.g. a
# Wayland/X11 parent that isn't reachable), so the caller can try the next one.
SIM_WL=""; ZPID=""
try_backend() {
    local backend="$1" before s b seen
    before="$(list_sockets)"
    echo "==> starting zcomp (backend=$backend renderer=$WLR_RENDERER)"
    WLR_BACKENDS="$backend" "$BUILD/compositor/zcomp" & ZPID=$!
    for _ in $(seq 1 60); do
        for s in $(list_sockets); do
            seen=0; for b in $before; do [ "$b" = "$s" ] && seen=1; done
            if [ "$seen" = 0 ] && [ -S "$s" ]; then SIM_WL="$(basename "$s")"; return 0; fi
        done
        kill -0 "$ZPID" 2>/dev/null || return 1
        sleep 0.25
    done
    return 1
}

for backend in $BACKENDS; do
    try_backend "$backend" && break
    echo "!! zcomp backend '$backend' did not come up; trying next"
    kill "$ZPID" 2>/dev/null || true; wait "$ZPID" 2>/dev/null || true
    SIM_WL=""
done
if [ -z "$SIM_WL" ]; then
    echo "!! no usable backend ($BACKENDS)."
    echo "   In WSL you need a WSLg GUI session — check:  echo \$WAYLAND_DISPLAY \$DISPLAY"
    echo "   (both should be set, e.g. wayland-0 and :0). Or run windowless: HEADLESS=1 SHOT=out.png meta/run-sim.sh"
    exit 1
fi
echo "==> zcomp socket: $SIM_WL  (clients connect here)"
export WAYLAND_DISPLAY="$SIM_WL"

# App tiles for the launcher. On the real device build-initramfs.sh copies the
# .app manifests into /usr/share/zelto/apps (the launcher's hardcoded MANIFEST_DIR)
# and every app binary to /usr/bin. Here the launcher runs natively on the host,
# where that dir is empty — so we instead emit manifests into the OTHER dir the
# launcher scans, $ZELTO_DATA_DIR/apps/manifests (launcher main.c ensure_apps()),
# with each exec= rewritten from the image path /usr/bin/zelto-X to the actual
# build-host binary. Same app set as build-initramfs.sh (Widget is deliberately
# omitted there too — it ships only inside widget.zap for the P13 install demo).
MANIFEST_OUT="$ZELTO_DATA_DIR/apps/manifests"
mkdir -p "$MANIFEST_OUT"
# Clear only the manifests this script OWNS (each is rewritten below). A manifest
# put here by zelto-install belongs to a runtime-installed package and must
# survive a reboot — wiping the whole directory would uninstall it, which is
# precisely the thing the install harness needs to prove does not happen.
for stale in "$MANIFEST_OUT"/*.app; do
    [ -f "$stale" ] || continue
    grep -q '^# Installed at runtime by zelto-install' "$stale" || rm -f "$stale"
done
for m in "$REPO_ROOT/samples/hello/zelto-hello.app" \
         "$REPO_ROOT/system/apps/cards/zelto-cards.app" \
         "$REPO_ROOT/system/share/zelto-share.app" \
         "$REPO_ROOT/system/apps/notes/zelto-notes.app" \
         "$REPO_ROOT/system/apps/pinger/zelto-pinger.app" \
         "$REPO_ROOT/system/apps/notepad/zelto-notepad.app" \
         "$REPO_ROOT/system/apps/settings/zelto-settings.app" \
         "$REPO_ROOT/system/apps/fetch/zelto-fetch.app" \
         "$REPO_ROOT/system/apps/store/zelto-store.app" \
         "$REPO_ROOT/system/apps/jsdemo/zelto-jsdemo.app" \
         "$REPO_ROOT/system/apps/sensors/zelto-sensors.app" \
         "$REPO_ROOT/system/apps/photos/zelto-photos.app" \
         "$REPO_ROOT/samples/andemu-demo/zelto-andemu.app"; do
    [ -f "$m" ] || { echo "!! manifest missing: $m"; continue; }
    # exec= is a COMMAND (system/common/exec_cmd.h): the binary, then optional
    # args — a script app is "/usr/bin/zelto-script --id X /usr/share/zelto/
    # scripts/X.js". Rewrite the binary to its build-host path, and any .js
    # argument to the copy that lives next to the manifest in the repo.
    exec_img="$(sed -n 's/^exec=//p' "$m" | head -1)"
    bin_name="$(basename "${exec_img%% *}")"
    exec_args="${exec_img#* }"
    [ "$exec_args" = "$exec_img" ] && exec_args=""     # no args
    bin_host="$(find "$BUILD/system" "$BUILD/samples" "$BUILD/script" -type f -name "$bin_name" -perm -u+x 2>/dev/null | head -1)"
    if [ -z "$bin_host" ]; then
        echo "!! host binary missing for $(basename "$m") ($bin_name); skipping tile"
        continue
    fi
    if [ -n "$exec_args" ]; then
        # /usr/share/zelto/scripts/foo.js -> <dir of this manifest>/foo.js
        exec_args="$(echo "$exec_args" | sed "s#[^ ]*/\([^/ ]*\.js\)#$(dirname "$m")/\1#g")"
        exec_host="$bin_host $exec_args"
    else
        exec_host="$bin_host"
    fi
    # Copy the manifest verbatim but repoint exec= at the host command, and
    # rewrite any icon= from its device path to the matching in-repo asset:
    # <...>/<name>.svg -> resources/icons/<name>.svg, and a .png -> the authored
    # resources/app-icons/<name>.png. Apps with no icon= fall back to the
    # Placeholder resolved via $ZELTO_PLACEHOLDER_ICON above.
    sed -e "s#^exec=.*#exec=$exec_host#" \
        -e "s#^icon=.*/\([^/]*\.svg\)\$#icon=$REPO_ROOT/resources/icons/\1#" \
        -e "s#^icon=.*/\([^/]*\.png\)\$#icon=$REPO_ROOT/resources/app-icons/\1#" \
        "$m" > "$MANIFEST_OUT/$(basename "$m")"
    echo "    tile: $(basename "$m" .app) -> $exec_host"
done

# Launch the shell in the same order /init does (minus the kernel/udev/net/binder
# bring-up, which the desktop already provides). Each piece is a client of zcomp.
spawn() { [ -x "$1" ] && { "$@" & PIDS+=($!); }; }
# zsysd forks the consent dialog on a permission prompt by absolute path; on the
# device that's /usr/bin/zelto-consent, uninstalled here — point it at the build
# binary (the ZELTO_RECENTS_BIN idiom) so prompts resolve instead of auto-denying.
# Honour a preset ZELTO_CONSENT_BIN (a harness can point it at /bin/true to
# auto-allow every prompt for an unattended screenshot); default to the real one.
export ZELTO_CONSENT_BIN="${ZELTO_CONSENT_BIN:-$SYS/consent/zelto-consent}"
spawn "$SYS/zsysd/zsysd"

# Wait for the broker to be LISTENING before starting anything that reads a
# setting from it.
#
# Every System-UI client opens with a z_setting_get, and if the broker's socket is
# not bound yet that call falls back to a compiled-in default — silently, because
# a default looks exactly like a configured value. That is how a shot booted with
# zelto-lock at lock_enabled=0 and photographed an unlocked home screen for a lock
# test. libzelto now waits for the broker itself (sdk/src/app.c zsysd_connect), so
# this is belt-and-braces; it is here because the harness should be the place the
# race is VISIBLE rather than absorbed, and because it keeps the boot log ordered.
wait_for_zsysd() {
    for _ in $(seq 1 100); do            # up to 10s
        [ -S "$XDG_RUNTIME_DIR/zsysd.sock" ] && return 0
        sleep 0.1
    done
    echo "!! zsysd never bound $XDG_RUNTIME_DIR/zsysd.sock — clients will run on"
    echo "   compiled-in defaults and this boot is NOT representative"
    return 1
}
wait_for_zsysd || true

spawn "$SYS/bar/zelto-bar"
# The home indicator's switcher gesture fork/execs the recents overlay by
# absolute path, which on the device is /usr/bin/zelto-recents. Uninstalled here,
# so point it at the build-host binary (homebar reads ZELTO_RECENTS_BIN).
export ZELTO_RECENTS_BIN="$SYS/recents/zelto-recents"
spawn "$SYS/homebar/zelto-homebar"
spawn "$SYS/keyboard/zelto-keyboard"
spawn "$SYS/shade/zelto-shade"
spawn "$SYS/dim/zelto-dim"
spawn "$SYS/lock/zelto-lock"
spawn "$SYS/volume/zelto-volume"
sleep 1
spawn "$SYS/launcher/zelto-launcher"

# Resolve a binary by name (or accept an absolute path) under the host build.
resolve_bin() {
    if [ -x "$1" ]; then echo "$1"; return; fi
    find "$BUILD/system" "$BUILD/samples" "$BUILD/script" -type f -name "$1" -perm -u+x 2>/dev/null | head -1
}

# Optional (SIM_NET=1): a local HTTP endpoint for the networking demo, the sim's
# answer to the QEMU harness's host server on 10.0.2.2 (run-qemu.sh's NET=1).
#
# In the VM the guest reaches the host through slirp's 10.0.2.2 alias; the sim has
# no VM and no slirp — an app here runs natively, so the server is simply on
# loopback. The app must therefore be TOLD where to look, which it reads from its
# own prefs (jsdemo.endpoint): the .prefs file is a TAB-separated key/value store
# in the app's data dir (sdk/src/storage.c), so seeding it is a one-line write and
# needs no special-casing inside the app.
if [ "${SIM_NET:-0}" = "1" ]; then
    NET_PORT="${NET_PORT:-8080}"
    SERVE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zelto-sim-net.XXXXXX")"
    printf 'hello from the host\n' > "$SERVE_DIR/hello"
    ( cd "$SERVE_DIR" && exec python3 -m http.server "$NET_PORT" --bind 127.0.0.1 ) \
        >/dev/null 2>&1 &
    NET_SRV_PID=$!
    trap 'kill "$NET_SRV_PID" 2>/dev/null || true; rm -rf "$SERVE_DIR"' EXIT
    echo "==> [net] host HTTP server on 127.0.0.1:$NET_PORT ($SERVE_DIR)"

    JSDEMO_PREFS="$ZELTO_DATA_DIR/apps/os.zelto.jsdemo/documents"
    mkdir -p "$JSDEMO_PREFS"
    printf 'jsdemo.endpoint\thttp://127.0.0.1:%s/hello\n' "$NET_PORT" \
        > "$JSDEMO_PREFS/.prefs"
fi

# Optional: auto-launch a Zelto Script app by .js path (SIM_SCRIPT=path/to/app.js),
# the script analog of SIM_APP — the runtime is one binary shared by every script
# app, so the app to launch is an argument, not a binary name. SIM_SCRIPT_ID
# overrides the app id (defaults to the JS Demo's, which most script shots use).
if [ -n "${SIM_SCRIPT:-}" ]; then
    sleep 2
    spawn "$(resolve_bin zelto-script)" \
        --id "${SIM_SCRIPT_ID:-os.zelto.jsdemo}" "$SIM_SCRIPT"
fi

# Optional: auto-launch an app (name like "zelto-notepad", or a full path), handy
# for a headless screenshot of a specific app without scripting a tile tap.
if [ -n "${SIM_APP:-}" ]; then
    sleep 2
    spawn "$(resolve_bin "$SIM_APP")"
fi

# Optional: launch an app N seconds INTO the run, rather than during boot
# (SIM_LATE_APP="zelto-cards 6"). SIM_APP and SIM_EXTRA both spawn while the shell
# is still coming up, which is fine for "have this on screen" but useless for
# "make something happen once the system has settled into a state" — the window
# maps before the state exists. A late launch is a focus change at a chosen
# moment, which is how a test reaches an edge that only fires on one (the App
# Switcher's capture is taken at the active->inactive edge; the lock-suppression
# path needs that edge to land AFTER the screen has locked).
if [ -n "${SIM_LATE_APP:-}" ]; then
    set -- $SIM_LATE_APP
    late_bin="$(resolve_bin "$1")"
    late_delay="${2:-5}"
    if [ -n "$late_bin" ]; then
        echo "==> [late] will launch $(basename "$late_bin") after ${late_delay}s"
        ( sleep "$late_delay"; exec "$late_bin" ) & PIDS+=($!)
    else
        echo "!! SIM_LATE_APP: no such binary: $1"
    fi
fi

# Optional (shots harness): extra apps to leave running — a space-separated list of
# binary names — so a populated Recents / task switcher can be captured. Each is an
# ordinary xdg toplevel; they stack behind whatever overlay is spawned below.
if [ -n "${SIM_EXTRA:-}" ]; then
    sleep 1
    for name in $SIM_EXTRA; do spawn "$(resolve_bin "$name")"; done
fi

# Optional (shots harness): spawn the Recents overlay last so it composites over the
# running apps (SIM_EXTRA populates the list it shows).
if [ "${SIM_RECENTS:-0}" = "1" ]; then
    sleep 1
    spawn "$SYS/recents/zelto-recents"
fi

# Optional (shots harness): spawn the permission-consent overlay standalone with
# "<app_id> <perm>" — the modal card the broker normally forks, captured directly.
if [ -n "${SIM_CONSENT:-}" ]; then
    sleep 1
    # shellcheck disable=SC2086
    spawn "$SYS/consent/zelto-consent" $SIM_CONSENT
fi

# Optional (shots harness): spawn the share sheet standalone with a space-separated
# list of candidate app_ids — the sheet zsysd normally forks to resolve an intent,
# captured directly. Its exit code is the pick, which nothing reads here. Set
# ZELTO_SHARE_MIME / ZELTO_SHARE_PAYLOAD too (zsysd passes them in the child's
# environment) to get the preview row the real sheet shows.
if [ -n "${SIM_CHOOSER:-}" ]; then
    sleep 1
    # shellcheck disable=SC2086
    spawn "$SYS/chooser/zelto-chooser" $SIM_CHOOSER
fi

# Headless + SHOT: give it a moment to render, grab a PNG with grim, then exit —
# the agent/CI verification path. Otherwise block on zcomp (interactive window).
if [ -n "${SHOT:-}" ]; then
    sleep "$SHOT_DELAY"

    # Optional: tap a point before capturing (SIM_TAP="x y"), for a shot that has
    # to show the RESULT of an interaction — a script app's counter after +1, say.
    # zcomp's virtual pointer takes RELATIVE motion only, so the cursor is parked
    # at the origin first (a large negative move saturates at 0,0) and then moved
    # by exactly x,y.
    if [ -n "${SIM_TAP:-}" ] && command -v wlrctl >/dev/null 2>&1; then
        set -- $SIM_TAP
        wlrctl pointer move -9999 -9999 2>/dev/null || true
        wlrctl pointer move "$1" "$2" 2>/dev/null || true
        wlrctl pointer click left 2>/dev/null || true
        sleep 1
    fi

    mkdir -p "$(dirname "$SHOT")"
    if command -v grim >/dev/null 2>&1; then
        # grim can race an unsettled headless zcomp on a cold boot ("failed to
        # create display" = the screencopy/output isn't up yet). Retry a few times
        # a second apart instead of re-booting the whole sim, so a transient race
        # doesn't drop the shot. (Each run_shot in shots.sh is a fresh cold boot,
        # so every capture is a "first shot" and hits this more often.)
        for attempt in 1 2 3 4 5 6; do
            if grim "$SHOT" 2>/dev/null && [ -s "$SHOT" ]; then
                echo "==> wrote $SHOT (grim attempt $attempt)"
                break
            fi
            echo "   grim attempt $attempt: display not ready, retrying"
            sleep 1.5
        done
        # A frame is not proof of a boot. If the shell never came up, grim happily
        # captures zcomp's flat teal clear colour and writes a perfectly valid PNG
        # — which then sits in the catalogue looking like a deliberate design.
        # A real Zelto frame has a wallpaper, a status bar and text in it, so it
        # compresses poorly; a flat fill compresses to almost nothing. Judge on
        # that, and say so loudly rather than exiting 0 on a blank screen.
        if [ ! -s "$SHOT" ]; then
            echo "!! grim never produced a frame after 6 attempts"
            exit 1
        fi
        # ALLOW_FLAT=1 for the one frame whose SUBJECT is a flat fill: the
        # screen-off scrim. P46's catalogue audit found 17-lock-off had been
        # rejected by this guard and reported MISSING every run — the guard is
        # right about every other shot and wrong about the one that is meant to
        # be black, so the exception is declared rather than the guard weakened.
        if [ "${ALLOW_FLAT:-0}" != "1" ]; then
            bytes=$(stat -c%s "$SHOT" 2>/dev/null || echo 0)
            if [ "$bytes" -lt 20000 ]; then
                echo "!! $SHOT is only ${bytes}B — that is a BLANK/flat frame, not a"
                echo "   booted shell. Treating this capture as failed."
                rm -f "$SHOT"
                exit 1
            fi
        fi
    else
        echo "!! grim not installed (apt install grim); cannot screenshot"
    fi
    exit 0
fi

echo "==> simulator running. Ctrl-C to quit."
wait "$ZPID"
