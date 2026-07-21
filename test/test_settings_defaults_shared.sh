#!/usr/bin/env bash
# test_settings_defaults_shared — a brokered setting read by more than one
# surface must take its fallback from system/common/settings_defaults.h, never
# from a literal at the call site.
#
# WHY THIS IS A TEST AND NOT A CODE REVIEW NOTE. zsysd has no defaults table: a
# key that has never been written is simply absent from the store, and each
# client supplies its own fallback to z_setting_get_int(). sys.brightness is read
# by three separate surfaces — the Settings app, the status bar and the dim
# overlay — and every one of them carried its own literal `3`. That is not a
# tidiness problem: those three literals ARE the boot-time behaviour of an
# unconfigured device, they have to agree, they are in three files that are never
# opened together, and nothing anywhere fails when they drift. The symptom of
# drift is a phone that boots at one brightness and dims itself the first time
# any other surface writes the key.
#
# So: assert the literals are gone. This is a lint, and it is cheap, and it is
# the only mechanism that will notice the fourth surface someone adds next year.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
. "$HERE/framework/ztest.sh"

DEFAULTS="$REPO_ROOT/system/common/settings_defaults.h"

if [ ! -f "$DEFAULTS" ]; then
    zt_fail "system/common/settings_defaults.h is missing" "present" "absent"
    zt_done
fi

# The shared header must actually define what it claims to.
for macro in ZELTO_DEFAULT_BRIGHTNESS ZELTO_DEFAULT_VOLUME; do
    if ! grep -q "define $macro" "$DEFAULTS"; then
        zt_fail "settings_defaults.h does not define $macro" "defined" "missing"
    fi
done

# The value the user asked for, pinned so a future edit is a deliberate one: a
# fresh device runs at FULL brightness. Anything below 5 makes zelto-dim paint a
# real black scrim over every pixel below the status bar on first boot.
got_brightness="$(sed -n 's/^#define ZELTO_DEFAULT_BRIGHTNESS[[:space:]]*\([0-9]*\).*/\1/p' \
    "$DEFAULTS" | head -1)"
zt_expect_eq "5" "$got_brightness" "default sys.brightness should be full (5 of 5)"

# No surface may hard-code a fallback for a shared key. Matches a numeric literal
# as the second argument to z_setting_get_int for one of these keys; the correct
# form passes the ZELTO_DEFAULT_* macro instead, which does not match.
shared_keys='sys\.brightness|sys\.volume'
offenders="$(grep -rnE "z_setting_get_int\(\"($shared_keys)\"[[:space:]]*,[[:space:]]*[0-9]" \
    "$REPO_ROOT/system" 2>/dev/null || true)"

if [ -n "$offenders" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "  hard-coded default: $line" >&2
    done <<< "$offenders"
    n="$(printf '%s\n' "$offenders" | grep -c .)"
    zt_fail "a shared setting's default is hard-coded at the call site" \
            "0 sites" "$n site(s)"
fi

# And the surfaces that read sys.brightness must be reading the shared macro, so
# the lint above cannot be satisfied by simply deleting the call.
for f in system/apps/settings/main.c system/bar/main.c system/dim/main.c; do
    if ! grep -q "ZELTO_DEFAULT_BRIGHTNESS" "$REPO_ROOT/$f"; then
        zt_fail "$f no longer uses the shared brightness default" \
                "ZELTO_DEFAULT_BRIGHTNESS" "not found"
    fi
done

zt_done
