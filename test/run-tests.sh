#!/usr/bin/env bash
# run-tests.sh — the Zelto test harness runner.
#
# Discovers and runs every test in test/ (one file per function/behaviour under
# test), building the project first so C tests link the freshly built libs. C
# tests (test_*.c) are compiled against the SDK headers + the vendored QuickJS
# static lib and executed; shell tests (test_*.sh) are executed directly. Both
# signal pass/fail through their EXIT CODE and emit failures in the shared
# machine-parseable format the framework defines (ZT_FAIL|file|line|msg|exp|act);
# see test/README.md.
#
# Usage:
#   bash test/run-tests.sh                 # build, then run all tests
#   bash test/run-tests.sh --list          # list available tests, don't run
#   bash test/run-tests.sh NAME [NAME...]  # run only the named test(s)
#   bash test/run-tests.sh -k 'android*'   # run tests whose name matches a glob
#   bash test/run-tests.sh --filter '*prefs*'
#   bash test/run-tests.sh --no-build ...  # skip the ninja build (libs must exist)
#   bash test/run-tests.sh --help
#
# Runs inside the WSL Ubuntu build env (gcc/ninja); see memory zelto-os-build-env.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$REPO_ROOT/build-host}"
QUICKJS_LIB="$BUILD/third_party/quickjs/libquickjs.a"

# The thin QuickJS archive names its members relative to the repo root, and C
# tests read guest .js sources by relative path — so everything runs from here.
cd "$REPO_ROOT"

# --- ANSI (suppressed when not a tty) --------------------------------------
if [ -t 1 ]; then
    C_GRN=$'\033[32m'; C_RED=$'\033[31m'; C_YEL=$'\033[33m'
    C_DIM=$'\033[2m';  C_BLD=$'\033[1m';  C_RST=$'\033[0m'
else
    C_GRN=""; C_RED=""; C_YEL=""; C_DIM=""; C_BLD=""; C_RST=""
fi

# --- args ------------------------------------------------------------------
DO_BUILD=1
DO_LIST=0
declare -a FILTERS=()

usage() {
    sed -n '2,28p' "$HERE/run-tests.sh" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage 0 ;;
        --list|-l) DO_LIST=1; shift ;;
        --no-build) DO_BUILD=0; shift ;;
        --filter|-k)
            [ $# -ge 2 ] || { echo "error: $1 needs a glob" >&2; exit 2; }
            FILTERS+=("$2"); shift 2 ;;
        --*) echo "error: unknown option: $1" >&2; usage 2 ;;
        *) FILTERS+=("$1"); shift ;;
    esac
done

# --- discovery -------------------------------------------------------------
# One test = one file matching test/test_*.{c,sh}. framework/ is support code and
# is never a test (its files live in test/framework/, excluded by the glob).
declare -a ALL_TESTS=()
for f in "$HERE"/test_*.c "$HERE"/test_*.sh; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    ALL_TESTS+=("${base%.*}")
done
# De-dup (a test_x.c and test_x.sh would collide; we don't ship such a pair, but
# guard anyway) and sort.
IFS=$'\n' ALL_TESTS=($(printf '%s\n' "${ALL_TESTS[@]}" | LC_ALL=C sort -u)); unset IFS

name_matches() {
    local name="$1"
    [ ${#FILTERS[@]} -eq 0 ] && return 0
    local pat
    for pat in "${FILTERS[@]}"; do
        # shellcheck disable=SC2254
        case "$name" in $pat) return 0 ;; esac
    done
    return 1
}

declare -a SELECTED=()
for name in "${ALL_TESTS[@]}"; do
    name_matches "$name" && SELECTED+=("$name")
done

if [ "$DO_LIST" -eq 1 ]; then
    echo "${C_BLD}Available tests:${C_RST}"
    for name in "${ALL_TESTS[@]}"; do
        [ -f "$HERE/$name.c" ] && kind="C   " || kind="shell"
        printf '  %s  %s\n' "$kind" "$name"
    done
    exit 0
fi

if [ ${#SELECTED[@]} -eq 0 ]; then
    echo "${C_YEL}no tests matched${C_RST} ${FILTERS[*]:-}" >&2
    exit 1
fi

# --- build once ------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    echo "${C_DIM}==> ninja -C $BUILD${C_RST}"
    if ! ninja -C "$BUILD" >/tmp/zt-build.log 2>&1; then
        echo "${C_RED}BUILD FAILED${C_RST} (project did not compile; tests cannot link):"
        tail -30 /tmp/zt-build.log | sed 's/^/    /'
        exit 1
    fi
fi
if [ ! -f "$QUICKJS_LIB" ]; then
    echo "${C_YEL}warning:${C_RST} $QUICKJS_LIB missing — C tests that link QuickJS will BUILD-FAIL" >&2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/zelto-tests.XXXXXX")"
# Cleanup is explicit (an `rm -rf "$WORK"` right before the final `exit`) rather
# than an EXIT trap, so the pass/fail exit code is never entangled with a cleanup
# command's own status — the runner's exit code is authoritative for CI. The
# INT/TERM trap only covers an interrupted run so it still tidies up.
trap 'rm -rf "$WORK"; exit 130' INT TERM

CC="${CC:-gcc}"
CFLAGS=(-std=c17 -D_GNU_SOURCE -O0 -g
        -I "$REPO_ROOT/test" -I "$REPO_ROOT/third_party/quickjs"
        -I "$REPO_ROOT/sdk/include" -I "$REPO_ROOT/sdk/src")
LDLIBS=("$QUICKJS_LIB" -lm -lpthread)

# --- pretty-print captured failures ----------------------------------------
# Reproduce ZT_FAIL lines and any stderr the runner captured, indented.
print_failures() {
    local out="$1" err="$2"
    if grep -q '^ZT_FAIL|' "$out"; then
        echo "        ${C_BLD}assertions:${C_RST}"
        while IFS='|' read -r _ file line msg exp act; do
            printf '          %s:%s  %s\n' "$file" "$line" "$msg"
            printf '            expected: %s\n' "$exp"
            printf '            actual:   %s\n' "$act"
        done < <(grep '^ZT_FAIL|' "$out")
    fi
    if [ -s "$err" ]; then
        echo "        ${C_BLD}stderr:${C_RST}"
        sed 's/^/          /' "$err" | head -20
    fi
}

# --- run -------------------------------------------------------------------
PASS=0; FAIL=0
declare -a FAILED_NAMES=()

now_ms() { date +%s%3N; }

for name in "${SELECTED[@]}"; do
    printf '%s[ RUN  ]%s %s\n' "$C_DIM" "$C_RST" "$name"
    out="$WORK/$name.out"; err="$WORK/$name.err"
    t0="$(now_ms)"

    if [ -f "$HERE/$name.c" ]; then
        # ---- C test: compile (BUILD-FAIL) then run (RUN-FAIL) ----
        bin="$WORK/$name.bin"; buildlog="$WORK/$name.build"
        if ! "$CC" "${CFLAGS[@]}" "$HERE/$name.c" "${LDLIBS[@]}" -o "$bin" \
                >"$buildlog" 2>&1; then
            t1="$(now_ms)"
            printf '%s[ FAIL ]%s %s %s(BUILD-FAIL, %s ms)%s\n' \
                "$C_RED" "$C_RST" "$name" "$C_DIM" "$((t1 - t0))" "$C_RST"
            echo "        ${C_BLD}compiler:${C_RST}"
            sed 's/^/          /' "$buildlog" | head -30
            FAIL=$((FAIL + 1)); FAILED_NAMES+=("$name"); continue
        fi
        "$bin" >"$out" 2>"$err"; rc=$?
    else
        # ---- shell test ----
        bash "$HERE/$name.sh" >"$out" 2>"$err"; rc=$?
    fi

    t1="$(now_ms)"; dur="$((t1 - t0))"
    if [ "$rc" -eq 0 ]; then
        printf '%s[ PASS ]%s %s %s(%s ms)%s\n' \
            "$C_GRN" "$C_RST" "$name" "$C_DIM" "$dur" "$C_RST"
        PASS=$((PASS + 1))
    else
        printf '%s[ FAIL ]%s %s %s(RUN-FAIL, exit %s, %s ms)%s\n' \
            "$C_RED" "$C_RST" "$name" "$C_DIM" "$rc" "$dur" "$C_RST"
        print_failures "$out" "$err"
        FAIL=$((FAIL + 1)); FAILED_NAMES+=("$name")
    fi
done

# --- summary ---------------------------------------------------------------
SKIPPED=$(( ${#ALL_TESTS[@]} - ${#SELECTED[@]} ))
echo
echo "${C_BLD}────────────────────────────────────────${C_RST}"
if [ "$FAIL" -eq 0 ]; then
    printf '%s%d passed%s, %d failed, %d skipped\n' \
        "$C_GRN" "$PASS" "$C_RST" "$FAIL" "$SKIPPED"
else
    printf '%d passed, %s%d failed%s (%s), %d skipped\n' \
        "$PASS" "$C_RED" "$FAIL" "$C_RST" "${FAILED_NAMES[*]}" "$SKIPPED"
fi
rm -rf "$WORK"
if [ "$FAIL" -eq 0 ]; then exit 0; else exit 1; fi
