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
# Env: BUILD (build-host), WLR_RENDERER (gles2|pixman fallback), SHOT_DELAY (7),
#      ZELTO_DATA_DIR (/tmp/zelto-sim/data), SIM_APP (extra app to launch).
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

export ZELTO_FONT="${ZELTO_FONT:-$REPO_ROOT/sdk/assets/fonts/ZeltoSans.ttf}"
# No /dev/vda here — apps' persistent storage lives under a host temp dir instead
# of the phone's /var/zelto (libzelto reads $ZELTO_DATA_DIR, storage.c).
export ZELTO_DATA_DIR="${ZELTO_DATA_DIR:-/tmp/zelto-sim/data}"
mkdir -p "$ZELTO_DATA_DIR/apps"
rm -f "$XDG_RUNTIME_DIR/zsysd.sock"     # drop a stale broker socket from a prior run
pkill -f "$BUILD/compositor/zcomp" 2>/dev/null || true   # reap a stale sim compositor

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
rm -f "$MANIFEST_OUT"/*.app
for m in "$REPO_ROOT/samples/hello/zelto-hello.app" \
         "$REPO_ROOT/system/apps/cards/zelto-cards.app" \
         "$REPO_ROOT/system/share/zelto-share.app" \
         "$REPO_ROOT/system/apps/notes/zelto-notes.app" \
         "$REPO_ROOT/system/apps/pinger/zelto-pinger.app" \
         "$REPO_ROOT/system/apps/notepad/zelto-notepad.app" \
         "$REPO_ROOT/system/apps/settings/zelto-settings.app" \
         "$REPO_ROOT/system/apps/fetch/zelto-fetch.app" \
         "$REPO_ROOT/system/apps/store/zelto-store.app"; do
    [ -f "$m" ] || { echo "!! manifest missing: $m"; continue; }
    # Pull the image exec path (/usr/bin/zelto-X), find that binary under build-host.
    exec_img="$(sed -n 's/^exec=//p' "$m" | head -1)"
    bin_name="$(basename "$exec_img")"
    bin_host="$(find "$BUILD/system" "$BUILD/samples" -type f -name "$bin_name" -perm -u+x 2>/dev/null | head -1)"
    if [ -z "$bin_host" ]; then
        echo "!! host binary missing for $(basename "$m") ($bin_name); skipping tile"
        continue
    fi
    # Copy the manifest verbatim but repoint exec= at the host binary.
    sed "s#^exec=.*#exec=$bin_host#" "$m" > "$MANIFEST_OUT/$(basename "$m")"
    echo "    tile: $(basename "$m" .app) -> $bin_host"
done

# Launch the shell in the same order /init does (minus the kernel/udev/net/binder
# bring-up, which the desktop already provides). Each piece is a client of zcomp.
spawn() { [ -x "$1" ] && { "$@" & PIDS+=($!); }; }
spawn "$SYS/zsysd/zsysd"
spawn "$SYS/bar/zelto-bar"
spawn "$SYS/nav/zelto-nav"
spawn "$SYS/keyboard/zelto-keyboard"
spawn "$SYS/shade/zelto-shade"
spawn "$SYS/dim/zelto-dim"
spawn "$SYS/lock/zelto-lock"
sleep 1
spawn "$SYS/launcher/zelto-launcher"

# Optional: auto-launch an app (name like "zelto-notepad", or a full path), handy
# for a headless screenshot of a specific app without scripting a tile tap.
if [ -n "${SIM_APP:-}" ]; then
    APP="$SIM_APP"
    [ -x "$APP" ] || APP="$(find "$BUILD/system" "$BUILD/samples" -type f -name "$SIM_APP" -perm -u+x 2>/dev/null | head -1)"
    sleep 2
    spawn "$APP"
fi

# Headless + SHOT: give it a moment to render, grab a PNG with grim, then exit —
# the agent/CI verification path. Otherwise block on zcomp (interactive window).
if [ -n "${SHOT:-}" ]; then
    sleep "$SHOT_DELAY"
    mkdir -p "$(dirname "$SHOT")"
    if command -v grim >/dev/null 2>&1; then
        grim "$SHOT" && echo "==> wrote $SHOT"
    else
        echo "!! grim not installed (apt install grim); cannot screenshot"
    fi
    exit 0
fi

echo "==> simulator running. Ctrl-C to quit."
wait "$ZPID"
