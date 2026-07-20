// test_permissions_coalesce — concurrent same-permission requests must collapse
// to ONE broker dialog (script/js/permissions.js).
//
// The consent broker (zsysd) allows only one permission dialog in flight; a
// second native permRequest for a permission already being asked is denied. The
// "request once, open many" shape hits this naturally — an app that opens the
// accelerometer AND the magnetometer to fuse orientation fires two concurrent
// `sensors` requests before either grant lands. permissions.request() coalesces
// them via an in-flight promise cache so only one native call is made and every
// caller shares its result; the cache is cleared on settle so a later, separate
// ask still prompts afresh.
//
// This drives the REAL permissions.js under QuickJS over a stub zelto:native whose
// permRequest counts how many times it is actually invoked. No compositor, no
// broker socket — just the coalescing logic, which is the whole fix.
#include "framework/qjs_harness.h"
#include "framework/ztest.h"

// A zelto:native stub modelling the broker: permStatus reports "prompt" (0) until
// a grant lands, and permRequest counts its invocations and resolves true on a
// microtask (the async human-in-the-loop grant). If the JS did NOT coalesce, the
// three concurrent asks below would each bump __perm_calls.
static const char *NATIVE_STUB =
    "globalThis.__perm_calls = 0;\n"
    "export const PERM_PROMPT = 0;\n"
    "export const PERM_GRANTED = 1;\n"
    "export const PERM_DENIED = 2;\n"
    "let __status = PERM_PROMPT;\n"
    "export function permStatus(name){ return __status; }\n"
    "export function permRequest(name){\n"
    "  globalThis.__perm_calls++;\n"
    "  return Promise.resolve().then(() => { __status = PERM_GRANTED; return true; });\n"
    "}\n";

static const char *DRIVER =
    "import { request } from 'zelto/permissions';\n"
    // Three concurrent asks for the same permission, all while undecided.
    "const a = request('sensors');\n"
    "const b = request('sensors');\n"
    "const c = request('sensors');\n"
    // Coalescing means the three callers share ONE promise object.
    "globalThis.__same_ab = (a === b) ? 1 : 0;\n"
    "globalThis.__same_bc = (b === c) ? 1 : 0;\n"
    "Promise.all([a, b, c]).then((rs) => {\n"
    "  globalThis.__all_granted = rs.every((x) => x === true) ? 1 : 0;\n"
    "  globalThis.__calls_after_first = globalThis.__perm_calls;\n"  // expect 1
    // A later, SEPARATE ask must reach the broker again (cache cleared on settle).
    "  return request('sensors');\n"
    "}).then((r2) => {\n"
    "  globalThis.__second_granted = (r2 === true) ? 1 : 0;\n"
    "  globalThis.__calls_after_second = globalThis.__perm_calls;\n"  // expect 2
    "});\n";

int main(void) {
    zt_qjs_add_module("zelto:native", NATIVE_STUB);
    zt_qjs_add_file_module("zelto/permissions", "script/js/permissions.js");

    JSContext *ctx = zt_qjs_new();
    ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);

    // The three concurrent callers received the same coalesced promise.
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__same_ab"));
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__same_bc"));
    // All three resolved to the grant.
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__all_granted"));
    // THE FIX: three concurrent asks made exactly ONE broker call.
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__calls_after_first"));
    // Coalescing does not leak: a later separate ask prompts afresh (call #2)...
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__second_granted"));
    EXPECT_EQ_INT(2, (int)zt_qjs_global_num(ctx, "__calls_after_second"));

    return zt_result();
}
