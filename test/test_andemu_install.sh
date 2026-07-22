#!/usr/bin/env bash
# test_andemu_install — participates the existing end-to-end install test in the
# harness. meta/test-andemu-install.sh packages the runtime=andemu app as a signed
# .zap, installs it host-side, and asserts the synthesised exec= carries --android
# and that a tampered package is rejected. Here we just run it and translate its
# pass/fail into the harness's machine-parseable format.
#
# The runner builds the project before any test, so we pass SKIP_BUILD=1 to avoid a
# redundant ninja invocation (REPO_ROOT/BUILD are resolved by the inner script).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

INNER="$REPO_ROOT/meta/test-andemu-install.sh"
if [ ! -f "$INNER" ]; then
    zt_fail "meta/test-andemu-install.sh not found" "present" "missing"
    zt_done
fi

# Run it, capturing output so a failure can be surfaced with detail.
OUT="$(SKIP_BUILD=1 bash "$INNER" 2>&1)"
RC=$?

if [ "$RC" -ne 0 ]; then
    # Show the inner script's tail so the report explains the failure.
    echo "$OUT" | tail -8 >&2
    zt_fail "meta/test-andemu-install.sh failed" "exit 0" "exit $RC"
    zt_done
fi

# Belt-and-braces: the inner script prints a definitive PASS line.
if ! grep -q '^PASS:' <<<"$OUT"; then
    zt_fail "install test did not print its PASS line" "PASS:" "$(echo "$OUT" | tail -1)"
fi

zt_done
