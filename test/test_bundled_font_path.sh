#!/usr/bin/env bash
# test_bundled_font_path — the image installs the font the runtime opens.
#
# WHY THIS EXISTS. libzelto opens ONE face at startup and disables text entirely
# if it cannot. The path came from two places that had no reason to be edited
# together: Z_DEFAULT_FONT in sdk/src/app.c (what a client opens) and the `cp`
# in meta/initramfs/build-initramfs.sh (what the image ships) — plus, until P45,
# a third: an `export ZELTO_FONT=...` in meta/initramfs/init that sat in FRONT of
# both.
#
# P30 (71d4908) switched the bundled face from ZeltoSans.ttf to
# Satoshi-Variable.ttf in the build script and left the init export pointing at
# the old name. For FIFTEEN PHASES every libzelto process on the real target
# opened a file that did not exist and QEMU rendered not one glyph. It survived
# that long for the same reason P43's type scale did — it was consistently wrong
# for everybody, so nothing looked misaligned, it just looked EMPTY — and because
# every phase from P31 on was verified in the simulator, which sets its own
# ZELTO_FONT and so was never affected. The only signal was eight lines of
# "could not open font" in a 60,000-line serial log.
#
# So this is a lint, not a review note: the failure is silent, cross-file, and
# invisible in the medium (a screenshot) that the phases after it were checked in.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

APP_C="$REPO_ROOT/sdk/src/app.c"
BUILD_SH="$REPO_ROOT/meta/initramfs/build-initramfs.sh"
INIT="$REPO_ROOT/meta/initramfs/init"

for f in "$APP_C" "$BUILD_SH" "$INIT"; do
    if [ ! -f "$f" ]; then
        zt_fail "missing $f" "present" "absent"
        zt_done
    fi
done

# --- 1. What the runtime opens ---------------------------------------------
default_font="$(sed -n 's/^#define Z_DEFAULT_FONT[[:space:]]*"\(.*\)".*/\1/p' \
    "$APP_C" | head -1)"
if [ -z "$default_font" ]; then
    zt_fail "sdk/src/app.c no longer defines Z_DEFAULT_FONT as a string literal" \
            "a quoted path" "not found"
    zt_done
fi

# --- 2. What the image installs --------------------------------------------
# The destination of the `cp "$FONT_SRC" ...` in the initramfs build.
dest="$(grep -oE '\$ROOT/usr/share/zelto/fonts/[A-Za-z0-9._-]+' "$BUILD_SH" \
    | head -1 | sed 's|^\$ROOT||')"
if [ -z "$dest" ]; then
    zt_fail "build-initramfs.sh no longer copies a font under /usr/share/zelto/fonts" \
            "a cp destination" "not found"
    zt_done
fi

zt_expect_eq "$default_font" "$dest" \
    "the image installs the font Z_DEFAULT_FONT opens (this is THE P30 bug)"

# --- 3. And the source file it copies actually exists -----------------------
# A dangling FONT_SRC is silent too: the copy is guarded by `[ -f ... ]`, so a
# renamed asset produces an image with no font and no error from the build.
src="$(sed -n 's|^FONT_SRC="${FONT_SRC:-\$REPO_ROOT/\(.*\)}"|\1|p' "$BUILD_SH" | head -1)"
if [ -z "$src" ]; then
    zt_fail "build-initramfs.sh no longer has a default FONT_SRC" \
            "a default under \$REPO_ROOT" "not found"
elif [ ! -f "$REPO_ROOT/$src" ]; then
    zt_fail "FONT_SRC points at an asset that is not in the repo" \
            "$src exists" "missing"
fi

# The basename shipped must be the basename opened, or the cp renames it and
# check 2 passes while the guest still opens the wrong name.
zt_expect_eq "$(basename "$default_font")" "$(basename "$dest")" \
    "the installed filename is the one the runtime opens"

# --- 4. Nothing puts a STALE default in front of it ------------------------
# ZELTO_FONT is a legitimate override — the simulator sets it, and a device with
# a different face would. What is not legitimate is a hardcoded default baked
# into the guest image, because it shadows Z_DEFAULT_FONT and then only the
# override's copy of the path is ever exercised. That is exactly the P30 bug.
stale="$(grep -nE '^[[:space:]]*export[[:space:]]+ZELTO_FONT=' "$INIT" || true)"
if [ -n "$stale" ]; then
    echo "  $stale" >&2
    zt_fail "meta/initramfs/init exports a hardcoded ZELTO_FONT, shadowing Z_DEFAULT_FONT" \
            "no export (the compiled-in default is the contract)" "an export"
fi

# --- 5. The warning names the path -----------------------------------------
# The diagnostic that should have caught this in one boot instead of fifteen
# phases. An unactionable warning repeated once per client reads as boot noise.
if ! grep -A 6 'z_text_open(font_path)' "$APP_C" | grep -q "font_path"; then
    zt_fail "the could-not-open-font warning no longer reports which path failed" \
            "the attempted path in the message" "an anonymous warning"
fi

zt_done
