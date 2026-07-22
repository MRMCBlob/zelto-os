// qjs_harness.h — run REAL guest JS (script/js/*, script/android/*) under a bare
// QuickJS context, so a C test can exercise the actual shipped module code the way
// the Zelto Script runtime would, but without a compositor, a socket, or the app
// loop.
//
// Why this exists: the andemu translation modules (android/hardware.js, prefs.js,
// compat.js, ...) are ES modules that `import` the zelto/* capability modules.
// Those capability modules reach the device (sensors socket, prefs store, consent
// broker), which a unit test has none of. So the harness serves each import from a
// small registry: the module under test is registered from its real on-disk
// source, and every capability it imports is registered as a hand-written STUB
// that records/returns in memory. The function bodies that actually run — the
// rotation math, the key prefixing, the graceful stub Proxy — are the real ones.
//
// It links only libquickjs.a (+ -lm -lpthread); no libzelto, no wayland. The test
// evaluates a small "main" module that drives the real code and stashes results on
// globalThis, then reads them back from C and asserts with ztest.h.
#ifndef ZT_QJS_HARNESS_H
#define ZT_QJS_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

// --- module registry -------------------------------------------------------
//
// One process runs one test, so a file-static registry is enough. Each entry is a
// bare module specifier ("android/hardware", "zelto/storage") and its source. The
// QuickJS module loader below resolves imports against it.

#define ZT_QJS_MAX_MODULES 32

typedef struct {
    char name[64];
    char *source;  // owned; freed at exit (process-scoped, so we leak on purpose)
} ZtQjsModule;

static ZtQjsModule zt_qjs_modules_[ZT_QJS_MAX_MODULES];
static int zt_qjs_module_count_ = 0;

// Register a module from an in-memory source string (a stub, or a generated test
// main). The string is copied.
static void zt_qjs_add_module(const char *name, const char *source) {
    if (zt_qjs_module_count_ >= ZT_QJS_MAX_MODULES) {
        fprintf(stderr, "qjs_harness: too many modules\n");
        exit(2);
    }
    ZtQjsModule *m = &zt_qjs_modules_[zt_qjs_module_count_++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->source = strdup(source);
    if (!m->source) {
        exit(2);
    }
}

// Register a module from a real source file on disk (the code under test). `path`
// is relative to the repo root, e.g. "script/android/hardware.js".
static void zt_qjs_add_file_module(const char *name, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "qjs_harness: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        exit(2);
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (zt_qjs_module_count_ >= ZT_QJS_MAX_MODULES) {
        exit(2);
    }
    ZtQjsModule *m = &zt_qjs_modules_[zt_qjs_module_count_++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->source = buf;
}

// --- module loader ---------------------------------------------------------

// Bare specifiers pass through unchanged (we resolve them from the registry, not
// the filesystem), so `import "android/compat"` stays "android/compat".
static char *zt_qjs_normalize_(JSContext *ctx, const char *base,
                               const char *name, void *opaque) {
    (void)base;
    (void)opaque;
    return js_strdup(ctx, name);
}

static JSModuleDef *zt_qjs_loader_(JSContext *ctx, const char *name,
                                   void *opaque) {
    (void)opaque;
    for (int i = 0; i < zt_qjs_module_count_; i++) {
        if (strcmp(zt_qjs_modules_[i].name, name) == 0) {
            const char *src = zt_qjs_modules_[i].source;
            JSValue fn = JS_Eval(ctx, src, strlen(src), name,
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(fn)) {
                return NULL;
            }
            JSModuleDef *m = JS_VALUE_GET_PTR(fn);
            JS_FreeValue(ctx, fn);
            return m;
        }
    }
    JS_ThrowReferenceError(ctx, "qjs_harness: unregistered module: %s", name);
    return NULL;
}

// --- console shim ----------------------------------------------------------
//
// compat.js / widget.js call console.warn/log on the graceful path. Give them a
// real console that writes to stderr, so a test's stdout carries only ZT_ lines.

static JSValue zt_qjs_console_(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        fprintf(stderr, "%s%s", i ? " " : "", s ? s : "?");
        if (s) {
            JS_FreeCString(ctx, s);
        }
    }
    fprintf(stderr, "\n");
    return JS_UNDEFINED;
}

// --- context ---------------------------------------------------------------

// A fresh runtime + context with the registry loader, a console, and a
// setTimeout/queueMicrotask good enough for module-top-level code (the andemu
// translations do not schedule during import, but os.js/handlers might).
static JSContext *zt_qjs_new(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetModuleLoaderFunc(rt, zt_qjs_normalize_, zt_qjs_loader_, NULL);

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JSValue cfn = JS_NewCFunction(ctx, zt_qjs_console_, "log", 1);
    JS_SetPropertyStr(ctx, console, "log", JS_DupValue(ctx, cfn));
    JS_SetPropertyStr(ctx, console, "info", JS_DupValue(ctx, cfn));
    JS_SetPropertyStr(ctx, console, "warn", JS_DupValue(ctx, cfn));
    JS_SetPropertyStr(ctx, console, "error", JS_DupValue(ctx, cfn));
    JS_SetPropertyStr(ctx, console, "debug", cfn);
    JS_SetPropertyStr(ctx, g, "console", console);
    JS_FreeValue(ctx, g);

    const char *boot =
        "globalThis.queueMicrotask = (fn) => { Promise.resolve().then(fn); };";
    JSValue r = JS_Eval(ctx, boot, strlen(boot), "<boot>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
    return ctx;
}

// Evaluate a driver module (which imports the code under test and stashes results
// on globalThis). Returns 0 on clean completion, -1 on a thrown exception (whose
// message is printed to stderr — that is itself a test failure the caller flags).
static int zt_qjs_run(JSContext *ctx, const char *source) {
    JSValue v = JS_Eval(ctx, source, strlen(source), "test-main",
                        JS_EVAL_TYPE_MODULE);
    // Drain the microtask queue so any promise the module created settles.
    JSContext *jc;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &jc) > 0) {
    }
    int rc = 0;
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "qjs_harness: uncaught: %s\n", msg ? msg : "?");
        if (msg) {
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, exc);
        rc = -1;
    }
    JS_FreeValue(ctx, v);
    return rc;
}

// --- reading results back from globalThis ----------------------------------

static JSValue zt_qjs_global_(JSContext *ctx, const char *name) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, name);
    JS_FreeValue(ctx, g);
    return v;  // caller frees
}

static int zt_qjs_global_bool(JSContext *ctx, const char *name) {
    JSValue v = zt_qjs_global_(ctx, name);
    int b = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return b;
}

static double zt_qjs_global_num(JSContext *ctx, const char *name) {
    JSValue v = zt_qjs_global_(ctx, name);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

// Read element i of a numeric array held in global `name`.
static double zt_qjs_global_arr(JSContext *ctx, const char *name, uint32_t i) {
    JSValue arr = zt_qjs_global_(ctx, name);
    JSValue el = JS_GetPropertyUint32(ctx, arr, i);
    double d = 0;
    JS_ToFloat64(ctx, &d, el);
    JS_FreeValue(ctx, el);
    JS_FreeValue(ctx, arr);
    return d;
}

// Copy a string global into `out` (truncating). Returns out.
static const char *zt_qjs_global_str(JSContext *ctx, const char *name, char *out,
                                     size_t n) {
    JSValue v = zt_qjs_global_(ctx, name);
    const char *s = JS_ToCString(ctx, v);
    snprintf(out, n, "%s", s ? s : "");
    if (s) {
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return out;
}

#endif  // ZT_QJS_HARNESS_H
