#!/usr/bin/env bash
# mkzap.sh — build a signed .zap package (the host-side packager).
#
# A .zap is the distributable unit of a Zelto app: a ZIP archive holding the
# manifest, the native binary (per ABI), optional assets, a content-hash
# manifest, and a detached signature. See docs/packaging/zap-format.md +
# docs/packaging/signing.md. This is the dev stand-in for `zelto build` — a real
# TOML manifest + a Script bundle are Planned; for the MVP a pure-C app ships the
# existing key=value manifest (stored as zelto.toml) and a single aarch64 ELF.
#
# Layout produced:
#     <app>.zap
#     ├── zelto.toml              # the manifest (key=value; exec= -> native path)
#     ├── native/aarch64/<bin>    # the cross-built ELF
#     ├── assets/icon.png         # optional
#     ├── MANIFEST.sha256         # "<sha256>  <path>" per packaged file
#     └── SIGNATURE               # raw Ed25519 signature over MANIFEST.sha256
#
# Usage:
#   meta/mkzap.sh <manifest.app> <binary> <out.zap> [key.pem] [icon.png]
#
# Env:
#   TAMPER=1   after signing, flip a byte in the packaged binary WITHOUT updating
#              MANIFEST.sha256 — produces a package whose signature is valid over
#              the manifest but whose file hash no longer matches, so the installer
#              rejects it at the per-file hash check. (A wrong-key package — signing
#              with a different key than the bundled trusted root — is the other
#              rejection path; this one exercises the hash check.)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

MANIFEST_SRC="${1:?usage: mkzap.sh <manifest.app> <binary> <out.zap> [key.pem] [icon.png]}"
BIN_SRC="${2:?missing <binary>}"
OUT_ZAP="${3:?missing <out.zap>}"
KEY="${4:-$REPO_ROOT/meta/keys/zelto-dev.pem}"
ICON="${5:-}"

for tool in openssl zip sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool not found"; exit 1; }
done
[ -f "$MANIFEST_SRC" ] || { echo "ERROR: manifest not found: $MANIFEST_SRC"; exit 1; }
[ -f "$BIN_SRC" ]      || { echo "ERROR: binary not found: $BIN_SRC"; exit 1; }
[ -f "$KEY" ]          || { echo "ERROR: signing key not found: $KEY (run meta/keys/gen-keys.sh)"; exit 1; }

# Resolve OUT_ZAP to an absolute path: `zip` runs after `cd "$STAGE"`, so a
# relative out path would otherwise land inside the staging dir.
mkdir -p "$(dirname "$OUT_ZAP")"
OUT_ZAP="$(cd "$(dirname "$OUT_ZAP")" && pwd)/$(basename "$OUT_ZAP")"

BIN_NAME="$(basename "$BIN_SRC")"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/zelto-mkzap.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/native/aarch64"
cp "$BIN_SRC" "$STAGE/native/aarch64/$BIN_NAME"

# zelto.toml = the manifest, with exec= rewritten to the in-package native path so
# the installer can find the binary (it then rewrites exec= again to the installed
# absolute path). Drop any pre-existing exec= line from the source manifest.
{
    grep -v '^exec=' "$MANIFEST_SRC" || true
    echo "exec=native/aarch64/$BIN_NAME"
} > "$STAGE/zelto.toml"

if [ -n "$ICON" ] && [ -f "$ICON" ]; then
    mkdir -p "$STAGE/assets"
    cp "$ICON" "$STAGE/assets/icon.png"
fi

# MANIFEST.sha256: a deterministic, sorted list of every packaged file's hash.
# Paths are relative (./native/...). The installer recomputes each and compares.
( cd "$STAGE" \
    && find . -type f ! -name MANIFEST.sha256 ! -name SIGNATURE | LC_ALL=C sort \
    | xargs sha256sum > MANIFEST.sha256 )

# SIGNATURE: a detached raw Ed25519 signature over the BYTES of MANIFEST.sha256.
# openssl's Ed25519 raw signature (64 bytes) is exactly what libsodium's
# crypto_sign_verify_detached() expects, and the raw 32-byte pubkey (trusted.pub)
# is the loader's key — no format conversion in the guest.
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$STAGE/MANIFEST.sha256" \
    -out "$STAGE/SIGNATURE"

# Self-check: verify the signature host-side with the derived public key, so a
# broken package never ships. (Mirrors `zelto verify`.)
PUB_PEM="$STAGE/pub.pem"
openssl pkey -in "$KEY" -pubout -out "$PUB_PEM"
if openssl pkeyutl -verify -pubin -inkey "$PUB_PEM" -rawin \
        -in "$STAGE/MANIFEST.sha256" -sigfile "$STAGE/SIGNATURE" >/dev/null 2>&1; then
    echo "==> signature self-check OK"
else
    echo "ERROR: signature self-check FAILED"; exit 1
fi
rm -f "$PUB_PEM"

if [ "${TAMPER:-0}" = "1" ]; then
    # Corrupt one byte of the packaged binary AFTER signing: the manifest + sig
    # stay valid, but the binary's hash no longer matches its MANIFEST entry, so
    # the installer rejects the package at the per-file integrity check.
    echo "==> TAMPER: flipping a byte in native/aarch64/$BIN_NAME (hash will mismatch)"
    printf '\xff' | dd of="$STAGE/native/aarch64/$BIN_NAME" bs=1 seek=64 count=1 \
        conv=notrunc status=none
fi

mkdir -p "$(dirname "$OUT_ZAP")"
rm -f "$OUT_ZAP"
# -X drops extra file attributes for a reproducible archive; busybox unzip reads
# a standard deflate ZIP.
( cd "$STAGE" && zip -X -q -r "$OUT_ZAP" \
    zelto.toml native MANIFEST.sha256 SIGNATURE \
    $( [ -d assets ] && echo assets ) )

echo "==> wrote $OUT_ZAP ($(du -h "$OUT_ZAP" | cut -f1))"
