# Zelto test harness

`test/` holds Zelto's unit/integration tests and a runner that builds the project,
runs every test, and reports per-test pass/fail with the failing assertion's
`file:line`, expected-vs-actual, and (for C tests) a BUILD-FAIL / RUN-FAIL split.

It runs in the WSL Ubuntu build env (gcc + ninja); see memory `zelto-os-build-env`.

```
bash test/run-tests.sh                 # build, then run all tests
bash test/run-tests.sh --list          # list available tests, don't run
bash test/run-tests.sh test_sdk_sensor_rate   # run one named test
bash test/run-tests.sh -k 'test_android*'      # run tests matching a glob
bash test/run-tests.sh --filter '*prefs*'
bash test/run-tests.sh --no-build ...  # skip the ninja build (libs must exist)
bash test/run-tests.sh --help
```

The runner exits non-zero if any test fails (CI-friendly). It builds `build-host`
once up front so C tests link the freshly built libs, then compiles and runs each
C test and executes each shell test. Working files go to a `mktemp` dir, never the
repo.

## What a test is

**One file per function/behaviour under test.** Name it `test_<module>_<thing>`:

| Kind  | File                              | How it runs                                 |
|-------|-----------------------------------|---------------------------------------------|
| C     | `test_<module>_<fn>.c`            | compiled, then executed                     |
| shell | `test_<module>_<fn>.sh`           | executed with `bash`                        |

Support code lives in `test/framework/` and is **never** discovered as a test (the
runner only globs `test/test_*.{c,sh}`).

A test signals its result **by exit code**: `0` = pass, non-zero = fail. On a
failure it also emits one machine-parseable line per failed assertion that the
runner parses and reproduces in the report:

```
ZT_FAIL|<file>|<line>|<message>|<expected>|<actual>
```

Both the C framework (`framework/ztest.h`) and the shell framework
(`framework/ztest.sh`) emit exactly this format, so the runner handles both the
same way.

## C tests

Include `framework/ztest.h`, assert, and `return zt_result();`. `ASSERT_*` aborts
the test on first failure; `EXPECT_*` records and keeps going.

Available macros (all record `file:line` + expected/actual):

```
ASSERT_TRUE(c)   EXPECT_TRUE(c)     ASSERT_FALSE(c)   EXPECT_FALSE(c)
ASSERT_EQ_INT(e,a)   EXPECT_EQ_INT(e,a)
ASSERT_NEAR(e,a,eps) EXPECT_NEAR(e,a,eps)      // absolute float tolerance
ASSERT_STR_EQ(e,a)   EXPECT_STR_EQ(e,a)
```

Every C test is compiled from the repo root with a single uniform recipe:

```
gcc -std=c17 -D_GNU_SOURCE -I test -I third_party/quickjs \
    -I sdk/include -I sdk/src \
    test/<name>.c build-host/third_party/quickjs/libquickjs.a -lm -lpthread
```

so a test may freely `#include "zelto/ui.h"` (SDK public headers) and/or
`framework/qjs_harness.h` (QuickJS). It does **not** link libzelto — anything that
needs a live compositor/socket is integration-tested via `meta/run-sim.sh`
instead; a unit test should exercise the pure part and say so in a comment.

### Testing guest JS (script/js, script/android)

`framework/qjs_harness.h` runs the **real** shipped `.js` under a bare QuickJS
context, stubbing only the `zelto/*` imports a unit test cannot provide (sensor
socket, prefs store, consent broker). The module under test is registered from its
on-disk source; the driver imports it, does the work, and stashes results on
`globalThis`, which the C side reads back and asserts.

```c
zt_qjs_add_module("zelto/sensors", SENSORS_STUB);          // in-memory stub
zt_qjs_add_file_module("android/compat", "script/android/compat.js");  // real
zt_qjs_add_file_module("android/hardware", "script/android/hardware.js");
JSContext *ctx = zt_qjs_new();
ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);      // DRIVER is an ES-module string
EXPECT_EQ_INT(2, (int)zt_qjs_global_num(ctx, "__stops_after"));
```

Read-back helpers: `zt_qjs_global_bool`, `zt_qjs_global_num`,
`zt_qjs_global_arr(ctx,name,i)`, `zt_qjs_global_str(ctx,name,buf,n)`.

### New C test — copy-paste template

```c
// test_mymod_myfn — one sentence: what invariant this pins.
#include "framework/ztest.h"

int main(void) {
    ASSERT_EQ_INT(42, my_pure_fn(40, 2));
    EXPECT_TRUE(my_pure_fn(0, 0) == 0);
    return zt_result();
}
```

Drop it in `test/`, done — no registration, the runner discovers it by name.

## Shell tests

Source `framework/ztest.sh`, assert, and end with `zt_done`.

```
zt_expect_eq <expected> <actual> [msg]   # record on mismatch, keep going
zt_assert_eq <expected> <actual> [msg]   # record on mismatch, then abort
zt_ok      <cmd...>                       # cmd must succeed
zt_not_ok  <cmd...>                       # cmd must fail (e.g. "installer rejects")
zt_fail    <msg> [expected] [actual]      # record a failure directly
zt_done                                   # exit 0 iff nothing failed
```

### New shell test — copy-paste template

```bash
#!/usr/bin/env bash
# test_mymod_myfn — one sentence.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/framework/ztest.sh"

OUT="$(some-command)"
zt_expect_eq "expected" "$OUT" "some-command output"
zt_ok test -f /some/artifact
zt_done
```

A shell test that drives a longer scenario can shell out to an existing script and
translate its result — see `test_andemu_install.sh`, which runs
`meta/test-andemu-install.sh` (package → install → assert → tamper-reject) and maps
its pass/fail into the harness format.

## Seeded tests

| Test                              | Exercises                                                         |
|-----------------------------------|------------------------------------------------------------------|
| `test_sdk_sensor_rate.c`          | SDK public sensor-rate contract in `<zelto/ui.h>` (bounds/presets/enum count) |
| `test_android_getRotationMatrix.c`| `android/hardware.js` orientation math vs an Android reference    |
| `test_android_hardware_listener.c`| `android/hardware.js` multi-sensor listener teardown (no stream leak) |
| `test_android_prefs_prefix.c`     | `android/prefs.js` SharedPreferences key namespacing              |
| `test_android_compat_stub.c`      | `android/compat.js` graceful-stub "don't crash" contract         |
| `test_andemu_install.sh`          | end-to-end signed `.zap` install of a `runtime=andemu` app        |
