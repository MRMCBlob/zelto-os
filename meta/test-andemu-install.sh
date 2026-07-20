#!/usr/bin/env bash
# test-andemu-install.sh — end-to-end install test for a runtime=andemu package.
#
# The installer synthesises a script app's exec= rather than trusting the manifest
# (system/installer/main.c: a package must not choose which interpreter runs it).
# For runtime=andemu it must additionally add `--android`, so the runtime advertises
# the Android compat layer to the guest. That synthesis path
# (`m.runtime == "andemu"` -> " --android") had no automated coverage; this proves
# it, host-side, the way the QEMU INSTALL harness proves the native path — but
# without a boot, because the whole thing under test is what zelto-install writes.
#
# It reuses the two simulator retarget hooks the installer already exposes
# (ZELTO_TRUSTED_KEY, ZELTO_SCRIPT_BIN — the same idiom as run-sim.sh) so it runs on
# the host x86_64 build with no image. See docs/packaging/manifest.md,
# docs/zelto-script/android-compat.md, and memory zelto-os-p13-status (the .zap /
# install convention).
#
#   meta/test-andemu-install.sh          # build, package, install, assert
#   SKIP_BUILD=1 meta/test-andemu-install.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build-host}"

APP_ID="os.zelto.andemu.demo"
MANIFEST="$REPO_ROOT/samples/andemu-demo/zelto-andemu.app"
ENTRY="$REPO_ROOT/samples/andemu-demo/andemu-demo.js"
KEY="$REPO_ROOT/meta/keys/zelto-dev.pem"
TRUSTED="$REPO_ROOT/meta/keys/trusted.pub"

fail() { echo "FAIL: $*" >&2; exit 1; }
for tool in openssl zip sha256sum unzip; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool not found (needed to package/install)"
done
[ -f "$KEY" ] && [ -f "$TRUSTED" ] || fail "signing keys missing (run meta/keys/gen-keys.sh)"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "==> building host"
    ninja -C "$BUILD" >/dev/null
fi
INSTALLER="$BUILD/system/installer/zelto-install"
SCRIPT_BIN="$BUILD/script/zelto-script"
[ -x "$INSTALLER" ] || fail "zelto-install not built at $INSTALLER (libsodium-dev missing?)"
[ -x "$SCRIPT_BIN" ] || fail "zelto-script not built at $SCRIPT_BIN"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/zelto-andemu-install.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
ZAP="$WORK/andemu.zap"
DATA="$WORK/data"
mkdir -p "$DATA"

# --- package the andemu app as a signed script .zap ------------------------
echo "==> packaging $APP_ID"
bash "$HERE/mkzap.sh" "$MANIFEST" "$ENTRY" "$ZAP" "$KEY" >/dev/null
# Sanity: the package must carry runtime=andemu (mkzap preserves it) and a script=
# payload (mkzap synthesises it), and NOT an author-declared exec=.
unzip -p "$ZAP" zelto.toml > "$WORK/zelto.toml"
grep -q '^runtime=andemu$' "$WORK/zelto.toml" || fail "packaged manifest lost runtime=andemu"
grep -q '^script=script/andemu-demo.js$' "$WORK/zelto.toml" || fail "packaged manifest has no script= payload"
grep -q '^exec=' "$WORK/zelto.toml" && fail "packaged manifest must not carry a declared exec="

# --- install it (simulator retargets: no device image) ---------------------
echo "==> installing into $DATA"
ZELTO_DATA_DIR="$DATA" \
ZELTO_TRUSTED_KEY="$TRUSTED" \
ZELTO_SCRIPT_BIN="$SCRIPT_BIN" \
    "$INSTALLER" "$ZAP" || fail "zelto-install rejected a valid package (exit $?)"

# --- assert the synthesised runtime manifest -------------------------------
RT="$DATA/apps/manifests/$APP_ID.app"
[ -f "$RT" ] || fail "no runtime manifest written at $RT"
EXEC_LINE="$(sed -n 's/^exec=//p' "$RT")"
[ -n "$EXEC_LINE" ] || fail "runtime manifest has no exec="
echo "    exec= $EXEC_LINE"

# The headline: an andemu package's synthesised exec MUST carry --android.
case "$EXEC_LINE" in
    *" --android "*) : ;;
    *) fail "synthesised exec= is missing --android: $EXEC_LINE" ;;
esac
# And it must be the shared script runtime running the INSTALLED .js under this id —
# not anything the package chose.
case "$EXEC_LINE" in
    "$SCRIPT_BIN"*) : ;;
    *) fail "exec= is not the named script runtime: $EXEC_LINE" ;;
esac
grep -q -- "--id $APP_ID" <<<"$EXEC_LINE" || fail "exec= does not pin --id $APP_ID"
INSTALLED_JS="$DATA/installed/$APP_ID/andemu-demo.js"
[ -f "$INSTALLED_JS" ] || fail "the .js payload was not installed at $INSTALLED_JS"
grep -q "$INSTALLED_JS" <<<"$EXEC_LINE" || fail "exec= does not point at the installed .js"
# Note: runtime=andemu is an INPUT — the installer consumes it into the exec
# synthesis (the --android above) and does not re-emit it. Nothing downstream reads
# runtime= after install (the launcher/zsysd act on exec=), so the synthesised
# --android IS the preserved intent. We assert that, not the input field's echo.

# --- tamper rejection (mirrors the INSTALL harness's bad-package check) -----
# A byte flipped in the .js after signing must be caught at the per-file hash check:
# code is signature-covered whether it is an ELF or a script.
echo "==> tamper: a corrupted package must be rejected"
BADZAP="$WORK/andemu-bad.zap"
DATA2="$WORK/data-bad"
mkdir -p "$DATA2"
TAMPER=1 bash "$HERE/mkzap.sh" "$MANIFEST" "$ENTRY" "$BADZAP" "$KEY" >/dev/null
if ZELTO_DATA_DIR="$DATA2" ZELTO_TRUSTED_KEY="$TRUSTED" ZELTO_SCRIPT_BIN="$SCRIPT_BIN" \
        "$INSTALLER" "$BADZAP" >/dev/null 2>&1; then
    fail "installer ACCEPTED a tampered package"
fi
[ -f "$DATA2/apps/manifests/$APP_ID.app" ] && fail "tampered package left a registered manifest"

echo "PASS: runtime=andemu installs and its synthesised exec= carries --android; tampered package rejected"
