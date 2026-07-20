// test_android_compat_stub — the andemu "don't crash" contract (android/compat.js).
//
// compat.js.stub() is the safety net of the whole andemu route: when an emulated
// Android app reaches for a service Zelto does not map, getSystemService returns a
// stub Proxy so `getSomething(x).doThing()` runs harmlessly instead of throwing.
// The invariants that make that safe are subtle, so pin them against the REAL
// module under QuickJS: a method call returns the fallback (null by default), the
// stub is NOT thenable (or `await`/Promise chaining would try to drive it), it
// stringifies to a readable tag, and nothing it does ever throws.
#include "framework/qjs_harness.h"
#include "framework/ztest.h"

static const char *DRIVER =
    "import { stub } from 'android/compat';\n"
    "const s = stub('svc');\n"
    "globalThis.__call_null   = (s.anything() === null) ? 1 : 0;\n"
    "globalThis.__not_thenable= (s.then === undefined) ? 1 : 0;\n"
    "globalThis.__tostring    = String(s);\n"
    "globalThis.__fallback    = stub('svc2', 5).bar();\n"
    "let threw = 0;\n"
    "try { stub('x').whatever(); const t = `${stub('y')}`; } catch (e) { threw = 1; }\n"
    "globalThis.__no_throw    = threw;\n";

int main(void) {
    zt_qjs_add_file_module("android/compat", "script/android/compat.js");

    JSContext *ctx = zt_qjs_new();
    ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);

    // A called stub method yields the fallback (null default), never throws.
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__call_null"));
    // Must not look like a thenable, or a stray `await stub()` would hang/mangle.
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__not_thenable"));
    // Readable when coerced to a string.
    char buf[64];
    EXPECT_STR_EQ("[andemu stub svc]",
                  zt_qjs_global_str(ctx, "__tostring", buf, sizeof(buf)));
    // A custom fallback flows through.
    EXPECT_EQ_INT(5, (int)zt_qjs_global_num(ctx, "__fallback"));
    // The whole point: no path through the stub throws.
    EXPECT_EQ_INT(0, (int)zt_qjs_global_num(ctx, "__no_throw"));

    return zt_result();
}
