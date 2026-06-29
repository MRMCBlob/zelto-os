#!/usr/bin/env bash
# Build (if needed) and launch Zelto OS in QEMU (aarch64 'virt').
#
# Boots the mainline kernel + tiny initramfs, attaches a virtio-gpu display and
# virtio input, and runs zcomp -> a rendered frame. This is the success path for
# the bootstrap task.
#
# Usage:
#   meta/run-qemu.sh              # build everything, show a GTK window (WSLg)
#   HEADLESS=1 meta/run-qemu.sh   # no window; dump a frame to out/frame.ppm
#   SKIP_BUILD=1 meta/run-qemu.sh # just launch with existing artifacts
#
# Env:
#   MEM (default 2048)   SMP (default 4)   CPU (default cortex-a72)
#   SHOT_DELAY (default 8) seconds to wait before the headless screendump
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
OUT="$REPO_ROOT/device/qemu-virt/out"

MEM="${MEM:-4096}"
SMP="${SMP:-4}"
CPU="${CPU:-cortex-a72}"
SHOT_DELAY="${SHOT_DELAY:-16}"
# There is no aarch64 KVM on an x86 host, so QEMU emulates via TCG. Multi-threaded
# TCG (one host thread per vCPU) plus a larger translation-block cache is the
# biggest lever we have on the lag (the "[libinput] your system is too slow"
# warning is the guest clock outrunning emulation). Override with ACCEL= if a
# host ever offers KVM.
ACCEL="${ACCEL:-tcg,thread=multi,tb-size=1024}"

KERNEL="$OUT/Image"
INITRD="$OUT/initramfs.cpio.gz"
# Persistent data disk (P11): a virtio-blk image the guest mounts at /var/zelto.
# It lives in out/ so it persists across runs (and across the STORAGE two-boot).
DATA_IMG="${DATA_IMG:-$OUT/data.img}"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> [1/3] cross-build zcomp (aarch64)"
    if [ ! -d "$REPO_ROOT/build-arm64" ]; then
        meson setup "$REPO_ROOT/build-arm64" "$REPO_ROOT" \
            --cross-file "$REPO_ROOT/meta/cross/aarch64-linux-gnu.txt"
    fi
    ninja -C "$REPO_ROOT/build-arm64"

    echo "==> [2/3] kernel"
    [ -f "$KERNEL" ] || "$REPO_ROOT/device/qemu-virt/build-kernel.sh"

    echo "==> [3/3] initramfs"
    "$REPO_ROOT/meta/initramfs/build-initramfs.sh"
fi

# Ensure the persistent data disk exists (idempotent; created+formatted once).
"$REPO_ROOT/meta/mkdata.sh" "$DATA_IMG"

[ -f "$KERNEL" ] || { echo "ERROR: kernel missing ($KERNEL); run device/qemu-virt/build-kernel.sh"; exit 1; }
[ -f "$INITRD" ] || { echo "ERROR: initramfs missing ($INITRD)"; exit 1; }

# QEMU's kernel loader cannot mmap files on a WSL 9p/drvfs mount (/mnt/c/...).
# If the artifacts live there, stage them to a native tmp dir before launching.
case "$KERNEL" in
    /mnt/*)
        STAGE="$(mktemp -d "${TMPDIR:-/tmp}/zelto-run.XXXXXX")"
        echo "==> staging kernel+initramfs to native fs ($STAGE) for QEMU"
        cp -f "$KERNEL" "$STAGE/Image"
        cp -f "$INITRD" "$STAGE/initramfs.cpio.gz"
        KERNEL="$STAGE/Image"
        INITRD="$STAGE/initramfs.cpio.gz"
        trap 'rm -rf "$STAGE"' EXIT
        ;;
esac

# Common QEMU arguments.
common=(
    -M virt
    -accel "$ACCEL"
    -cpu "$CPU"
    -smp "$SMP"
    -m "$MEM"
    -kernel "$KERNEL"
    -initrd "$INITRD"
    # virtio-gpu (DRM/KMS in guest) + virtio input.
    -device virtio-gpu-pci
    -device virtio-keyboard-pci
    -device virtio-tablet-pci
    # Persistent data disk -> /dev/vda in the guest (P11). file.locking=off so the
    # image opens on a WSL drvfs (/mnt/c) mount, whose 9p layer lacks OFD locks.
    -drive "file=$DATA_IMG,if=none,format=raw,id=data,file.locking=off,cache=writeback"
    -device virtio-blk-pci,drive=data
    -no-reboot
)

KCMD="console=ttyAMA0 rdinit=/init loglevel=7"

if [ "${HEADLESS:-0}" = "1" ]; then
    echo "==> launching QEMU headless; frame -> $OUT/frame.ppm after ${SHOT_DELAY}s"
    rm -f "$OUT/frame.ppm" "$OUT/frame-after.ppm"

    # ---------------------------------------------------------------------
    # P11 persistent-storage flow (STORAGE=1): a TWO-BOOT test against the same
    # virtio-blk data.img. Boot #1 launches Notepad and taps "Add note" a few
    # times (each tap bumps a prefs counter AND inserts a SQLite row in the app's
    # private dir on /var/zelto), captures count = N, syncs, and kills QEMU.
    # Boot #2 is a *fresh* QEMU process on the SAME disk image: it relaunches
    # Notepad, which reads the counter back via z_prefs_get_int + the rows via
    # z_db_query and shows the value survived (still N, not 0). This is the whole
    # success criterion for the phase. Self-contained (its own two launches), so
    # it runs and exits before the single-boot path below. Coordinates are
    # overridable to retune to the rendered layout from a captured frame (rerun
    # with SKIP_BUILD=1 + overrides). Boot is slow under TCG: keep SHOT_DELAY high.
    if [ "${STORAGE:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"                 # x over a launcher tile
        NOTEPAD_TILE_Y="${NOTEPAD_TILE_Y:-265}" # "Notepad" tile centre (retune!)
        ADD_X="${ADD_X:-640}"                   # "Add note" button
        ADD_Y="${ADD_Y:-470}"
        TAPS="${TAPS:-3}"                       # how many notes to add on boot #1
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

        # qmp uses the current $QMP_SOCK (set by storage_boot for each boot).
        qmp() {
            [ "$have_socat" = "1" ] || return 0
            printf '%s\n' '{"execute":"qmp_capabilities"}' "$1" \
                | socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
        }
        to_png() {
            [ -f "$1" ] || return 0
            echo "==> wrote $1"
            if command -v pnmtopng >/dev/null 2>&1; then
                pnmtopng "$1" > "$2" 2>/dev/null && echo "==> wrote $2"
            elif command -v convert >/dev/null 2>&1; then
                convert "$1" "$2" && echo "==> wrote $2"
            elif command -v python3 >/dev/null 2>&1; then
                python3 "$REPO_ROOT/meta/ppm2png.py" "$1" "$2" && echo "==> wrote $2"
            fi
        }
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        tap() { move "$1" "$2"; sleep 0.2; btn down; sleep 0.1; btn up; }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        # Launch one fresh QEMU bound to a new QMP socket; sets QMP_SOCK + QPID.
        storage_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        storage_kill() {
            sync                                   # flush host page cache too
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [storage boot 1/2] launch; write counter + SQLite rows"
        storage_boot
        sleep "$SHOT_DELAY"
        shot frame-storage-launcher               # read the Notepad tile y off this
        echo "==> [storage 1] tap 'Notepad' tile"
        tap "$TILE_X" "$NOTEPAD_TILE_Y"
        sleep 5; shot frame-storage-app           # Notepad: loaded count=0 (first run)
        i=0
        while [ "$i" -lt "$TAPS" ]; do
            echo "==> [storage 1] tap 'Add note' ($((i + 1))/$TAPS)"
            tap "$ADD_X" "$ADD_Y"
            sleep 2
            i=$((i + 1))
        done
        shot frame-storage-write                  # count = N, rows listed
        echo "==> [storage 1] sync + shutdown"
        sleep 3                                    # let the periodic sync flush
        storage_kill

        echo "==> [storage boot 2/2] REBOOT same disk; read persisted value back"
        storage_boot
        sleep "$SHOT_DELAY"
        shot frame-storage-reboot-launcher
        echo "==> [storage 2] relaunch 'Notepad'"
        tap "$TILE_X" "$NOTEPAD_TILE_Y"
        sleep 5
        shot frame-storage-persisted              # loaded count=N (survived!) + rows
        shot frame-storage-rows                   # same frame; the DB query result
        storage_kill
        echo "==> storage two-boot test done; frames in $OUT/frame-storage-*.png"
        exit 0
    fi

    # The QMP unix socket must live on a native fs (9p/drvfs can't bind sockets).
    QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
    rm -f "$QMP_SOCK"
    qemu-system-aarch64 "${common[@]}" \
        -append "$KCMD" \
        -display none \
        -serial mon:stdio \
        -qmp "unix:$QMP_SOCK,server,nowait" &
    QPID=$!

    have_socat=0
    command -v socat >/dev/null 2>&1 && have_socat=1
    [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

    # Send one QMP command (JSON, sans the capabilities handshake) to the socket.
    qmp() {
        [ "$have_socat" = "1" ] || return 0
        printf '%s\n' '{"execute":"qmp_capabilities"}' "$1" \
            | socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
    }
    # PPM -> PNG (netpbm / ImageMagick / pure-python fallback).
    to_png() {
        [ -f "$1" ] || return 0
        echo "==> wrote $1"
        if command -v pnmtopng >/dev/null 2>&1; then
            pnmtopng "$1" > "$2" 2>/dev/null && echo "==> wrote $2"
        elif command -v convert >/dev/null 2>&1; then
            convert "$1" "$2" && echo "==> wrote $2"
        elif command -v python3 >/dev/null 2>&1; then
            python3 "$REPO_ROOT/meta/ppm2png.py" "$1" "$2" && echo "==> wrote $2"
        fi
    }

    # Wait for the GPU + compositor to paint, then screendump the "before" frame.
    sleep "$SHOT_DELAY"
    qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/frame.ppm\"}}"
    sleep 1
    to_png "$OUT/frame.ppm" "$OUT/frame.png"

    # Optional: drive the System UI over QMP input-send-event and capture a
    # *sequence* of frames proving the P7 app model works:
    #   - the launcher builds its tiles from on-disk manifests;
    #   - launching app #1 makes it Active (xdg activated-state lifecycle);
    #   - launching app #2 backgrounds app #1 (it shows "PAUSED");
    #   - the launcher's "Running" section (wlr-foreign-toplevel-management)
    #     lists both apps; tapping app #1 there switches to it (it resumes,
    #     app #2 pauses) via foreign-toplevel activate;
    #   - tapping the × on app #2's card closes it (it drops out of the list).
    # Each stage dumps a PNG. Coordinates are on-screen pixels mapped into the
    # 0..32767 absolute input range. The "Running" card coordinates are
    # overridable (RUN_X / RUN1_Y / RUN2_Y / CLOSE_X) so they can be retuned to
    # the rendered layout without a rebuild (SKIP_BUILD=1).
    if [ "${INJECT:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"           # x over a launcher tile (tiles are wide)
        TILE1_Y="${TILE1_Y:-170}"         # "Rows" tile centre (below the bar)
        TILE2_Y="${TILE2_Y:-275}"         # "Cards" tile centre
        RUN_X="${RUN_X:-260}"             # x over a Running card body (switch)
        RUN1_Y="${RUN1_Y:-430}"           # first Running card (app #1) centre
        RUN2_Y="${RUN2_Y:-540}"           # second Running card (app #2) centre
        CLOSE_X="${CLOSE_X:-1230}"        # x over a Running card's × button
        HOME_KEY="${HOME_KEY:-home}"      # compositor Home chord -> launcher
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        # Move the absolute pointer to (x,y) px.
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {  # btn down|up
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        tap() {  # tap X Y : press + release at one spot (a tap, no drag)
            move "$1" "$2"; sleep 0.2; btn down; sleep 0.1; btn up
        }
        keypress() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
        }
        shot() {  # shot NAME : screendump <out>/NAME.ppm -> NAME.png
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }

        echo "==> [inject 0/6] shell: launcher with manifest tiles + status bar"
        shot frame-launcher

        echo "==> [inject 1/6] tap 'Rows' tile -> launch app #1 (-> Active)"
        tap "$TILE_X" "$TILE1_Y"
        sleep 4; shot frame-app1        # app #1 mapped, Active banner, under bar

        echo "==> [inject 2/6] Home -> launcher (app #1 now in Running list)"
        keypress "$HOME_KEY"
        sleep 2

        echo "==> [inject 3/6] tap 'Cards' tile -> launch app #2 (app #1 pauses)"
        tap "$TILE_X" "$TILE2_Y"
        sleep 4; shot frame-app2        # app #2 Active in front, under the bar

        echo "==> [inject 4/6] Home -> launcher Running list (app1 paused + app2)"
        keypress "$HOME_KEY"
        sleep 2; shot frame-running

        echo "==> [inject 5/6] tap app #1 in Running list -> switch (it resumes)"
        tap "$RUN_X" "$RUN1_Y"
        sleep 3; shot frame-resumed     # app #1 back in front, Active again

        echo "==> [inject 6/6] Home -> launcher; close app #2 via its × button"
        keypress "$HOME_KEY"
        sleep 2
        tap "$CLOSE_X" "$RUN2_Y"        # × on app #2's Running card
        sleep 3; shot frame-closed      # app #2 terminated, gone from the list
    fi

    # Optional: drive the P8 system-service flow over QMP and capture a sequence
    # proving the permission broker + consent modal end-to-end:
    #   - launch the Cards app (it declares `permissions=camera`);
    #   - tap "Use camera" -> z_perm_request -> zsysd has no stored grant, so it
    #     shows the System-UI consent dialog (an OVERLAY layer-shell modal);
    #   - tap Allow -> zsysd records the grant, returns granted, the dialog
    #     dismisses and the app flips to its granted ("Camera ready") state;
    #   - re-tap "Use camera" -> the cached grant returns immediately, NO dialog.
    # Each stage dumps a PNG. Element coordinates are overridable so they can be
    # retuned to the rendered layout from a captured frame without a rebuild
    # (run again with SKIP_BUILD=1 and the overrides).
    if [ "${PERM:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"           # x over a launcher tile
        CARDS_Y="${CARDS_Y:-275}"         # "Cards" tile centre (below the bar)
        CAM_X="${CAM_X:-640}"             # x over the "Use camera" button
        CAM_Y="${CAM_Y:-470}"             # y of the "Use camera" button
        ALLOW_X="${ALLOW_X:-820}"         # x over the consent dialog's Allow
        ALLOW_Y="${ALLOW_Y:-500}"         # y of the consent dialog's Allow
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        tap() { move "$1" "$2"; sleep 0.2; btn down; sleep 0.1; btn up; }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }

        echo "==> [perm 0/4] shell: launcher + status bar"
        shot frame-perm-launcher

        echo "==> [perm 1/4] tap 'Cards' tile -> launch the camera-demo app"
        tap "$TILE_X" "$CARDS_Y"
        sleep 4; shot frame-perm-app        # Cards: "camera: prompt" + Use camera

        echo "==> [perm 2/4] tap 'Use camera' -> zsysd shows the consent modal"
        tap "$CAM_X" "$CAM_Y"
        sleep 4; shot frame-perm-consent    # overlay modal: Allow / Deny

        echo "==> [perm 3/4] tap Allow -> grant recorded, app flips to granted"
        tap "$ALLOW_X" "$ALLOW_Y"
        sleep 4; shot frame-perm-granted    # "camera: granted" + Camera ready

        echo "==> [perm 4/4] re-tap 'Use camera' -> cached grant, NO dialog"
        tap "$CAM_X" "$CAM_Y"
        sleep 3; shot frame-perm-recheck    # still granted, no overlay (fast path)
    fi

    # Optional: drive the P9 app-to-app intents flow over QMP and capture a
    # sequence proving deep links + the share sheet end-to-end:
    #   - launch the "Share" source app;
    #   - tap "Open note link" -> z_open_url("zelto://note/42") -> zsysd resolves
    #     the single "zelto" handler (Notes), launches it (it isn't running) and
    #     pushes the deliver; Notes' z_on_open_url fires and it shows the URL;
    #   - switch back to Share via the launcher's Running list;
    #   - tap "Share text" -> z_share(text/plain) -> zsysd resolves the apps that
    #     accept text/* and the System UI shows the share-sheet chooser (overlay);
    #   - tap the Notes row -> zsysd delivers to the already-running Notes; its
    #     z_on_share_target fires and it shows the shared text, the sheet dismisses.
    # Each stage dumps a PNG. Coordinates are overridable so they can be retuned
    # to the rendered layout from a captured frame without a rebuild (rerun with
    # SKIP_BUILD=1 and the overrides). Boot is slow under TCG: SHOT_DELAY high.
    if [ "${SHARE:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"           # x over a launcher tile
        SHARE_TILE_Y="${SHARE_TILE_Y:-158}"   # "Share" tile centre (1st tile)
        LINK_X="${LINK_X:-640}"           # "Open note link" button
        LINK_Y="${LINK_Y:-490}"
        SHARE_BTN_X="${SHARE_BTN_X:-640}" # "Share text" button
        SHARE_BTN_Y="${SHARE_BTN_Y:-425}"
        RUN_X="${RUN_X:-260}"             # x over a Running card body (switch)
        RUNSHARE_Y="${RUNSHARE_Y:-620}"   # Share's Running card centre
        RUNNOTES_Y="${RUNNOTES_Y:-718}"   # Notes' Running card centre
        SHEET_X="${SHEET_X:-640}"         # x over a share-sheet row
        SHEET_ROW_Y="${SHEET_ROW_Y:-342}" # first candidate row centre in the sheet
        HOME_KEY="${HOME_KEY:-home}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        tap() { move "$1" "$2"; sleep 0.2; btn down; sleep 0.1; btn up; }
        keypress() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"$1\"}}}]}}"
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }

        echo "==> [share 0/6] shell: launcher with Share + Notes tiles"
        shot frame-share-launcher

        echo "==> [share 1/6] tap 'Share' tile -> launch the intent-source app"
        tap "$TILE_X" "$SHARE_TILE_Y"
        sleep 4; shot frame-share-source     # Share app: Share text / Open note link

        echo "==> [share 2/6] tap 'Open note link' -> deep link launches Notes"
        tap "$LINK_X" "$LINK_Y"
        sleep 5; shot frame-share-deeplink   # Notes shows Opened: zelto://note/42

        echo "==> [share 3/6] Home -> launcher; switch back to Share (Running list)"
        keypress "$HOME_KEY"
        sleep 2
        tap "$RUN_X" "$RUNSHARE_Y"
        sleep 3; shot frame-share-back        # Share app in front again

        echo "==> [share 4/6] tap 'Share text' -> share sheet (chooser overlay)"
        tap "$SHARE_BTN_X" "$SHARE_BTN_Y"
        sleep 4; shot frame-share-sheet       # overlay sheet listing Notes

        echo "==> [share 5/6] tap the Notes row -> delivered to running Notes"
        tap "$SHEET_X" "$SHEET_ROW_Y"
        sleep 4; shot frame-share-picked      # sheet dismissed, Share back in front

        # Notes received the share while backgrounded; raise it to capture the
        # delivered text (it now shows BOTH the deep-linked URL and the shared
        # text — the proof that both intents reached the same running target).
        echo "==> [share 6/6] Home -> raise Notes (Running list) to show payloads"
        keypress "$HOME_KEY"
        sleep 2
        tap "$RUN_X" "$RUNNOTES_Y"
        sleep 4; shot frame-share-delivered   # Notes: Shared text + Opened link
    fi

    # Optional: drive the P10 notifications flow over QMP and capture a sequence
    # proving heads-up banners + tap-route + action routing end-to-end:
    #   - launch "Pinger" (declares permissions=notifications);
    #   - tap "Post" -> z_notify_post -> no stored grant, so zsysd shows the P8
    #     consent modal;
    #   - tap Allow -> zsysd assigns an id, stores it, and pushes notify_show to
    #     the shade, which renders a heads-up banner card (app + title + body +
    #     the "Ack" action) as an OVERLAY layer-shell strip over the apps;
    #   - tap the banner's "Ack" action -> zsysd routes it to Pinger's mailbox,
    #     z_on_notification_action fires and Pinger (still in front) shows
    #     "last action: ack"; the banner is dropped;
    #   - tap "Post" again (grant cached -> no consent, banner at once);
    #   - tap the banner body -> the shade opens its tap_route (zelto://note/7)
    #     via z_open_url -> Notes launches and z_on_open_url shows the URL.
    # z_notify_post pumps the Wayland connection while it waits for the broker, so
    # Pinger survives the (multi-second) consent and stays the foreground app for
    # the whole flow — no launcher round-trip needed. Coordinates overridable so
    # they can be retuned to the rendered layout from a captured frame without a
    # rebuild (rerun with SKIP_BUILD=1 + overrides). Boot is slow under TCG:
    # keep SHOT_DELAY high.
    if [ "${NOTIFY:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"               # x over a launcher tile
        PINGER_TILE_Y="${PINGER_TILE_Y:-265}" # "Pinger" tile centre (2nd tile)
        POST_X="${POST_X:-640}"               # "Post" button in Pinger
        POST_Y="${POST_Y:-392}"
        ALLOW_X="${ALLOW_X:-894}"             # consent dialog's Allow
        ALLOW_Y="${ALLOW_Y:-527}"
        BANNER_BODY_X="${BANNER_BODY_X:-300}" # banner card body (left column)
        BANNER_BODY_Y="${BANNER_BODY_Y:-97}"
        BANNER_ACT_X="${BANNER_ACT_X:-1224}"  # banner card action button (right)
        BANNER_ACT_Y="${BANNER_ACT_Y:-97}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }
        move() {
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$(ax "$1")}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$(ay "$2")}}]}}"
        }
        btn() {
            local d=true; [ "$1" = up ] && d=false
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$d,\"button\":\"left\"}}]}}"
        }
        tap() { move "$1" "$2"; sleep 0.2; btn down; sleep 0.1; btn up; }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }

        echo "==> [notify 0/6] shell: launcher with the Pinger tile"
        shot frame-notify-launcher

        echo "==> [notify 1/6] tap 'Pinger' tile -> launch the source app"
        tap "$TILE_X" "$PINGER_TILE_Y"
        sleep 4; shot frame-notify-pinger     # Pinger before posting (Post button)

        echo "==> [notify 2/6] tap 'Post' -> zsysd shows the consent modal"
        tap "$POST_X" "$POST_Y"
        sleep 4; shot frame-notify-consent    # overlay modal: Allow / Deny

        echo "==> [notify 3/6] tap Allow -> grant + banner pushed to the shade"
        tap "$ALLOW_X" "$ALLOW_Y"
        sleep 4; shot frame-notify-banner     # heads-up banner card (top strip)

        echo "==> [notify 4/6] tap the 'Ack' action -> routed back to Pinger"
        tap "$BANNER_ACT_X" "$BANNER_ACT_Y"
        sleep 4; shot frame-notify-action     # Pinger: last action: ack

        echo "==> [notify 5/6] tap 'Post' again -> banner (grant cached, no modal)"
        tap "$POST_X" "$POST_Y"
        sleep 4; shot frame-notify-banner2

        echo "==> [notify 6/6] tap banner body -> deep link routes to Notes"
        tap "$BANNER_BODY_X" "$BANNER_BODY_Y"
        sleep 5; shot frame-notify-deeplink   # Notes shows Opened: zelto://note/7
    fi

    kill "$QPID" 2>/dev/null || true
else
    echo "==> launching QEMU with GTK display (WSLg)"
    exec qemu-system-aarch64 "${common[@]}" \
        -append "$KCMD" \
        -display gtk,gl=off \
        -serial mon:stdio
fi
