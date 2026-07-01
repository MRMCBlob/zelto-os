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
    # Networking (P12): QEMU user-mode (slirp) NAT. Guest gets 10.0.2.15, host is
    # 10.0.2.2. Always present (harmless when unused); the NET=1 harness starts a
    # host HTTP endpoint the Fetch app GETs through 10.0.2.2.
    -netdev user,id=net0
    -device virtio-net-pci,netdev=net0
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

    # ---------------------------------------------------------------------
    # P13 packaging/install flow (INSTALL=1): install a signed .zap at runtime
    # onto the persistent disk, then prove it survives a reboot and runs. A
    # TWO-BOOT test against the same data.img (like STORAGE):
    #   Boot #1 — the launcher shows NO Widget tile (Widget is not baked in; it
    #     ships only inside /usr/share/zelto/packages/widget.zap). Open the Store
    #     app and tap "Install Widget": it fork/execs zelto-install, which verifies
    #     the package's Ed25519 signature + every file hash, unpacks the binary to
    #     /var/zelto/installed/os.zelto.widget/, and registers its manifest under
    #     /var/zelto/apps/manifests/. Then tap "Install tampered": zelto-install
    #     refuses widget-bad.zap (a byte was flipped after signing -> hash
    #     mismatch), so nothing is registered. Sync + shutdown.
    #   Boot #2 — a FRESH QEMU on the SAME disk. The launcher's startup scan now
    #     reads the runtime-installed manifest, so a Widget tile appears (it
    #     persisted). Tap it -> the installed binary maps its window.
    # This captures all four required proofs (before / install / after / running)
    # plus the rejection and the bonus reboot-persistence. Coordinates are
    # overridable to retune to the rendered layout from a captured frame (rerun
    # with SKIP_BUILD=1 + overrides — first guesses miss). Boot is slow under TCG:
    # keep SHOT_DELAY high and give the install generous time.
    if [ "${INSTALL:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        # Tiles are alphabetical (launcher sorts by name). Boot #1 has 8 tiles
        # (Store is last/bottom); boot #2 has 9 (Widget appended after Store).
        TILE_X="${TILE_X:-300}"                 # x over a launcher tile
        STORE_TILE_Y="${STORE_TILE_Y:-655}"     # "Store" tile centre (8-tile list)
        WIDGET_TILE_Y="${WIDGET_TILE_Y:-714}"   # "Widget" tile centre (9-tile list)
        INSTALL_X="${INSTALL_X:-640}"           # "Install Widget" button
        INSTALL_Y="${INSTALL_Y:-422}"
        TAMPER_X="${TAMPER_X:-640}"             # "Install tampered" button
        TAMPER_Y="${TAMPER_Y:-485}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        install_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        install_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [install boot 1/2] launcher (no Widget tile yet)"
        install_boot
        sleep "$SHOT_DELAY"
        shot frame-install-launcher          # BEFORE: no Widget tile; read Store y
        echo "==> [install 1] tap 'Store' tile"
        tap "$TILE_X" "$STORE_TILE_Y"
        sleep 5; shot frame-install-store     # Store UI: Install Widget / tampered
        echo "==> [install 2] tap 'Install Widget' -> verify + unpack to /var/zelto"
        tap "$INSTALL_X" "$INSTALL_Y"
        sleep 7; shot frame-install-installed # "Widget installed OK (exit 0)"
        echo "==> [install 3] tap 'Install tampered' -> rejected (hash mismatch)"
        tap "$TAMPER_X" "$TAMPER_Y"
        sleep 7; shot frame-install-rejected  # "Tampered package rejected (exit N)"
        echo "==> [install 1] sync + shutdown"
        sleep 3
        install_kill

        echo "==> [install boot 2/2] REBOOT same disk; Widget tile persisted"
        install_boot
        sleep "$SHOT_DELAY"
        shot frame-install-launcher2          # AFTER: Widget tile now present
        echo "==> [install 4] tap 'Widget' tile -> the installed app runs"
        tap "$TILE_X" "$WIDGET_TILE_Y"
        sleep 5; shot frame-install-widget    # Widget app mapped (installed binary)
        install_kill
        echo "==> install test done; frames in $OUT/frame-install-*.png"
        exit 0
    fi

    # ---------------------------------------------------------------------
    # P14 home screen + system navigation (NAV=1): a SINGLE-boot test that the
    # grid home screen + bottom 3-button nav bar drive the existing window
    # manager. The flow exercises every button against P6/P7 machinery:
    #   1. Boot to the grid home screen (square icons) with a bottom nav bar
    #      (Back / Home / Recents). The idle shade is collapsed (1px) so the
    #      top row of icons is not covered.
    #   2. Tap an app icon (A) -> app A maps in the usable area (above the nav).
    #   3. Tap Home -> the launcher grid returns to front (foreign-toplevel
    #      activate of os.zelto.launcher). Tap a second icon (B) -> app B maps.
    #   4. Tap Recents -> the zelto-recents overlay lists A + B as cards.
    #      Tap card A -> z_task_activate(A) + dismiss (A is now foreground).
    #   5. Tap Recents again -> tap a card's X -> z_task_close drops that window.
    # Coordinates are overridable to retune to the rendered layout from a captured
    # frame (rerun with SKIP_BUILD=1 + overrides — first guesses miss). Boot is
    # slow under TCG: keep SHOT_DELAY high.
    if [ "${NAV:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        # Grid icon centres (4 columns; the launcher sorts apps alphabetically).
        # The app area starts below the 40px top bar, so y includes that offset.
        TILE_AX="${TILE_AX:-170}"; TILE_AY="${TILE_AY:-165}"   # icon col 1, row 1 (app A)
        TILE_BX="${TILE_BX:-483}"; TILE_BY="${TILE_BY:-165}"   # icon col 2, row 1 (app B)
        # Bottom nav buttons (strip y ~ 736..800; centre ~768).
        BACK_X="${BACK_X:-213}";    NAV_Y="${NAV_Y:-768}"
        HOME_X="${HOME_X:-640}"
        RECENTS_X="${RECENTS_X:-1067}"
        # Recents overlay card centres (card is ~620px wide, centred).
        CARD_AY="${CARD_AY:-300}"                # first (top) task card
        CARD_CLOSE_X="${CARD_CLOSE_X:-900}"      # the card's red X button
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        nav_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        nav_kill() {
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [nav] boot to grid home screen + bottom nav bar"
        nav_boot
        sleep "$SHOT_DELAY"
        shot frame-nav-home                 # grid icons + Back/Home/Recents bar
        echo "==> [nav] tap app A icon -> maps above the nav bar"
        tap "$TILE_AX" "$TILE_AY"
        sleep 5; shot frame-nav-appA
        echo "==> [nav] tap Home -> grid returns to front"
        tap "$HOME_X" "$NAV_Y"
        sleep 3; shot frame-nav-home2
        echo "==> [nav] tap app B icon -> second app maps (A + B both running)"
        tap "$TILE_BX" "$TILE_BY"
        sleep 5; shot frame-nav-appB
        echo "==> [nav] tap Recents -> overview lists running apps"
        tap "$RECENTS_X" "$NAV_Y"
        sleep 4; shot frame-nav-recents
        echo "==> [nav] tap a Recents card -> switch to it + dismiss overlay"
        tap "$HOME_X" "$CARD_AY"            # tap the top card body (centre-x)
        sleep 4; shot frame-nav-switch
        echo "==> [nav] tap Recents again -> close a window via its X"
        tap "$RECENTS_X" "$NAV_Y"
        sleep 4; shot frame-nav-recents2
        tap "$CARD_CLOSE_X" "$CARD_AY"      # the card's red X
        sleep 4; shot frame-nav-closed
        nav_kill
        echo "==> nav test done; frames in $OUT/frame-nav-*.png"
        exit 0
    fi

    # ---------------------------------------------------------------------
    # P15 home screen + app drawer (HOME=1): a TWO-BOOT test against the same
    # data.img proving (a) the home/drawer split, (b) the slide-up drawer with a
    # live swipe + spring settle, (c) launching from both surfaces, and (d)
    # favourites persisting across a reboot (the prefs string on /var/zelto).
    #   Boot #1 — boot to the wallpapered HOME surface: only the favourites grid
    #     (a subset) + a drawer handle. Launch from a FAVOURITE (tap a home icon).
    #     Home (nav) back to the launcher. Then *swipe up* to open the drawer: the
    #     drag is captured mid-slide WHILE THE BUTTON IS HELD (the offset tracks
    #     the finger live, so the frame is stable), then again once it settles
    #     open showing the FULL app list in a scroll. Launch from the DRAWER (tap
    #     a drawer icon). On boot #1 the launcher finds no stored favourites, seeds
    #     the default set, and persists home.favorites to /var/zelto. Sync + kill.
    #   Boot #2 — a FRESH QEMU on the SAME disk. The launcher reads home.favorites
    #     back (serial log: "favorites loaded from prefs: ..."), so the home grid
    #     shows the same favourites — they survived the reboot.
    # Coordinates are overridable to retune to the rendered layout from a captured
    # frame (rerun with SKIP_BUILD=1 + overrides — first guesses miss). Boot is
    # slow under TCG: keep SHOT_DELAY high and give the spring generous time.
    if [ "${HOME_TEST:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        # Home favourites grid (4 cols, below the 40px top bar). Row-1, col-1 icon.
        FAV_X="${FAV_X:-180}"; FAV_Y="${FAV_Y:-150}"
        # Bottom nav Home button (strip ~736..800; centre ~768).
        HOME_X="${HOME_X:-640}"; NAV_Y="${NAV_Y:-768}"
        # Swipe-up gesture column (x) and its start/mid/end y (screen px).
        SWIPE_X="${SWIPE_X:-640}"
        SWIPE_Y1="${SWIPE_Y1:-690}"   # start: low on the home surface
        SWIPE_MID="${SWIPE_MID:-410}" # held mid-drag capture point
        SWIPE_Y2="${SWIPE_Y2:-110}"   # end: near the top (well past threshold)
        # A drawer app icon that is NOT a favourite (row-2 col-2 = "Rows"),
        # to prove the drawer launches apps absent from the home grid.
        DRAWER_X="${DRAWER_X:-483}"; DRAWER_Y="${DRAWER_Y:-317}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        home_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        home_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [home boot 1/2] wallpapered home: favourites only + drawer handle"
        home_boot
        sleep "$SHOT_DELAY"
        shot frame-home-home
        echo "==> [home 1] launch from a FAVOURITE (tap a home icon)"
        tap "$FAV_X" "$FAV_Y"
        sleep 5; shot frame-home-app-favourite
        echo "==> [home 1] nav Home -> back to the home surface"
        tap "$HOME_X" "$NAV_Y"
        sleep 3; shot frame-home-home2
        echo "==> [home 1] swipe up -> open the drawer (capture mid-drag, held)"
        # Drag in small steps so the pan recognizer tracks continuously; hold at
        # the mid point and capture there (the drawer follows the finger live, so
        # the held frame is stable), then continue past the threshold and release.
        move "$SWIPE_X" "$SWIPE_Y1"; sleep 0.2; btn down; sleep 0.3
        move "$SWIPE_X" 600; sleep 0.4
        move "$SWIPE_X" 510; sleep 0.4
        move "$SWIPE_X" "$SWIPE_MID"; sleep 1.5
        shot frame-home-drawer-mid           # held mid-drag, drawer ~halfway up
        move "$SWIPE_X" 260; sleep 0.4
        move "$SWIPE_X" "$SWIPE_Y2"; sleep 0.3; btn up
        sleep 4; shot frame-home-drawer-open # settled open: full app list, scroll
        echo "==> [home 1] launch from the DRAWER (tap a drawer icon)"
        tap "$DRAWER_X" "$DRAWER_Y"
        sleep 5; shot frame-home-app-drawer
        echo "==> [home 1] sync + shutdown (favourites persisted to /var/zelto)"
        sleep 3
        home_kill

        echo "==> [home boot 2/2] REBOOT same disk; favourites read back from prefs"
        home_boot
        sleep "$SHOT_DELAY"
        shot frame-home-reboot               # same favourites grid (survived reboot)
        home_kill
        echo "==> home test done; frames in $OUT/frame-home-*.png"
        exit 0
    fi

    # ---------------------------------------------------------------------
    # P16 curate favourites + quick-settings shade (QUICK=1): a TWO-BOOT test
    # against the same data.img proving (a) a long-press on a drawer icon adds it
    # to the home favourites, (b) a long-press on a home favourite removes it,
    # both rewriting the home.favorites prefs CSV; and (c) a down-swipe from the
    # top pulls the quick-settings shade down, where tapping a toggle chip flips a
    # persisted bool. Boot #2 reboots the SAME disk and proves the curated
    # favourites AND the toggle survived (read back from /var/zelto prefs).
    #   Boot #1 — home (favourites only). Open the drawer (swipe up), LONG-PRESS a
    #     non-favourite drawer icon -> the curate menu -> tap "Add to home"; close
    #     the drawer -> the icon is now on home. LONG-PRESS a home favourite ->
    #     menu -> "Remove from home" -> it leaves the grid. Then DOWN-SWIPE from
    #     the top edge -> the quick-settings shade slides down; tap the Wi-Fi chip
    #     (it recolours + persists). Sync + kill.
    #   Boot #2 — FRESH QEMU, SAME disk: home shows the curated set (added icon
    #     present, removed favourite gone); pull the shade down again -> the Wi-Fi
    #     chip is still in its flipped state (serial: "favorites loaded from
    #     prefs: ..."). Coordinates are overridable to retune to the rendered
    #     layout from a captured frame (rerun SKIP_BUILD=1 + overrides). Boot is
    #     slow under TCG: keep SHOT_DELAY high and give the long-press hold + the
    #     springs generous time.
    if [ "${QUICK:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        FAV_X="${FAV_X:-180}"; FAV_Y="${FAV_Y:-150}"   # home fav row-1 col-1 (remove)
        HOME_X="${HOME_X:-640}"; NAV_Y="${NAV_Y:-768}" # bottom nav Home
        SWIPE_X="${SWIPE_X:-640}"                       # vertical swipe column
        # A non-favourite drawer icon to ADD (row-2 col-2 = "Rows" once the drawer
        # is open; the drawer header offsets the grid down a little).
        DRAWER_X="${DRAWER_X:-483}"; DRAWER_Y="${DRAWER_Y:-317}"
        # Curate menu buttons (centred modal): Add/Remove is the first button,
        # Cancel the second. Retune to the captured menu frame.
        MENU_BTN_X="${MENU_BTN_X:-640}"
        MENU_ADD_Y="${MENU_ADD_Y:-381}"                # "Add to home" / "Remove..." (Cancel is ~442)
        # Quick-settings: the Wi-Fi chip (first of three across the top card) and a
        # scrim point below the card to close it.
        QS_WIFI_X="${QS_WIFI_X:-230}"; QS_WIFI_Y="${QS_WIFI_Y:-150}"
        QS_SCRIM_X="${QS_SCRIM_X:-640}"; QS_SCRIM_Y="${QS_SCRIM_Y:-640}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        # Press-and-hold in place past the SDK long-press threshold (0.45s): NO
        # move between down and up, or it would cross the slop and become a pan.
        longpress() { move "$1" "$2"; sleep 0.2; btn down; sleep 0.9; btn up; }
        # A multi-step vertical drag from (X,Y1) to (X,Y2); the pan recognizer
        # tracks the finger continuously, so step it rather than jump once.
        drag() {
            local x="$1" y1="$2" y2="$3"
            move "$x" "$y1"; sleep 0.2; btn down; sleep 0.3
            move "$x" $(( (y1*2 + y2) / 3 )); sleep 0.3
            move "$x" $(( (y1 + y2*2) / 3 )); sleep 0.3
            move "$x" "$y2"; sleep 0.4; btn up
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        quick_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        quick_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [quick boot 1/2] home: favourites only"
        quick_boot
        sleep "$SHOT_DELAY"
        shot frame-quick-home
        echo "==> [quick 1] swipe up -> open the app drawer"
        drag "$SWIPE_X" 690 110
        sleep 4; shot frame-quick-drawer
        echo "==> [quick 1] long-press a non-favourite drawer icon -> curate menu"
        longpress "$DRAWER_X" "$DRAWER_Y"
        sleep 2; shot frame-quick-menu-add        # menu: "Add to home"
        echo "==> [quick 1] tap 'Add to home'"
        tap "$MENU_BTN_X" "$MENU_ADD_Y"
        sleep 2; shot frame-quick-added           # drawer still open, fav added
        echo "==> [quick 1] close the drawer (swipe down on the grabber)"
        drag "$SWIPE_X" 90 700
        sleep 3; shot frame-quick-home-added      # home now shows the added icon
        echo "==> [quick 1] long-press a home favourite -> Remove from home"
        longpress "$FAV_X" "$FAV_Y"
        sleep 2; shot frame-quick-menu-remove     # menu: "Remove from home"
        tap "$MENU_BTN_X" "$MENU_ADD_Y"
        sleep 2; shot frame-quick-removed         # that favourite left the grid
        echo "==> [quick 1] down-swipe from the top -> quick-settings shade"
        drag "$SWIPE_X" 70 470
        sleep 3; shot frame-quick-shade           # shade open: clock + chips
        echo "==> [quick 1] tap the Wi-Fi chip -> flips + persists"
        tap "$QS_WIFI_X" "$QS_WIFI_Y"
        sleep 2; shot frame-quick-toggled         # chip recoloured
        echo "==> [quick 1] tap the scrim -> close the shade"
        tap "$QS_SCRIM_X" "$QS_SCRIM_Y"
        sleep 2; shot frame-quick-closed
        echo "==> [quick 1] sync + shutdown (favourites + toggle persisted)"
        sleep 3
        quick_kill

        echo "==> [quick boot 2/2] REBOOT same disk; curated set + toggle survive"
        quick_boot
        sleep "$SHOT_DELAY"
        shot frame-quick-reboot                   # home: added present, removed gone
        echo "==> [quick 2] pull the shade down -> Wi-Fi chip still flipped"
        drag "$SWIPE_X" 70 470
        sleep 3; shot frame-quick-reboot-shade
        quick_kill
        echo "==> quick test done; frames in $OUT/frame-quick-*.png"
        exit 0
    fi

    # ---------------------------------------------------------------------
    # P17 system-wide pull-down shade (SHADE2=1): prove the quick-settings +
    # notifications shade is now a real OVERLAY pull-down that works OVER any
    # running app (not just the home screen, as in P16). A TWO-BOOT test against
    # the same data.img:
    #   Boot #1 — open the app drawer (swipe up) and launch Pinger so a normal app
    #     is in the foreground. Tap "Post" -> the notification consent modal ->
    #     Allow -> Pinger posts a notification and zelto-shade shows a heads-up
    #     banner over Pinger. Now PULL THE SHADE DOWN from the top edge (a
    #     down-swipe starting on the shade's thin grab strip just below the status
    #     bar): the translucent panel slides down OVER Pinger showing the clock,
    #     the Wi-Fi/Mute/Bright toggle chips, and the posted notification in the
    #     list. Tap the Wi-Fi chip (it recolours + persists to the shade's prefs).
    #     Close the shade (tap the see-through area below the panel). Sync + kill.
    #   Boot #2 — FRESH QEMU, SAME disk: pull the shade down again (over the home
    #     screen this time) -> the Wi-Fi chip is still in its flipped state (it was
    #     read back from /var/zelto). This is the toggle-persists-across-reboot
    #     proof. Coordinates are overridable to retune to the rendered layout from
    #     a captured frame (rerun SKIP_BUILD=1 + overrides — first guesses miss).
    #     Boot is slow under TCG: keep SHOT_DELAY high and give the springs +
    #     consent generous time. The grab strip is a thin top-edge region, so the
    #     pull-down swipe MUST start just below the 40px status bar (y ~ 60).
    if [ "${SHADE2:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        SWIPE_X="${SWIPE_X:-640}"                       # vertical swipe column
        # Pinger drawer tile: apps are alphabetical by name in a 4-col grid; on a
        # fresh image Pinger is the 5th (row 2, col 1). Retune from the drawer frame.
        PINGER_X="${PINGER_X:-180}"; PINGER_Y="${PINGER_Y:-317}"
        POST_X="${POST_X:-640}"; POST_Y="${POST_Y:-392}"    # Pinger "Post" button
        ALLOW_X="${ALLOW_X:-894}"; ALLOW_Y="${ALLOW_Y:-527}" # consent dialog Allow
        # The pull-down: start on the grab strip just below the 40px bar, end low.
        PULL_Y1="${PULL_Y1:-72}"; PULL_Y2="${PULL_Y2:-620}"
        # Wi-Fi chip in the pulled-down panel (top row of three chips).
        QS_WIFI_X="${QS_WIFI_X:-230}"; QS_WIFI_Y="${QS_WIFI_Y:-150}"
        # The see-through area below the panel (tap to close the shade).
        QS_CLOSE_X="${QS_CLOSE_X:-640}"; QS_CLOSE_Y="${QS_CLOSE_Y:-750}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        # A multi-step vertical drag; the pan recognizer tracks the finger live, so
        # step it (a single jump fires one CHANGED and misses the live track).
        drag() {
            local x="$1" y1="$2" y2="$3"
            move "$x" "$y1"; sleep 0.2; btn down; sleep 0.3
            move "$x" $(( (y1*2 + y2) / 3 )); sleep 0.3
            move "$x" $(( (y1 + y2*2) / 3 )); sleep 0.3
            move "$x" "$y2"; sleep 0.4; btn up
        }
        # Pull the system shade down: press on the grab strip, NUDGE a little while
        # still on the strip so the shade expands to full BEFORE the finger leaves
        # it (then input stays over the now-full surface), then drag the rest down.
        pull_shade() {
            move "$SWIPE_X" "$PULL_Y1"; sleep 0.3; btn down; sleep 0.4
            move "$SWIPE_X" 108; sleep 0.5       # nudge within the grab strip -> expand
            move "$SWIPE_X" 330; sleep 0.4
            move "$SWIPE_X" "$PULL_Y2"; sleep 0.5; btn up
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        shade_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        shade_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [shade2 boot 1/2] home; open drawer + launch Pinger"
        shade_boot
        sleep "$SHOT_DELAY"
        shot frame-shade2-home
        drag "$SWIPE_X" 690 110            # swipe up -> open the app drawer
        sleep 4; shot frame-shade2-drawer  # read the Pinger tile y off this
        echo "==> [shade2 1] tap Pinger -> it maps in the foreground"
        tap "$PINGER_X" "$PINGER_Y"
        sleep 5; shot frame-shade2-app     # Pinger before posting (Post button)
        echo "==> [shade2 1] tap Post -> consent modal"
        tap "$POST_X" "$POST_Y"
        sleep 4; shot frame-shade2-consent
        echo "==> [shade2 1] Allow -> Pinger posts; heads-up banner over Pinger"
        tap "$ALLOW_X" "$ALLOW_Y"
        sleep 4; shot frame-shade2-banner
        echo "==> [shade2 1] PULL the shade DOWN over Pinger (top-edge swipe)"
        pull_shade
        sleep 5; shot frame-shade2-pulled  # panel over Pinger: clock+chips+notif
        echo "==> [shade2 1] tap Wi-Fi chip -> flips + persists"
        tap "$QS_WIFI_X" "$QS_WIFI_Y"
        sleep 4; shot frame-shade2-toggled
        echo "==> [shade2 1] tap below the panel -> close the shade"
        tap "$QS_CLOSE_X" "$QS_CLOSE_Y"
        sleep 2; shot frame-shade2-closed  # Pinger visible again, shade retracted
        echo "==> [shade2 1] sync + shutdown (Wi-Fi toggle persisted)"
        sleep 3
        shade_kill

        echo "==> [shade2 boot 2/2] REBOOT same disk; pull shade -> Wi-Fi still off"
        shade_boot
        sleep "$SHOT_DELAY"
        shot frame-shade2-reboot
        pull_shade
        sleep 5; shot frame-shade2-reboot-shade   # Wi-Fi chip still in flipped state
        shade_kill
        echo "==> shade2 test done; frames in $OUT/frame-shade2-*.png"
        exit 0
    fi

    # ---------------------------------------------------------------------
    # P18 brokered settings + Settings app (SETTINGS=1): prove the system
    # toggles are now a single zsysd-brokered source of truth read/written by TWO
    # processes (the Settings app AND the shade), live in both directions and
    # persisted across a reboot. A TWO-BOOT test against the same data.img:
    #   Boot #1 — home; open the app drawer (swipe up) and launch Settings. It
    #     shows Wi-Fi On (the broker/default). Tap the Wi-Fi toggle -> it flips to
    #     Off and writes sys.wifi=0 through the broker (persist + broadcast). Now
    #     PULL THE SHADE DOWN over Settings: its Wi-Fi quick-settings chip already
    #     reads Off — it observed the change with no reboot (the live cross-process
    #     proof). Close the shade. Sync + kill.
    #   Boot #2 — FRESH QEMU, SAME disk: launch Settings -> Wi-Fi still Off (the
    #     broker loaded sys.wifi=0 back from /var/zelto, not memory). Pull the
    #     shade down -> its chip is Off too. The persistence + one-source proof.
    # Coordinates are overridable to retune to the rendered layout from a captured
    # frame (rerun SKIP_BUILD=1 + overrides — first guesses miss). Boot is slow
    # under TCG: keep SHOT_DELAY high; the grab strip is a thin top region so the
    # pull-down swipe MUST start just below the 40px bar (y ~ 72).
    if [ "${SETTINGS:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        SWIPE_X="${SWIPE_X:-640}"                       # vertical swipe column
        # Settings drawer tile: apps are alphabetical by name in a 4-col grid; on
        # a fresh image Settings falls in row 2, col 3. Retune from the drawer frame.
        SETTINGS_X="${SETTINGS_X:-786}"; SETTINGS_Y="${SETTINGS_Y:-317}"
        # The Wi-Fi toggle row in the Settings app (label left, On/Off chip right).
        SET_WIFI_X="${SET_WIFI_X:-890}"; SET_WIFI_Y="${SET_WIFI_Y:-265}"
        # The pull-down: start on the grab strip just below the 40px bar, end low.
        PULL_Y1="${PULL_Y1:-72}"; PULL_Y2="${PULL_Y2:-620}"
        # Wi-Fi chip in the pulled-down shade panel (top row of three chips).
        QS_WIFI_X="${QS_WIFI_X:-230}"; QS_WIFI_Y="${QS_WIFI_Y:-150}"
        # The see-through area below the panel (tap to close the shade).
        QS_CLOSE_X="${QS_CLOSE_X:-640}"; QS_CLOSE_Y="${QS_CLOSE_Y:-750}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"

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
        # A near-zero-hold press for launching a drawer/home icon: those carry an
        # OnLongPress (curate menu), and under TCG the guest monotonic clock can run
        # fast enough that a 0.1s held tap is measured past the 0.45s long-press
        # threshold -> the curate menu fires instead of a launch (the documented
        # drawer-tap flake). A press with no hold can't cross the threshold at any
        # clock scale, so it always reads as a tap.
        launchtap() { move "$1" "$2"; sleep 0.3; btn down; btn up; }
        drag() {
            local x="$1" y1="$2" y2="$3"
            move "$x" "$y1"; sleep 0.2; btn down; sleep 0.3
            move "$x" $(( (y1*2 + y2) / 3 )); sleep 0.3
            move "$x" $(( (y1 + y2*2) / 3 )); sleep 0.3
            move "$x" "$y2"; sleep 0.4; btn up
        }
        # Pull the system shade down: press on the grab strip, nudge while still on
        # it so the surface expands to full BEFORE the finger leaves, then drag down.
        pull_shade() {
            move "$SWIPE_X" "$PULL_Y1"; sleep 0.3; btn down; sleep 0.4
            move "$SWIPE_X" 108; sleep 0.5
            move "$SWIPE_X" 330; sleep 0.4
            move "$SWIPE_X" "$PULL_Y2"; sleep 0.5; btn up
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        settings_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        settings_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [settings boot 1/2] home; open drawer + launch Settings"
        settings_boot
        sleep "$SHOT_DELAY"
        shot frame-settings-home
        drag "$SWIPE_X" 690 110            # swipe up -> open the app drawer
        sleep 4; shot frame-settings-drawer  # read the Settings tile y off this
        echo "==> [settings 1] tap Settings -> it maps in the foreground"
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 5; shot frame-settings-app     # Settings: Wi-Fi On (broker default)
        echo "==> [settings 1] tap Wi-Fi toggle -> flips Off + writes sys.wifi=0"
        tap "$SET_WIFI_X" "$SET_WIFI_Y"
        sleep 3; shot frame-settings-flipped # Settings Wi-Fi now Off
        echo "==> [settings 1] PULL the shade DOWN -> its chip already reads Off"
        pull_shade
        sleep 5; shot frame-settings-shade-live  # LIVE cross-process: chip is Off
        echo "==> [settings 1] tap below the panel -> close the shade"
        tap "$QS_CLOSE_X" "$QS_CLOSE_Y"
        sleep 2; shot frame-settings-closed
        echo "==> [settings 1] sync + shutdown (sys.wifi=0 persisted)"
        sleep 3
        settings_kill

        echo "==> [settings boot 2/2] REBOOT same disk; Wi-Fi still Off"
        settings_boot
        sleep "$SHOT_DELAY"
        shot frame-settings-reboot
        drag "$SWIPE_X" 690 110
        sleep 4; shot frame-settings-reboot-drawer
        echo "==> [settings 2] launch Settings -> Wi-Fi loaded Off from /var/zelto"
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 5; shot frame-settings-reboot-app   # Wi-Fi Off survived reboot
        echo "==> [settings 2] pull the shade down -> chip Off too"
        pull_shade
        sleep 5; shot frame-settings-reboot-shade
        settings_kill
        echo "==> settings test done; frames in $OUT/frame-settings-*.png"
        exit 0
    fi

    # P19 settings ACTUATION (ACTUATE=1): prove the brokered toggles now DO
    # something system-wide, not just recolour a chip. THREE actuations + the bar
    # as a third independent broker reader, then persistence. A TWO-BOOT test
    # against the same data.img (the var is ACTUATE, not BRIGHT/DIM, to avoid
    # clobbering a shell env var):
    #   Boot #1 — home (bar shows a green Wi-Fi dot + a mid brightness pip). Open
    #     the drawer, launch Settings. Step Brightness DOWN to 1 -> the whole app
    #     area visibly DIMS (the zelto-dim OVERLAY scrim) AND the bar's brightness
    #     pip shrinks. Toggle Airplane ON -> the bar grows an orange airplane dot
    #     and greys the Wi-Fi dot. Open the drawer, launch Fetch, tap Fetch -> the
    #     GET FAILS immediately ("request failed", NO consent modal) because
    #     sys.airplane gates the network path. Sync + kill (brightness=1,
    #     airplane=1 persisted to /var/zelto).
    #   Boot #2 — FRESH QEMU, SAME disk: at boot the dim overlay reads
    #     sys.brightness=1 back from disk so the screen is ALREADY dimmed, and the
    #     bar shows the airplane dot — both persisted. Launch Settings (shows
    #     Brightness 1, Airplane On), turn Airplane OFF + step Brightness back to 5
    #     -> the screen un-dims. Launch Fetch -> consent -> Allow -> 200 OK (the
    #     network is restored: airplane off => GET succeeds). The off/on proof.
    # Coordinates are overridable (rerun SKIP_BUILD=1 + overrides to retune to the
    # rendered layout). Boot is slow under TCG: keep SHOT_DELAY high. Use launchtap
    # (zero-hold) for drawer launches so a held tap can't flake into the long-press.
    if [ "${ACTUATE:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        SWIPE_X="${SWIPE_X:-640}"
        # Home favourites row (y~115): Cards 172, Fetch 484, Notepad 796, Notes 1108.
        FETCH_FAV_X="${FETCH_FAV_X:-484}"; FAV_Y="${FAV_Y:-115}"
        NAV_HOME_X="${NAV_HOME_X:-630}"; NAV_HOME_Y="${NAV_HOME_Y:-757}"
        SETTINGS_X="${SETTINGS_X:-786}"; SETTINGS_Y="${SETTINGS_Y:-317}"
        # Settings app rows (label left, On/Off chip right at x~890), measured off
        # frame-actuate-settings: Wi-Fi 265, Mute 331, Bright-boost 397, Airplane 463.
        AIR_X="${AIR_X:-890}"; AIR_Y="${AIR_Y:-463}"          # Airplane toggle chip
        # Brightness stepper row (y~529): "-" at x~811, the number, "+" at x~897.
        BRIGHT_DEC_X="${BRIGHT_DEC_X:-811}"
        BRIGHT_INC_X="${BRIGHT_INC_X:-897}"
        BRIGHT_ROW_Y="${BRIGHT_ROW_Y:-529}"
        # The "Fetch" button bar is centred ~y356 (taps at 392 land in the gap below
        # it and never fire the GET — measured off frame-actuate-fetch-app).
        FETCH_BTN_X="${FETCH_BTN_X:-640}"; FETCH_BTN_Y="${FETCH_BTN_Y:-356}"
        ALLOW_X="${ALLOW_X:-894}"; ALLOW_Y="${ALLOW_Y:-527}"
        NET_PORT="${NET_PORT:-8080}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"
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
        launchtap() { move "$1" "$2"; sleep 0.3; btn down; btn up; }
        drag() {
            local x="$1" y1="$2" y2="$3"
            move "$x" "$y1"; sleep 0.2; btn down; sleep 0.3
            move "$x" $(( (y1*2 + y2) / 3 )); sleep 0.3
            move "$x" $(( (y1 + y2*2) / 3 )); sleep 0.3
            move "$x" "$y2"; sleep 0.4; btn up
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        actuate_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        actuate_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        # Host HTTP endpoint (guest reaches it at 10.0.2.2) for the Fetch proof.
        NET_SRV_PID=""
        if command -v python3 >/dev/null 2>&1; then
            SERVE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zelto-www.XXXXXX")"
            printf 'Hello from the Zelto host! (P19 airplane off => GET works)\n' \
                > "$SERVE_DIR/hello.txt"
            echo "==> [actuate] host HTTP server on :$NET_PORT ($SERVE_DIR)"
            ( cd "$SERVE_DIR" && python3 -m http.server "$NET_PORT" ) \
                >/dev/null 2>&1 &
            NET_SRV_PID=$!
            sleep 1
        else
            echo "WARN: python3 not found; Fetch success half can't be checked"
        fi

        echo "==> [actuate boot 1/2] home; bar status cluster (Wi-Fi dot + pip)"
        actuate_boot
        sleep "$SHOT_DELAY"
        shot frame-actuate-home
        # --- network: airplane OFF (persisted default) => Fetch succeeds ---------
        echo "==> [actuate 1] launch Fetch (home favourite); GET -> consent -> 200"
        launchtap "$FETCH_FAV_X" "$FAV_Y"
        sleep 5; shot frame-actuate-fetch-app
        tap "$FETCH_BTN_X" "$FETCH_BTN_Y"
        sleep 4; shot frame-actuate-consent      # airplane off: the perm modal shows
        tap "$ALLOW_X" "$ALLOW_Y"
        sleep 6; shot frame-actuate-fetch-ok      # 200 OK body (network reachable)
        # --- brightness: an unambiguous A/B on the Settings stepper -------------
        echo "==> [actuate 1] Home, open drawer, launch Settings"
        tap "$NAV_HOME_X" "$NAV_HOME_Y"; sleep 2
        drag "$SWIPE_X" 690 110            # open the app drawer
        sleep 4
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 5; shot frame-actuate-settings
        # NB: space the stepper taps ~1.6s apart — under TCG libinput drops taps
        # that arrive faster than it can process ("system too slow"), so rapid
        # 0.5s taps mostly no-op. 1.6s gaps land every step reliably.
        echo "==> [actuate 1] Brightness UP to 5 -> NO dim (A); pip widest"
        for i in 1 2 3 4 5; do tap "$BRIGHT_INC_X" "$BRIGHT_ROW_Y"; sleep 1.6; done
        shot frame-actuate-bright5               # brightness 5: undimmed (compare B)
        echo "==> [actuate 1] Brightness DOWN to 1 -> screen DIMS (B); pip min"
        for i in 1 2 3 4; do tap "$BRIGHT_DEC_X" "$BRIGHT_ROW_Y"; sleep 1.6; done
        shot frame-actuate-dim                   # brightness 1: zelto-dim scrim, pip min
        # --- airplane ON -> bar dot + the network gate --------------------------
        echo "==> [actuate 1] Airplane ON -> bar airplane dot; Wi-Fi greys"
        tap "$AIR_X" "$AIR_Y"
        sleep 3; shot frame-actuate-airplane
        echo "==> [actuate 1] Home -> Fetch again; GET FAILS (airplane gate, no modal)"
        tap "$NAV_HOME_X" "$NAV_HOME_Y"; sleep 2
        launchtap "$FETCH_FAV_X" "$FAV_Y"
        sleep 5
        tap "$FETCH_BTN_X" "$FETCH_BTN_Y"
        sleep 4; shot frame-actuate-fetch-fail   # "request failed", NO consent modal
        echo "==> [actuate 1] sync + shutdown (brightness=1, airplane=1 persisted)"
        sleep 3
        actuate_kill

        echo "==> [actuate boot 2/2] REBOOT same disk; dim + airplane persisted"
        actuate_boot
        sleep "$SHOT_DELAY"
        shot frame-actuate-reboot                # home dimmed at b=1 + bar airplane dot
        echo "==> [actuate 2] open drawer, launch Settings -> Brightness 1, Airplane On"
        drag "$SWIPE_X" 690 110
        sleep 4
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 5; shot frame-actuate-reboot-settings
        actuate_kill
        [ -n "$NET_SRV_PID" ] && kill "$NET_SRV_PID" 2>/dev/null || true
        echo "==> actuate test done; frames in $OUT/frame-actuate-*.png"
        exit 0
    fi

    # P20 idle/lock lifecycle (LOCK=1): prove the phone's idle -> dim -> lock ->
    # off -> wake -> unlock state machine, driven by ext-idle-notify + the
    # sys.lock_* broker settings, with the lock screen truly blocking the app
    # beneath and the foreground app exactly restored on unlock. A TWO-BOOT test
    # against the same data.img:
    #   Boot #1 — home (lock disabled by default, so nothing idles: the other
    #     phase harnesses are undisturbed). Open the drawer, launch Settings. In
    #     the Lock screen section flip "Lock screen" ON (the bar grows a padlock
    #     glyph — the 4th brokered indicator) and step "Lock after" once (a non-
    #     default timeout to prove persistence). Now leave Settings foreground and
    #     STOP input: after the timeout the screen dims then the lock screen takes
    #     over (clock + date) OVER Settings, blocking it. Inject a tap -> it wakes
    #     to the LOCK screen (NOT Settings — the app stays blocked). Swipe up ->
    #     unlock -> Settings is exactly the foreground app again. Then flip "Lock
    #     screen" OFF and idle again -> it no longer locks (Settings stays up).
    #     Sync + kill (lock_enabled=0 + the stepped timeout persisted to /var/zelto).
    #   Boot #2 — FRESH QEMU, SAME disk: launch Settings -> Lock screen reads Off
    #     and "Lock after" shows the stepped value (both loaded back from
    #     /var/zelto; zelto-lock's serial log prints the same config). The
    #     persistence proof.
    # The idle timeouts are wide (dim 8s / lock 20s / off 120s) so the LOCKED
    # window is large regardless of the TCG guest-clock skew; after the idle wait a
    # tap normalises OFF-or-LOCKED to LOCKED before the screenshot, so the lock UI
    # is captured whatever the clock did. Coordinates are overridable (rerun
    # SKIP_BUILD=1 + overrides to retune from a captured frame). Boot is slow under
    # TCG: keep SHOT_DELAY high. Use launchtap (zero-hold) for drawer launches.
    if [ "${LOCK:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        SWIPE_X="${SWIPE_X:-640}"
        # Settings drawer tile: alphabetical 4-col grid, row 2 col 3 on a fresh
        # image. Retune from frame-lock-drawer.
        SETTINGS_X="${SETTINGS_X:-786}"; SETTINGS_Y="${SETTINGS_Y:-317}"
        # Settings Lock-screen section (measured off frame-lock-settings; the app
        # is a vertical Scroll, these are the initial unscrolled positions):
        #   the "Lock screen" On/Off chip (right column, like the other toggles),
        #   and the "Lock after" stepper "+" button + its row y.
        LOCK_TOGGLE_X="${LOCK_TOGGLE_X:-890}"; LOCK_TOGGLE_Y="${LOCK_TOGGLE_Y:-496}"
        LOCK_INC_X="${LOCK_INC_X:-897}"; LOCK_ROW_Y="${LOCK_ROW_Y:-628}"
        # A tap to wake the screen (normalise OFF->LOCKED); anywhere on the lock.
        WAKE_X="${WAKE_X:-640}"; WAKE_Y="${WAKE_Y:-420}"
        # How long to leave the device idle (wall seconds) so it locks under any
        # clock scale (lock=20 guest-s; even at ~1x, 30s wall > 20).
        IDLE_WAIT="${IDLE_WAIT:-30}"
        ax() { echo $(( $1 * 32767 / OUTW )); }
        ay() { echo $(( $1 * 32767 / OUTH )); }

        have_socat=0
        command -v socat >/dev/null 2>&1 && have_socat=1
        [ "$have_socat" = "1" ] || echo "WARN: socat not installed; cannot drive QMP"
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
        launchtap() { move "$1" "$2"; sleep 0.3; btn down; btn up; }
        drag() {
            local x="$1" y1="$2" y2="$3"
            move "$x" "$y1"; sleep 0.2; btn down; sleep 0.3
            move "$x" $(( (y1*2 + y2) / 3 )); sleep 0.3
            move "$x" $(( (y1 + y2*2) / 3 )); sleep 0.3
            move "$x" "$y2"; sleep 0.4; btn up
        }
        # Swipe UP on the lock screen to unlock: press low, drag well past the top
        # so translation_y clears the unlock threshold (-150px), release.
        swipe_up() {
            move "$SWIPE_X" 620; sleep 0.2; btn down; sleep 0.3
            move "$SWIPE_X" 460; sleep 0.3
            move "$SWIPE_X" 300; sleep 0.3
            move "$SWIPE_X" 160; sleep 0.4; btn up
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        lock_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        lock_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [lock boot 1/2] home (lock disabled by default)"
        lock_boot
        sleep "$SHOT_DELAY"
        shot frame-lock-home
        echo "==> [lock 1] open drawer + launch Settings"
        drag "$SWIPE_X" 690 110
        sleep 4; shot frame-lock-drawer          # read the Settings tile y off this
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 8; shot frame-lock-settings         # Lock screen section (read coords)
        echo "==> [lock 1] enable Lock screen -> bar padlock glyph appears"
        tap "$LOCK_TOGGLE_X" "$LOCK_TOGGLE_Y"
        sleep 2; shot frame-lock-enabled          # toggle On + padlock in the bar
        echo "==> [lock 1] step 'Lock after' once (non-default -> persistence)"
        tap "$LOCK_INC_X" "$LOCK_ROW_Y"
        sleep 2; shot frame-lock-config           # "Lock after" bumped
        echo "==> [lock 1] leave Settings foreground + idle ${IDLE_WAIT}s -> locks"
        sleep "$IDLE_WAIT"
        shot frame-lock-idle                      # dimmed/locked/off (clock-dependent)
        echo "==> [lock 1] tap -> WAKES to the lock screen (app stays blocked)"
        tap "$WAKE_X" "$WAKE_Y"
        sleep 2; shot frame-lock-locked           # lock clock over Settings (blocked)
        echo "==> [lock 1] swipe up -> UNLOCK -> Settings is foreground again"
        swipe_up
        # Settle generously: the unlock repaint is immediate but the TCG
        # screendump can lag a frame and still show the (stale) lock (same capture
        # artifact P17/P18 flagged); the disable tap below lands on the real
        # Settings toggle, proving the unlock took.
        sleep 5; shot frame-lock-unlocked         # Settings restored, no lock
        echo "==> [lock 1] disable Lock screen -> padlock gone"
        tap "$LOCK_TOGGLE_X" "$LOCK_TOGGLE_Y"
        sleep 2; shot frame-lock-disabled
        echo "==> [lock 1] idle again ${IDLE_WAIT}s -> no lock (disabled)"
        sleep "$IDLE_WAIT"
        shot frame-lock-noidle                    # Settings still up, never locked
        echo "==> [lock 1] sync + shutdown (lock_enabled=0 + timeout persisted)"
        sleep 3
        lock_kill

        echo "==> [lock boot 2/2] REBOOT same disk; settings persisted"
        lock_boot
        sleep "$SHOT_DELAY"
        shot frame-lock-reboot                    # home, no lock (disabled persisted)
        echo "==> [lock 2] open drawer + launch Settings -> Off + stepped timeout"
        drag "$SWIPE_X" 690 110
        sleep 4
        launchtap "$SETTINGS_X" "$SETTINGS_Y"
        sleep 8; shot frame-lock-reboot-settings  # Lock Off + "Lock after" persisted
        lock_kill
        echo "==> lock test done; frames in $OUT/frame-lock-*.png"
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

    # Optional: drive the P12 networking flow over QMP and capture a sequence
    # proving HTTP fetch through the network-permission consent end-to-end:
    #   - start a tiny HTTP server on the HOST (python3 -m http.server) serving a
    #     known file; under QEMU user-mode networking the guest reaches it at
    #     http://10.0.2.2:8080/ (fully offline, deterministic);
    #   - launch the "Fetch" app (it declares `permissions=network`);
    #   - tap "Fetch" -> z_net_send -> no stored grant, so zsysd shows the P8
    #     consent modal;
    #   - tap Allow -> the request connects, the callback fires on the app loop,
    #     and the fetched body renders in the app;
    #   - tap "Fetch" again -> cached grant returns immediately, NO dialog, the
    #     body refreshes.
    # The non-blocking connect/send/recv state machine lives in the app loop, so
    # the render loop never stalls. Coordinates are overridable so they can be
    # retuned to the rendered layout from a captured frame without a rebuild
    # (rerun with SKIP_BUILD=1 + overrides). Boot is slow under TCG: SHOT_DELAY
    # high; give the fetch generous time too.
    if [ "${NET:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        TILE_X="${TILE_X:-300}"               # x over a launcher tile
        FETCH_TILE_Y="${FETCH_TILE_Y:-688}"   # "Fetch" tile centre (retune!)
        FETCH_BTN_X="${FETCH_BTN_X:-640}"     # "Fetch" button in the app
        FETCH_BTN_Y="${FETCH_BTN_Y:-392}"
        ALLOW_X="${ALLOW_X:-894}"             # consent dialog's Allow
        ALLOW_Y="${ALLOW_Y:-527}"
        NET_PORT="${NET_PORT:-8080}"
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

        # --- host HTTP endpoint (reachable from the guest at 10.0.2.2) --------
        NET_SRV_PID=""
        if command -v python3 >/dev/null 2>&1; then
            SERVE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zelto-www.XXXXXX")"
            printf 'Hello from the Zelto host! (P12 networking works)\n' \
                > "$SERVE_DIR/hello.txt"
            echo "==> [net] starting host HTTP server on :$NET_PORT ($SERVE_DIR)"
            ( cd "$SERVE_DIR" && python3 -m http.server "$NET_PORT" ) \
                >/dev/null 2>&1 &
            NET_SRV_PID=$!
            sleep 1
        else
            echo "WARN: python3 not found; cannot start the host HTTP endpoint"
        fi

        echo "==> [net 0/5] shell: launcher with the Fetch tile"
        shot frame-net-launcher              # read the Fetch tile y off this

        echo "==> [net 1/5] tap 'Fetch' tile -> launch the networking demo app"
        tap "$TILE_X" "$FETCH_TILE_Y"
        sleep 4; shot frame-net-app          # Fetch app before sending (button)

        echo "==> [net 2/5] tap 'Fetch' -> z_net_send -> consent modal"
        tap "$FETCH_BTN_X" "$FETCH_BTN_Y"
        sleep 4; shot frame-net-consent      # overlay modal: Allow / Deny

        echo "==> [net 3/5] tap Allow -> request completes, body renders"
        tap "$ALLOW_X" "$ALLOW_Y"
        sleep 6; shot frame-net-fetched      # fetched body shown in the app

        echo "==> [net 4/5] tap 'Fetch' again -> cached grant, NO modal"
        tap "$FETCH_BTN_X" "$FETCH_BTN_Y"
        sleep 6; shot frame-net-cached       # body refreshes, no consent overlay

        echo "==> [net 5/5] done; stopping host HTTP server"
        [ -n "$NET_SRV_PID" ] && kill "$NET_SRV_PID" 2>/dev/null || true
    fi

    kill "$QPID" 2>/dev/null || true
else
    echo "==> launching QEMU with GTK display (WSLg)"
    exec qemu-system-aarch64 "${common[@]}" \
        -append "$KCMD" \
        -display gtk,gl=off \
        -serial mon:stdio
fi
