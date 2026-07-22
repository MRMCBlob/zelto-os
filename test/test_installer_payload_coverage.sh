#!/usr/bin/env bash
# test_installer_payload_coverage — zelto-install must refuse a package whose
# executed payload is NOT enumerated by MANIFEST.sha256, even when the package is
# validly signed over that (doctored) manifest.
#
# Why this matters: verify_hashes() only proves every file the manifest LISTS
# matches its recorded digest, and the signature only covers the MANIFEST bytes.
# A .zap that ships a payload MANIFEST never names would pass both checks while
# the bytes actually parsed/executed went unattested — an attacker who can edit a
# package (but not forge the trust-root key) could swap in an unlisted payload.
# path_in_manifest() closes that gap by asserting the payload (and zelto.toml) are
# hash-covered before install. This test forges exactly that omitted-payload .zap
# and asserts the installer rejects it and registers nothing.
#
# It re-signs with the repo's dev key and points the installer at the matching dev
# trust root via ZELTO_TRUSTED_KEY (the run-sim.sh idiom), so no device image is
# needed. SKIP-worthy only if the signing keys / built installer are absent.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

BUILD="$REPO_ROOT/build-host"
KEY="$REPO_ROOT/meta/keys/zelto-dev.pem"
TRUSTED="$REPO_ROOT/meta/keys/trusted.pub"
INSTALL="$BUILD/system/installer/zelto-install"
SCRIPT_BIN="$BUILD/script/zelto-script"
ENTRY="$REPO_ROOT/samples/andemu-demo/andemu-demo.js"
MANIFEST_SRC="$REPO_ROOT/samples/andemu-demo/zelto-andemu.app"

# Prerequisites. The runner is exit-code driven (0 = pass); it has no separate
# skip state, so on a machine that lacks the signing keys / built installer we
# exit 0 with an explanatory note rather than failing a test the environment
# simply can't run. In this repo the keys and build products are always present.
for f in "$KEY" "$TRUSTED" "$INSTALL" "$ENTRY" "$MANIFEST_SRC"; do
    if [ ! -e "$f" ]; then
        echo "note: skipping — missing prerequisite: $f" >&2
        exit 0
    fi
done
for t in openssl zip sha256sum; do
    command -v "$t" >/dev/null 2>&1 || { echo "note: skipping — missing tool: $t" >&2; exit 0; }
done

W="$(mktemp -d "${TMPDIR:-/tmp}/zelto-payloadcov.XXXXXX")"
trap 'rm -rf "$W"' EXIT
STAGE="$W/stage"
mkdir -p "$STAGE/script"

# Stage a normal-looking script package, then compute a full, honest MANIFEST.
cp "$ENTRY" "$STAGE/script/andemu-demo.js"
{ grep -vE '^(exec|script)=' "$MANIFEST_SRC" || true; echo "script=script/andemu-demo.js"; } \
    > "$STAGE/zelto.toml"
( cd "$STAGE" \
    && find . -type f ! -name MANIFEST.sha256 ! -name SIGNATURE | LC_ALL=C sort \
    | xargs sha256sum > MANIFEST.sha256 )

# THE ATTACK: drop the payload's line from MANIFEST so it is no longer covered,
# then re-sign the doctored MANIFEST with the real dev key. Signature verify and
# the per-file hash loop will both PASS — only the coverage assertion should fire.
grep -v 'script/andemu-demo.js' "$STAGE/MANIFEST.sha256" > "$STAGE/M2"
mv "$STAGE/M2" "$STAGE/MANIFEST.sha256"
if grep -q 'script/andemu-demo.js' "$STAGE/MANIFEST.sha256"; then
    zt_fail "payload line was not removed from MANIFEST" "absent" "present"
    zt_done
fi
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$STAGE/MANIFEST.sha256" \
    -out "$STAGE/SIGNATURE" 2>/dev/null

ZAP="$W/evil.zap"
( cd "$STAGE" && zip -X -q -r "$ZAP" zelto.toml MANIFEST.sha256 SIGNATURE script )

DATA="$W/data"
# The installer MUST reject this package (non-zero exit).
zt_not_ok env ZELTO_DATA_DIR="$DATA" ZELTO_TRUSTED_KEY="$TRUSTED" \
    ZELTO_SCRIPT_BIN="$SCRIPT_BIN" "$INSTALL" "$ZAP"

# ...and it must have registered nothing (no runtime manifest left behind).
REG="$DATA/apps/manifests/os.zelto.andemu.demo.app"
if [ -e "$REG" ]; then
    zt_fail "rejected package left a registered manifest" "no manifest" "$REG exists"
fi

zt_done
