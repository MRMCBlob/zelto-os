#!/usr/bin/env bash
# Generate the Zelto dev signing keypair (run once; output committed to the repo).
#
# Ed25519. Three artefacts land next to this script:
#   - zelto-dev.pem  the PRIVATE key (PKCS#8 PEM). DEV ONLY — fine to commit for a
#                    dev OS; a real publisher key + a store are Planned.
#   - zelto-dev.pub.pem  the public key in PEM (used by host-side `openssl verify`
#                        in mkzap.sh's self-check).
#   - trusted.pub    the 32-byte RAW Ed25519 public key. This is the single trusted
#                    root bundled into the image at /usr/share/zelto/keys/trusted.pub;
#                    zelto-install feeds it straight to libsodium
#                    crypto_sign_verify_detached().
#
# The raw key is the last 32 bytes of the DER SubjectPublicKeyInfo (Ed25519 SPKI is
# a fixed 12-byte prefix + the 32-byte key).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PRIV="$HERE/zelto-dev.pem"
PUB_PEM="$HERE/zelto-dev.pub.pem"
PUB_RAW="$HERE/trusted.pub"

if [ -f "$PRIV" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "==> $PRIV already exists (FORCE=1 to regenerate); leaving it"
else
    echo "==> generating Ed25519 private key -> $PRIV"
    openssl genpkey -algorithm ed25519 -out "$PRIV"
fi

echo "==> deriving public key PEM -> $PUB_PEM"
openssl pkey -in "$PRIV" -pubout -out "$PUB_PEM"

echo "==> deriving raw 32-byte public key -> $PUB_RAW"
openssl pkey -in "$PRIV" -pubout -outform DER | tail -c 32 > "$PUB_RAW"

echo "==> raw pubkey is $(wc -c < "$PUB_RAW") bytes (expect 32)"
echo "==> done"
