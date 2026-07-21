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

MEM="${MEM:-8224}"
SMP="${SMP:-12}"
CPU="${CPU:-cortex-a76}"
SHOT_DELAY="${SHOT_DELAY:-16}"
# There is no aarch64 KVM on an x86 host, so QEMU emulates via TCG. Multi-threaded
# TCG (one host thread per vCPU) plus a larger translation-block cache is the
# biggest lever we have on the lag (the "[libinput] your system is too slow"
# warning is the guest clock outrunning emulation). Override with ACCEL= if a
# host ever offers KVM.
ACCEL="${ACCEL:-tcg,thread=multi,tb-size=2048}"

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
    #
    # !! STALE SINCE P40 (found while replacing HOME_TEST/QUICK in P42; not flagged
    # before). Two of the surfaces this drives are gone:
    #   - the BOTTOM NAV BAR (Back/Home/Recents at y~768) was system/nav, deleted
    #     in P40 stage 1. Navigation is the home-indicator gesture pill now
    #     (system/homebar), so every `tap "$…_X" "$NAV_Y"` below lands on the app.
    #   - the Recents card's red X close button went out in P40 stage 2; the App
    #     Switcher closes a card by flicking it UP, so step 5 cannot be tapped.
    # Steps 1-2 (launch an app from a tile) are still valid. Rewriting the rest
    # means driving gestures, which QMP can do (see the drag() helper that QUICK=1
    # used) but which needs a real TCG run to re-derive coordinates.
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
    # P42: home layout persistence (HOME_TEST=1). A TWO-BOOT test against the
    # same data.img proving the ONE thing only a real QEMU boot can prove — that
    # the home screen's arrangement survives a power cycle, written to and read
    # back from the ext4 /var/zelto, not a host temp dir.
    #
    # This replaces two harnesses that P40 had left driving deleted UI:
    #   - the old HOME_TEST=1 swiped up to open the APP DRAWER (deleted in P40
    #     stage 2 — its job is now the last page of the home carousel), tapped a
    #     BOTTOM NAV BAR (system/nav, deleted in P40 stage 1 — navigation is the
    #     home-indicator gesture pill now), and asserted on `home.favorites` (a
    #     prefs key superseded by the `home.layout` CSV in P27/P29, and read today
    #     only as a one-time migration). Three dead surfaces, not the one flagged.
    #   - QUICK=1 drove the same drawer, reached the curate menu by long-pressing
    #     a DRAWER icon, and pulled a UNIFIED quick-settings shade that P40 split
    #     into a Control Center (top right) and a Notification Center (top left).
    #     It is deleted outright rather than rewritten: every state it photographed
    #     is now covered deterministically and reproducibly by the shot catalogue
    #     (meta/shots.sh 05-09 rearrange, 11/11a CC/NC), which reaches them through
    #     env test-hooks instead of pixel coordinates. Re-deriving tap coordinates
    #     under TCG to re-photograph them here buys nothing and rots again on the
    #     next layout change.
    #
    # The lesson those two encode is why this one carries NO tap coordinates at
    # all. A harness whose correctness lives in hardcoded x/y is stale the moment
    # the layout moves, and — worse — it still runs, still writes PNGs, and still
    # exits 0, so the rot is invisible until someone reviews the frames by eye.
    # This one drives the launcher through the same env hooks the shot catalogue
    # uses, and ASSERTS on the serial log rather than on pixels, so it can fail.
    #
    #   Boot #1 — boot to home. The launcher finds no home.layout on a fresh disk,
    #     seeds the default arrangement and persists it (serial: "launcher: wrote
    #     home.layout: ..."). ZELTO_HOME_SEED_EXTRA appends a marker entry so boot
    #     #2 is checking for something this boot specifically chose. Sync + kill.
    #   Boot #2 — a FRESH QEMU on the SAME disk. The launcher must READ the CSV
    #     back rather than re-seed it (serial: "launcher: home.layout loaded"),
    #     and the CSV it reports must match boot #1's byte for byte.
    if [ "${HOME_TEST:-0}" = "1" ]; then
        SERIAL1="$OUT/home-boot1.log"
        SERIAL2="$OUT/home-boot2.log"
        mkdir -p "$OUT"

        # Boot once, capture the serial console to $1, and stop after $2 seconds.
        # No QMP and no input: this harness never touches the pointer.
        home_boot_capture() {
            local logfile="$1" secs="$2"
            rm -f "$logfile"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial "file:$logfile" &
            QPID=$!
            sleep "$secs"
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        # The home.layout CSV the launcher reports on a given boot, or "".
        layout_from() {
            sed -n 's/.*launcher: wrote home\.layout: //p;s/.*launcher: home\.layout read: //p' \
                "$1" 2>/dev/null | tail -1 | tr -d '\r'
        }

        echo "==> [home boot 1/2] fresh disk: launcher seeds + persists home.layout"
        home_boot_capture "$SERIAL1" "$((SHOT_DELAY + 8))"
        L1="$(layout_from "$SERIAL1")"
        echo "    boot 1 home.layout: ${L1:-<none>}"

        echo "==> [home boot 2/2] REBOOT the SAME disk: layout must be read back"
        home_boot_capture "$SERIAL2" "$((SHOT_DELAY + 8))"
        L2="$(layout_from "$SERIAL2")"
        echo "    boot 2 home.layout: ${L2:-<none>}"

        rc=0
        if [ -z "$L1" ]; then
            echo "!! FAIL: boot 1 never reported a home.layout (see $SERIAL1)"; rc=1
        elif [ -z "$L2" ]; then
            echo "!! FAIL: boot 2 never reported a home.layout (see $SERIAL2)"; rc=1
        elif [ "$L1" != "$L2" ]; then
            echo "!! FAIL: the arrangement changed across the reboot"
            echo "     boot 1: $L1"
            echo "     boot 2: $L2"; rc=1
        elif grep -q "launcher: wrote home.layout" "$SERIAL2"; then
            # Boot 2 must LOAD, not re-seed. If it wrote the CSV again it means it
            # found nothing on disk and happened to seed the same default — which
            # passes an equality check while proving the exact opposite.
            echo "!! FAIL: boot 2 re-SEEDED the layout instead of loading it —"
            echo "   nothing was actually read back from /var/zelto (see $SERIAL2)"; rc=1
        else
            echo "==> PASS: home.layout persisted across a reboot"
            echo "     $L2"
        fi
        echo "==> home test done; serial logs in $OUT/home-boot*.log"
        exit "$rc"
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
    #
    # !! STALE SINCE P40 STAGE 2 (found while replacing HOME_TEST/QUICK in P42; not
    # flagged before). Both halves drive deleted UI:
    #   - "open the app drawer (swipe up)" — the drawer is gone; the App Library is
    #     the last page of the home carousel, reached by paging sideways.
    #   - the unified shade split into a Control Center (pulled from the top RIGHT)
    #     and a Notification Center (top LEFT), latched at Z_PAN_BEGIN off
    #     `e->x < w/2`. A swipe down the CENTRE column (SWIPE_X=640 of 1280) is
    #     exactly the ambiguous case, and the "Wi-Fi chip" it then taps is a round
    #     toggle in a 3x2 grid at different coordinates.
    # The Wi-Fi-toggle-persists-across-reboot proof is the valuable part and is not
    # covered elsewhere; it needs the swipe re-aimed at the right half and the chip
    # coordinates re-derived from a real TCG frame.
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

    # P23 volume + battery (VOLUME=1): prove the two hardware indicators every
    # phone shows are SYSTEM SERVICES (zsysd power/audio duty), reflected by a
    # status-bar battery glyph + a transient volume-rocker overlay, driven by a
    # fake battery source + the compositor's media keys, and PERSISTED. A two-boot
    # test against the same data.img. (Selector named VOLUME; there is no $VOLUME
    # build var — the app path var is VOLAPP — so it can't clobber anything.)
    #   Boot #1 — the fake battery starts at 24% (zelto.batterystart) and drains
    #     fast (zelto.batterytick=700). Home: the bar shows a battery glyph. Wait
    #     for it to cross 20% -> zsysd posts a one-shot "Battery low" notification
    #     (the shade banner) + the glyph turns red + brightness is nudged down.
    #     Then press Volume Up x3 (QMP send-key volumeup -> the compositor's
    #     XF86AudioRaiseVolume keybind -> settings_set sys.volume) -> the centred
    #     volume HUD pops with a fuller bar; Volume Down once -> the HUD updates.
    #     Sync + kill (sys.volume + the drained sys.battery_pct persisted).
    #   Boot #2 — FRESH QEMU, SAME disk: zsysd loads sys.volume + sys.battery_pct
    #     from /var/zelto, so the bar's battery glyph is at the persisted (low)
    #     level at boot; press Volume Up -> the HUD shows the persisted volume+1
    #     (the setting survived). Coordinates overridable; TCG boot is slow so keep
    #     SHOT_DELAY high.
    if [ "${VOLUME:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        # Fake battery: start just above the 20% low threshold and drain fast so
        # the low-battery warning fires within the first SHOT_DELAY window.
        # Widen the HUD dwell (zelto.volumems -> ZELTO_VOLUME_MS) so the screendump
        # reliably catches the rocker after a media-key press.
        VOL_MS="${VOL_MS:-8000}"
        KCMD="$KCMD zelto.fakebattery=1 zelto.batterytick=700 zelto.batterystart=24 zelto.volumems=$VOL_MS"

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
        # A volume media key: QMP send-key with the qcode QEMU maps to the evdev
        # KEY_VOLUME{UP,DOWN} the guest xkb resolves to XF86AudioRaise/LowerVolume,
        # which the compositor's handle_chord turns into a settings_set.
        volkey() {
            qmp "{\"execute\":\"send-key\",\"arguments\":{\"keys\":[{\"type\":\"qcode\",\"data\":\"$1\"}]}}"
        }
        shot() {
            rm -f "$OUT/$1.ppm"
            qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$OUT/$1.ppm\"}}"
            sleep 1
            to_png "$OUT/$1.ppm" "$OUT/$1.png"
        }
        volume_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        volume_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [volume boot 1/2] home; bar battery glyph (~24%, draining)"
        volume_boot
        sleep "$SHOT_DELAY"
        shot frame-volume-home
        echo "==> [volume 1] wait for the drain below 20% -> low-battery banner + red glyph"
        sleep 8
        shot frame-volume-low
        echo "==> [volume 1] Volume Up x3 (media key -> compositor -> sys.volume)"
        volkey volumeup; sleep 0.4
        volkey volumeup; sleep 0.4
        volkey volumeup; sleep 0.6
        shot frame-volume-up                     # centred rocker HUD, fuller bar
        echo "==> [volume 1] Volume Down x1 -> HUD updates"
        volkey volumedown; sleep 0.6
        shot frame-volume-down
        echo "==> [volume 1] sync + shutdown (sys.volume + battery_pct persisted)"
        sleep 3
        volume_kill

        echo "==> [volume boot 2/2] REBOOT same disk; battery + volume persisted"
        volume_boot
        sleep "$SHOT_DELAY"
        shot frame-volume-reboot                 # battery glyph at the persisted low level
        echo "==> [volume 2] Volume Up -> HUD shows the persisted volume+1"
        volkey volumeup; sleep 0.6
        shot frame-volume-reboot-up
        volume_kill
        echo "==> volume test done; frames in $OUT/frame-volume-*.png"
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

    # ---------------------------------------------------------------------
    # P21 on-screen keyboard (KBD=1): prove a soft QWERTY keyboard auto-appears
    # when a text field is focused, types into whatever app owns the field via the
    # text-input-v3 <-> input-method-v2 handshake, and hides again — then that the
    # typed text persists (it is saved to /var/zelto and survives a reboot). A
    # TWO-BOOT test against the same data.img (reuses the P11 persistence rig):
    #   Boot #1 — home; open the drawer (swipe up) and launch Notepad. Its note
    #     TextField shows a placeholder, no keyboard. TAP THE FIELD -> the QWERTY
    #     keyboard slides up from the bottom (a real exclusive zone shrinks Notepad
    #     so the field stays visible above it) and the field shows a caret. Inject
    #     taps on several letter keys -> the characters appear IN THE FIELD (in
    #     Notepad, which knows nothing about the keyboard). Tap backspace -> the
    #     last char is removed. Tap "Add note" -> the typed note is written to the
    #     SQLite DB on /var/zelto and listed. Tap the keyboard's hide key -> it
    #     slides away and the field keeps its (now cleared) state. Sync + kill.
    #   Boot #2 — FRESH QEMU, SAME disk: relaunch Notepad -> the persisted counter
    #     and the typed note row are read back from /var/zelto (survived the
    #     reboot). Also confirms the keyboard does NOT show on the home screen (no
    #     focused field) — frame-kbd-home has no keyboard.
    # Coordinates are overridable to retune to the rendered layout from a captured
    # frame (rerun SKIP_BUILD=1 + overrides — first guesses miss). Boot is slow
    # under TCG: keep SHOT_DELAY high; TCG drops rapid taps so keys are spaced, and
    # the screendump lags a frame so trust the downstream state.
    if [ "${KBD:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        SWIPE_X="${SWIPE_X:-640}"
        # Notepad drawer tile: alphabetical 4-col grid, row 1 col 3 on a fresh
        # image (Cards, Fetch, Notepad, Notes). Retune from frame-kbd-drawer.
        NOTEPAD_X="${NOTEPAD_X:-786}"; NOTEPAD_Y="${NOTEPAD_Y:-165}"
        # Notepad's note field + the "Add note" button (retune from frame-kbd-app).
        FIELD_X="${FIELD_X:-640}"; FIELD_Y="${FIELD_Y:-372}"
        ADD_X="${ADD_X:-640}"; ADD_Y="${ADD_Y:-452}"
        # Keyboard keys @1280x800. The keyboard sits ABOVE the 64px nav bar, so the
        # KBD_H=300 strip lands at y~436..736 and the four settled row centres are
        # ~row1 488, row2 561, row3 634, row4 707. Letters typed: h (row2), i (row1).
        H_X="${H_X:-751}"; H_Y="${H_Y:-561}"
        I_X="${I_X:-915}"; I_Y="${I_Y:-488}"
        BKSP_X="${BKSP_X:-1177}"; BKSP_Y="${BKSP_Y:-634}"   # row3 rightmost (<x)
        HIDE_X="${HIDE_X:-1183}"; HIDE_Y="${HIDE_Y:-707}"   # row4 rightmost (v)
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
        tap() { move "$1" "$2"; sleep 0.3; btn down; sleep 0.15; btn up; }
        # Launch tap: press then release as two separate input-send-events over ONE
        # socat connection, so the down->up gap is ~microseconds of wall time. Two
        # separate qmp() calls each spawn socat (~100ms+ apart), and the TCG guest
        # clock jumps ahead unpredictably in that gap — long enough to trip the
        # drawer icons' long-press (curate menu) or to be dropped. One connection
        # with two distinct commands is a reliable tap (distinct timestamps dodge
        # debounce; near-zero gap dodges long-press).
        launchtap() {
            move "$1" "$2"; sleep 0.3
            [ "$have_socat" = "1" ] || return 0
            printf '%s\n' \
                '{"execute":"qmp_capabilities"}' \
                '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}' \
                '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}' \
                | socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
        }
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
        kbd_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        kbd_kill() {
            sync
            kill "$QPID" 2>/dev/null || true
            wait "$QPID" 2>/dev/null || true
        }

        echo "==> [kbd boot 1/2] home (no keyboard; no focused field)"
        kbd_boot
        sleep "$SHOT_DELAY"
        shot frame-kbd-home
        echo "==> [kbd 1] open drawer + launch Notepad"
        drag "$SWIPE_X" 690 110
        # Let the drawer slide-up fully settle: a launch tap on a mid-slide tile
        # misses (tile still moving) and simply does nothing.
        sleep 8; shot frame-kbd-drawer            # read the Notepad tile y off this
        launchtap "$NOTEPAD_X" "$NOTEPAD_Y"
        sleep 6; shot frame-kbd-app               # Notepad: field + Add note, no kbd
        echo "==> [kbd 1] tap the note field -> keyboard slides up"
        tap "$FIELD_X" "$FIELD_Y"
        # Let the slide-up spring fully settle before typing: under TCG it takes
        # several wall seconds, and key taps that land mid-slide hit above the keys.
        sleep 18; shot frame-kbd-shown            # QWERTY fully up; field has a caret
        echo "==> [kbd 1] type 'h' then 'i' -> chars land in the field"
        tap "$H_X" "$H_Y"
        sleep 2
        tap "$I_X" "$I_Y"
        sleep 3; shot frame-kbd-typed             # field shows "hi" (allow for shot lag)
        echo "==> [kbd 1] backspace -> one char removed"
        tap "$BKSP_X" "$BKSP_Y"
        sleep 3; shot frame-kbd-backspace         # field shows "h"
        echo "==> [kbd 1] tap hide -> keyboard slides away, field keeps its text"
        tap "$HIDE_X" "$HIDE_Y"
        # Let the slide-DOWN fully complete so Notepad relayouts to full height and
        # the Add-note button returns to its keyboard-down position before we tap it.
        sleep 7; shot frame-kbd-hidden            # keyboard gone, Notepad full; "h" kept
        echo "==> [kbd 1] Add note -> the typed note is written to /var/zelto"
        tap "$ADD_X" "$ADD_Y"
        sleep 2; shot frame-kbd-added             # note row "#1: h" listed; field cleared
        echo "==> [kbd 1] sync + shutdown"
        sleep 3
        kbd_kill

        echo "==> [kbd boot 2/2] REBOOT same disk; typed note + counter persisted"
        kbd_boot
        sleep "$SHOT_DELAY"
        shot frame-kbd-reboot                      # home, no keyboard
        echo "==> [kbd 2] open drawer + relaunch Notepad -> note row survived"
        drag "$SWIPE_X" 690 110
        sleep 8
        launchtap "$NOTEPAD_X" "$NOTEPAD_Y"
        sleep 6; shot frame-kbd-persisted          # loaded count=N + the typed row
        kbd_kill
        echo "==> kbd test done; frames in $OUT/frame-kbd-*.png"
        exit 0
    fi

    # P22 system clipboard + text selection (CLIP=1): prove select/copy/paste moves
    # text within a field, between two fields in one app, and ACROSS a process
    # boundary. A SINGLE boot (the clipboard is runtime, not persisted). The
    # selector is named CLIP (never a $CLIP build var), so it can't clobber a
    # build-initramfs binary the way LOCK/KBD did (the P20/P21 trap).
    #   1. Home; open the drawer and launch Notepad. Tap the note field -> the
    #      keyboard slides up. Type "hi".
    #   2. Long-press the word in the note field -> it selects (highlight + drag
    #      handles) and the Copy/Cut/Paste/Select-all action bar appears. Tap Copy
    #      -> the text is placed on the CLIPBOARD selection (core wl_data_device).
    #   3. Tap the SECOND (title) field to move the caret there, then tap the
    #      keyboard's Paste key -> "hi" appears in the title field (in-app move; the
    #      keyboard read the clipboard through wlr-data-control and committed it via
    #      input-method).
    #   4. Home -> drawer -> launch Notes (a SEPARATE process). Tap its field, tap
    #      the keyboard Paste key -> "hi" appears -> the clipboard crossed the
    #      process boundary via wl_data_device, neither app knowing about the other.
    # Coordinates overridable to retune from a captured frame (rerun SKIP_BUILD=1).
    # TCG is slow + drops rapid taps: high SHOT_DELAY, spaced taps, launch as a
    # batched down+up, and the screendump lags a frame (trust downstream state).
    if [ "${CLIP:-0}" = "1" ]; then
        OUTW="${OUTW:-1280}"; OUTH="${OUTH:-800}"
        # No launcher-tile taps: the two demo apps are auto-launched at boot (init,
        # gated by zelto.clipdemo=1 which we append to the cmdline below), so the
        # test only ever taps TEXT FIELDS (safe — a mis-fired long-press on an empty
        # field just focuses it) and the Switch chord (a deterministic key event).
        # Notepad is frontmost at boot; one Tab (Switch) brings Notes forward.
        KCMD="$KCMD zelto.clipdemo=1"
        # Notepad's two fields, KEYBOARD-DOWN full layout (retune from frame-clip-app):
        # note ~364, title ~421, "Add note" ~480.
        NOTE_X="${NOTE_X:-640}"; NOTE_Y="${NOTE_Y:-364}"
        TITLE_X="${TITLE_X:-640}"; TITLE_Y="${TITLE_Y:-421}"
        # Content is TOP-anchored + stable keyboard up/down. Notepad: note ~222,
        # title ~277, action bar ~348. Notes: field ~222.
        NFIELD_X="${NFIELD_X:-640}"; NFIELD_Y="${NFIELD_Y:-222}"
        # The word to long-press: the note field is pre-filled "hello world"; aim
        # over "hello" (left side). Retune from frame-clip-app.
        SEL_X="${SEL_X:-500}"; SEL_Y="${SEL_Y:-222}"
        # Title field (paste target for the in-app move). Retune from frame-clip-app.
        TITLE_UP_Y="${TITLE_UP_Y:-277}"
        # Action bar buttons — pinned at the top of the focused app, so their
        # positions are fixed regardless of the keyboard's (per-boot-variable) row
        # layout. Copy/Cut/Paste/Select-all sit at x~66/136/207/301, y~95. We paste
        # through the bar (not the keyboard's Paste key) precisely because the bar
        # is keyboard-independent. Retune from frame-clip-selected.
        # The inline bar slot, KEYBOARD UP, sits at y~190 (below the shade strip).
        # Buttons (left-aligned): Copy ~62, Cut ~132, Paste ~203, Select all ~297.
        COPY_X="${COPY_X:-62}"; COPY_Y="${COPY_Y:-348}"
        BPASTE_X="${BPASTE_X:-203}"; BPASTE_Y="${BPASTE_Y:-348}"
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
        tap() { move "$1" "$2"; sleep 0.3; btn down; sleep 0.15; btn up; }
        # A press held past the SDK long-press threshold (0.45s GUEST clock) with no
        # motion -> selects the word. Hold long in WALL time: under TCG the guest
        # clock lags real time, so 0.45 guest-seconds can take several wall seconds.
        longpress() { move "$1" "$2"; sleep 0.5; btn down; sleep 3.0; btn up; }
        # Launch tap: down+up as two commands over ONE socat connection (near-zero
        # wall gap dodges the tile long-press; distinct timestamps dodge the drop).
        # Warm up first: a move + a 3s settle lets the lagging TCG guest clock catch
        # up BEFORE the press, so the guest-time gap between down and up stays tiny
        # (the first input after a long idle is the worst case for a spurious long-
        # press — the guest clock jumps ~600ms all at once on that first event).
        launchtap() {
            move "$1" "$2"; sleep 3
            [ "$have_socat" = "1" ] || return 0
            printf '%s\n' \
                '{"execute":"qmp_capabilities"}' \
                '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}' \
                '{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}' \
                | socat - "UNIX-CONNECT:$QMP_SOCK" >/dev/null 2>&1 || true
        }
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
        clip_boot() {
            QMP_SOCK="$(mktemp -u "${STAGE:-${TMPDIR:-/tmp}}/zelto-qmp.XXXXXX.sock")"
            rm -f "$QMP_SOCK"
            qemu-system-aarch64 "${common[@]}" \
                -append "$KCMD" \
                -display none \
                -serial mon:stdio \
                -qmp "unix:$QMP_SOCK,server,nowait" &
            QPID=$!
        }
        clip_kill() { sync; kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true; }

        echo "==> [clip boot] Notepad + Notes auto-launched (Notepad in front)"
        clip_boot
        sleep "$SHOT_DELAY"
        shot frame-clip-app                   # Notepad: note pre-filled "hello world"
        echo "==> [clip] long-press 'hello' -> select the word + raise keyboard + bar"
        longpress "$SEL_X" "$SEL_Y"
        sleep 16; shot frame-clip-selected    # "hello" highlighted + handles + Copy bar
        echo "==> [clip] tap Copy -> 'hello' on the clipboard (core wl_data_device)"
        tap "$COPY_X" "$COPY_Y"
        sleep 3; shot frame-clip-copied       # selection kept; bar still up
        echo "==> [clip] focus title field + bar Paste -> in-app move"
        tap "$TITLE_X" "$TITLE_UP_Y"
        sleep 4
        tap "$BPASTE_X" "$BPASTE_Y"
        sleep 3; shot frame-clip-pasted       # title field now shows "hello" (in-app move)
        echo "==> [clip] Switch chord (Tab x2: Notepad->launcher->Notes)"
        # zcomp_switch focuses the LRU tail, so from Notepad the first Tab lands on
        # the launcher and the second on Notes (a SEPARATE process).
        for _ in 1 2; do
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":true,\"key\":{\"type\":\"qcode\",\"data\":\"tab\"}}}]}}"
            qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"key\",\"data\":{\"down\":false,\"key\":{\"type\":\"qcode\",\"data\":\"tab\"}}}]}}"
            sleep 2
        done
        sleep 3; shot frame-clip-notes-app    # Notes: paste field, no keyboard
        echo "==> [clip] tap Notes field, then bar Paste -> CROSS-APP paste"
        tap "$NFIELD_X" "$NFIELD_Y"
        sleep 6
        tap "$BPASTE_X" "$BPASTE_Y"
        sleep 3; shot frame-clip-crossapp     # Notes field shows "hello" (crossed procs)
        sleep 8                               # let the guest serial drain before kill
        clip_kill
        echo "==> clip test done; frames in $OUT/frame-clip-*.png"
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
