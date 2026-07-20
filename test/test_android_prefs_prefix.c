// test_android_prefs_prefix — SharedPreferences key namespacing (android/prefs.js).
//
// Zelto has one flat per-app key/value store; Android apps expect several named
// SharedPreferences "files". android/prefs.js bridges the two by turning a file
// name into a key prefix ("<name>.<key>"), with the default ("") file unprefixed.
// This drives the REAL prefs.js under QuickJS over a stub zelto/storage whose
// backing object we can inspect, and asserts the prefix discipline, the
// Editor put/apply/remove flush, and the typed getters — the behaviour an Android
// app's settings screen silently depends on.
#include "framework/qjs_harness.h"
#include "framework/ztest.h"

// A zelto/storage stub backed by globalThis.__store so the test can read exactly
// which keys the Editor wrote.
static const char *STORAGE_STUB =
    "globalThis.__store = {};\n"
    "export function get(key, fallback = null){\n"
    "  const v = globalThis.__store[key];\n"
    "  return v === undefined ? fallback : v; }\n"
    "export function set(key, value){ globalThis.__store[key] = String(value); }\n"
    "export function remove(key){ delete globalThis.__store[key]; }\n"
    "export function getNumber(key, fallback = 0){\n"
    "  const v = globalThis.__store[key];\n"
    "  if (v === undefined) return fallback;\n"
    "  const n = Number(v); return Number.isNaN(n) ? fallback : n; }\n";

static const char *DRIVER =
    "import { SharedPreferences } from 'android/prefs';\n"
    "const def = new SharedPreferences('');\n"
    "def.edit().putString('a', '1').apply();\n"
    "const game = new SharedPreferences('game');\n"
    "game.edit().putInt('score', 42).putBoolean('won', true).apply();\n"
    "globalThis.__default_a  = globalThis.__store['a'] ?? '<unset>';\n"
    "globalThis.__game_score = globalThis.__store['game.score'] ?? '<unset>';\n"
    "globalThis.__game_won   = globalThis.__store['game.won'] ?? '<unset>';\n"
    "globalThis.__no_cross   = ('game.a' in globalThis.__store) ? 1 : 0;\n"
    "globalThis.__read_int   = game.getInt('score', 0);\n"
    "globalThis.__read_bool  = game.getBoolean('won', false) ? 1 : 0;\n"
    "globalThis.__contains   = game.contains('score') ? 1 : 0;\n"
    "globalThis.__default_int= game.getInt('missing', 7);\n"
    "game.edit().remove('score').apply();\n"
    "globalThis.__after_remove = ('game.score' in globalThis.__store) ? 1 : 0;\n";

int main(void) {
    zt_qjs_add_module("zelto/storage", STORAGE_STUB);
    zt_qjs_add_file_module("android/prefs", "script/android/prefs.js");

    JSContext *ctx = zt_qjs_new();
    ASSERT_TRUE(zt_qjs_run(ctx, DRIVER) == 0);

    char buf[64];
    // Default file: key stored WITHOUT a prefix.
    EXPECT_STR_EQ("1", zt_qjs_global_str(ctx, "__default_a", buf, sizeof(buf)));
    // Named file: keys stored under "game.".
    EXPECT_STR_EQ("42", zt_qjs_global_str(ctx, "__game_score", buf, sizeof(buf)));
    EXPECT_STR_EQ("true", zt_qjs_global_str(ctx, "__game_won", buf, sizeof(buf)));
    // The default file's "a" must NOT bleed into the "game" namespace.
    EXPECT_EQ_INT(0, (int)zt_qjs_global_num(ctx, "__no_cross"));

    // Typed getters round-trip through the prefix.
    EXPECT_EQ_INT(42, (int)zt_qjs_global_num(ctx, "__read_int"));
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__read_bool"));
    EXPECT_EQ_INT(1, (int)zt_qjs_global_num(ctx, "__contains"));
    // A miss returns the caller's default.
    EXPECT_EQ_INT(7, (int)zt_qjs_global_num(ctx, "__default_int"));
    // remove() flushes: the prefixed key is gone.
    EXPECT_EQ_INT(0, (int)zt_qjs_global_num(ctx, "__after_remove"));

    return zt_result();
}
