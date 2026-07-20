#!/usr/bin/env bash
# ztest.sh — shell-side assertion helpers for test/*.sh tests.
#
# Source this at the top of a shell test:  . "$(dirname "$0")/framework/ztest.sh"
# then use zt_expect_eq / zt_assert_eq / zt_ok / zt_fail, and end with `zt_done`.
#
# Failures print the SAME machine-parseable line the C framework (ztest.h) emits,
# so test/run-tests.sh parses both through one path:
#
#     ZT_FAIL|<file>|<line>|<message>|<expected>|<actual>
#
# and the exit code is authoritative: zt_done exits 0 iff nothing failed. As in C,
# zt_expect_* records and continues; zt_assert_* records and aborts the test.

ZT_FAILURES=0

# Emit one failure line. Args: message, expected, actual.
zt_fail() {
    local msg="$1" exp="${2:-}" act="${3:-}"
    # ${BASH_SOURCE[1]}:${BASH_LINENO[0]} is the caller's file:line.
    printf 'ZT_FAIL|%s|%s|%s|%s|%s\n' \
        "${BASH_SOURCE[1]:-$0}" "${BASH_LINENO[0]:-0}" "$msg" "$exp" "$act"
    ZT_FAILURES=$((ZT_FAILURES + 1))
}

# zt_expect_eq <expected> <actual> [message] — record on mismatch, keep going.
zt_expect_eq() {
    if [ "$1" != "$2" ]; then
        zt_fail "${3:-expect_eq}" "$1" "$2"
    fi
}

# zt_assert_eq <expected> <actual> [message] — record on mismatch, then abort.
zt_assert_eq() {
    if [ "$1" != "$2" ]; then
        zt_fail "${3:-assert_eq}" "$1" "$2"
        zt_done
    fi
}

# zt_ok <cmd...> — the command must succeed (exit 0), else fail + abort.
zt_ok() {
    if ! "$@"; then
        zt_fail "command failed: $*" "exit 0" "exit $?"
        zt_done
    fi
}

# zt_not_ok <cmd...> — the command must FAIL (non-zero), else fail + abort. Handy
# for "the installer must reject this".
zt_not_ok() {
    if "$@"; then
        zt_fail "command unexpectedly succeeded: $*" "non-zero exit" "exit 0"
        zt_done
    fi
}

# Finish: exit 0 if clean, 1 otherwise. Call at the end (and it is called for you
# by the assert_* aborts).
zt_done() {
    exit $([ "$ZT_FAILURES" -eq 0 ] && echo 0 || echo 1)
}
