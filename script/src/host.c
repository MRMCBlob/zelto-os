// The Zelto Script host: a QuickJS context wired to the libzelto app loop.
//
// Boot order (z_script_main):
//   1. runtime + context, native module ("zelto:native"), globals (console,
//      timers), module loader (built-in `zelto/*` + the app's own files),
//   2. evaluate the entry module, hand its default export to the core module,
//   3. z_app_main_id with a body() that calls into JS.
//
// The single-threaded model of docs/zelto-script/runtime.md falls out of this:
// body(), tap handlers, timers and promise jobs all run on the app loop, so
// script code never races the renderer — it just must not block.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal.h"
#include "quickjs.h"
#include "zelto/gfx.h"
#include "zelto/script.h"
#include "zelto/ui.h"

struct ZsTimer {
    int64_t id;
    int64_t due_ms;         // absolute deadline
    int32_t interval_ms;    // 0 = one-shot (setTimeout)
    JSValue fn;
    ZsTimer *next;          // sorted by due_ms, soonest first
};

// One script runtime per process — see internal.h.
static ZScript g_zs;

ZScript *zs_current(void) { return &g_zs; }

int64_t zs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- errors ----------------------------------------------------------------

void zs_dump_error(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, exc);
    fprintf(stderr, "zelto-script: %s\n", msg ? msg : "unknown error");
    if (msg) { JS_FreeCString(ctx, msg); }

    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack) && !JS_IsException(stack)) {
        const char *s = JS_ToCString(ctx, stack);
        if (s && *s) { fprintf(stderr, "%s", s); }
        if (s) { JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

void zs_run_jobs(ZScript *zs) {
    JSContext *job_ctx;
    for (;;) {
        int rc = JS_ExecutePendingJob(zs->rt, &job_ctx);
        if (rc == 0) { break; }             // queue drained
        if (rc < 0) { zs_dump_error(job_ctx); }
    }
}

// --- calling back into JS from the app loop --------------------------------

// Every C callback the bindings register (a tap, a pan, a settled fetch, a
// setting that changed) ends up here. The three steps are always the same, and
// getting any of them wrong is a class of bug rather than a one-off:
//   - a throw inside a handler must be REPORTED, not swallowed and not fatal;
//   - the promise/microtask queue must be drained, or an `await` in the handler
//     would not resume until the next unrelated wake-up;
//   - the UI must be repainted, because a handler exists to change state and
//     script state is opaque to the framework (it cannot know what moved).
void zs_call_handler(JSValue fn, int argc, JSValueConst *argv) {
    ZScript *zs = zs_current();
    if (!JS_IsFunction(zs->ctx, fn)) { return; }

    JSValue r = JS_Call(zs->ctx, fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(r)) { zs_dump_error(zs->ctx); }
    JS_FreeValue(zs->ctx, r);

    zs_run_jobs(zs);
    if (zs->app) { z_invalidate(zs->app); }
}

// --- the long-lived closure registry ---------------------------------------
//
// See internal.h: these entries outlive the build that made them, so unlike the
// per-build tap table they are never cleared wholesale. A one-shot entry (a
// promise settler) is released when it fires and its slot reused, so an app that
// polls in a loop reuses one slot forever rather than growing the array.

uint32_t zs_cb_add(JSContext *ctx, JSValueConst fn) {
    ZScript *zs = zs_current();
    for (uint32_t i = 0; i < zs->n_cbs; i++) {
        JSValue slot = JS_GetPropertyUint32(ctx, zs->cbs, i);
        bool free_slot = JS_IsUndefined(slot);
        JS_FreeValue(ctx, slot);
        if (free_slot) {
            JS_SetPropertyUint32(ctx, zs->cbs, i, JS_DupValue(ctx, fn));
            return i;
        }
    }
    uint32_t idx = zs->n_cbs++;
    JS_SetPropertyUint32(ctx, zs->cbs, idx, JS_DupValue(ctx, fn));
    return idx;
}

JSValue zs_cb_get(JSContext *ctx, uint32_t idx) {
    return JS_GetPropertyUint32(ctx, zs_current()->cbs, idx);
}

void zs_cb_del(JSContext *ctx, uint32_t idx) {
    JS_SetPropertyUint32(ctx, zs_current()->cbs, idx, JS_UNDEFINED);
}

// --- console ---------------------------------------------------------------

// Stringify one argument the way a developer expects at a glance: strings raw,
// everything else through JSON (so an object prints its shape, not
// "[object Object]"), falling back to ToString for what JSON refuses.
static void print_value(JSContext *ctx, FILE *out, JSValueConst v) {
    const char *s = NULL;
    if (JS_IsString(v) || JS_IsUndefined(v) || JS_IsNull(v) ||
        JS_IsNumber(v) || JS_IsBool(v) || JS_IsError(ctx, v)) {
        s = JS_ToCString(ctx, v);
    } else {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json) && !JS_IsUndefined(json)) {
            s = JS_ToCString(ctx, json);
        }
        JS_FreeValue(ctx, json);
        if (!s) {
            JS_FreeValue(ctx, JS_GetException(ctx));   // e.g. a cycle
            s = JS_ToCString(ctx, v);
        }
    }
    fputs(s ? s : "?", out);
    if (s) { JS_FreeCString(ctx, s); }
}

static JSValue js_console(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int magic) {
    (void)this_val;
    FILE *out = magic ? stderr : stdout;
    for (int i = 0; i < argc; i++) {
        if (i) { fputc(' ', out); }
        print_value(ctx, out, argv[i]);
    }
    fputc('\n', out);
    fflush(out);
    return JS_UNDEFINED;
}

// --- timers ----------------------------------------------------------------

static void timer_fired(ZApp *app, void *ud);

// Arm the app loop's one-shot for the soonest pending timer (z_after has a
// single slot, which is all this needs: the queue is kept sorted, so one
// wake-up is enough — the callback re-arms for whatever is next).
static void reschedule(ZScript *zs) {
    if (!zs->app) { return; }
    if (!zs->timers) {
        z_after_cancel(zs->app);
        return;
    }
    int64_t delay = zs->timers->due_ms - zs_now_ms();
    if (delay < 0) { delay = 0; }
    z_after(zs->app, (int)delay, timer_fired, NULL);
}

static void timer_insert(ZScript *zs, ZsTimer *t) {
    ZsTimer **slot = &zs->timers;
    while (*slot && (*slot)->due_ms <= t->due_ms) { slot = &(*slot)->next; }
    t->next = *slot;
    *slot = t;
}

static void timer_free(ZScript *zs, ZsTimer *t) {
    JS_FreeValue(zs->ctx, t->fn);
    free(t);
}

static void timer_fired(ZApp *app, void *ud) {
    (void)app; (void)ud;
    ZScript *zs = zs_current();
    int64_t now = zs_now_ms();

    // Pop everything due, running each callback. A callback may add or clear
    // timers, so the node is detached from the queue BEFORE it runs.
    while (zs->timers && zs->timers->due_ms <= now) {
        ZsTimer *t = zs->timers;
        zs->timers = t->next;

        JSValue r = JS_Call(zs->ctx, t->fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) { zs_dump_error(zs->ctx); }
        JS_FreeValue(zs->ctx, r);

        if (t->interval_ms > 0) {
            t->due_ms = now + t->interval_ms;
            timer_insert(zs, t);
        } else {
            timer_free(zs, t);
        }
    }

    zs_run_jobs(zs);
    reschedule(zs);
    z_invalidate(zs->app);   // a timer exists to change something
}

// setTimeout(fn, ms) / setInterval(fn, ms) -> id
static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv, int magic) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "setTimeout/setInterval(fn, ms)");
    }
    int32_t ms = 0;
    if (argc > 1 && JS_ToInt32(ctx, &ms, argv[1]) < 0) { return JS_EXCEPTION; }
    if (ms < 0) { ms = 0; }

    ZScript *zs = zs_current();
    ZsTimer *t = calloc(1, sizeof(*t));
    if (!t) { return JS_ThrowOutOfMemory(ctx); }
    t->id = zs->next_timer_id++;
    t->due_ms = zs_now_ms() + ms;
    // A zero-delay interval would spin the loop; clamp it the way browsers do.
    t->interval_ms = magic ? (ms > 0 ? ms : 1) : 0;
    t->fn = JS_DupValue(ctx, argv[0]);

    timer_insert(zs, t);
    reschedule(zs);
    return JS_NewInt64(ctx, t->id);
}

// clearTimeout(id) / clearInterval(id)
static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_UNDEFINED; }
    int64_t id;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) { return JS_EXCEPTION; }

    ZScript *zs = zs_current();
    for (ZsTimer **slot = &zs->timers; *slot; slot = &(*slot)->next) {
        if ((*slot)->id == id) {
            ZsTimer *t = *slot;
            *slot = t->next;
            timer_free(zs, t);
            reschedule(zs);
            break;
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry zs_console_funcs[] = {
    JS_CFUNC_MAGIC_DEF("log", 1, js_console, 0),
    JS_CFUNC_MAGIC_DEF("info", 1, js_console, 0),
    JS_CFUNC_MAGIC_DEF("debug", 1, js_console, 0),
    JS_CFUNC_MAGIC_DEF("warn", 1, js_console, 1),
    JS_CFUNC_MAGIC_DEF("error", 1, js_console, 1),
};

static void install_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, console, zs_console_funcs,
                               (int)(sizeof(zs_console_funcs) /
                                     sizeof(zs_console_funcs[0])));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "setTimeout",
                      JS_NewCFunctionMagic(ctx, js_set_timer, "setTimeout", 2,
                                           JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, global, "setInterval",
                      JS_NewCFunctionMagic(ctx, js_set_timer, "setInterval", 2,
                                           JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
                      JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "clearInterval",
                      JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));

    JS_FreeValue(ctx, global);

    // queueMicrotask rides the promise job queue the loop already drains.
    JSValue r = JS_Eval(ctx,
                        "globalThis.queueMicrotask = (fn) => "
                        "{ Promise.resolve().then(fn); };",
                        strlen("globalThis.queueMicrotask = (fn) => "
                               "{ Promise.resolve().then(fn); };"),
                        "<bootstrap>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);
}

// --- module loading --------------------------------------------------------

// Resolve `name` as imported from `base`. Built-ins keep their bare specifier;
// everything else is a path relative to the importing module's directory, so an
// app can `import { fmt } from "./util.js"`.
static char *module_normalize(JSContext *ctx, const char *base,
                              const char *name, void *opaque) {
    (void)opaque;
    if (zs_builtin_module(name) || strncmp(name, "zelto:", 6) == 0) {
        return js_strdup(ctx, name);
    }
    if (name[0] != '.') { return js_strdup(ctx, name); }

    // "<dir of base>/<name>", then collapse ./ and ../ segments.
    char *path = js_malloc(ctx, strlen(base) + strlen(name) + 2);
    if (!path) { return NULL; }
    strcpy(path, base);
    char *slash = strrchr(path, '/');
    if (slash) {
        slash[1] = '\0';
    } else {
        path[0] = '\0';
    }
    strcat(path, name);

    // In-place segment squash: a/./b -> a/b, a/b/../c -> a/c.
    char *out = path;
    for (char *p = path; *p;) {
        if (p[0] == '.' && p[1] == '/' && (p == path || p[-1] == '/')) {
            p += 2;
        } else if (p[0] == '.' && p[1] == '.' && p[2] == '/' &&
                   (p == path || p[-1] == '/') && out > path) {
            out--;                                       // step over the '/'
            while (out > path && out[-1] != '/') { out--; }
            p += 3;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return path;
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out) { *len_out = got; }
    return buf;
}

// Compile `source` as a module. The result is the module's function object: it
// OWNS a reference, and whoever takes it must dispose of it — the loader by
// freeing it (QuickJS keeps the module in the context's list), an evaluator by
// handing it to JS_EvalFunction (which consumes it).
static JSValue compile_module(JSContext *ctx, const char *source, size_t len,
                              const char *name) {
    return JS_Eval(ctx, source, len, name,
                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name,
                                  void *opaque) {
    (void)opaque;

    JSValue fn;
    const char *builtin = zs_builtin_module(name);
    if (builtin) {
        fn = compile_module(ctx, builtin, strlen(builtin), name);
    } else {
        size_t len = 0;
        char *source = read_file(name, &len);
        if (!source) {
            JS_ThrowReferenceError(ctx, "module not found: %s", name);
            return NULL;
        }
        fn = compile_module(ctx, source, len, name);
        free(source);
    }
    if (JS_IsException(fn)) { return NULL; }

    JSModuleDef *m = JS_VALUE_GET_PTR(fn);
    JS_FreeValue(ctx, fn);   // the module list holds the reference that matters
    return m;
}

// Compile + evaluate a module, settling its (possibly async) completion, and
// hand back its definition so the caller can read exports off the namespace.
// Returns NULL with the exception printed on failure.
static JSModuleDef *eval_module(JSContext *ctx, const char *source, size_t len,
                                const char *name) {
    JSValue fn = compile_module(ctx, source, len, name);
    if (JS_IsException(fn)) {
        zs_dump_error(ctx);
        return NULL;
    }
    JSModuleDef *m = JS_VALUE_GET_PTR(fn);

    JSValue promise = JS_EvalFunction(ctx, fn);   // consumes fn
    if (JS_IsException(promise)) {
        zs_dump_error(ctx);
        return NULL;
    }
    zs_run_jobs(zs_current());

    bool ok = true;
    if (JS_PromiseState(ctx, promise) == JS_PROMISE_REJECTED) {
        JS_Throw(ctx, JS_PromiseResult(ctx, promise));
        zs_dump_error(ctx);
        ok = false;
    }
    JS_FreeValue(ctx, promise);
    return ok ? m : NULL;
}

// --- the app body ----------------------------------------------------------

// One build: a fresh handler table, a fresh generation (so views from the last
// build are rejected rather than dereferenced), then straight into JS.
static ZView script_body(ZApp *app, void *state) {
    (void)state;
    ZScript *zs = zs_current();
    zs->app = app;
    zs->gen++;

    JS_FreeValue(zs->ctx, zs->handlers);
    zs->handlers = JS_NewArray(zs->ctx);
    zs->n_handlers = 0;

    // The gesture site tables are NOT thrown away — only the cursors reset, so
    // each site's slot is overwritten in place by this build's closure while a
    // gesture already latched onto slot N keeps resolving to site N (internal.h).
    zs->n_pan_sites = 0;
    zs->n_lp_sites = 0;

    zs->building = true;
    JSValue result = JS_Call(zs->ctx, zs->render, JS_UNDEFINED, 0, NULL);
    zs->building = false;
    if (JS_IsException(result)) {
        zs_dump_error(zs->ctx);
        JS_FreeValue(zs->ctx, result);
        // Render errors are a fact of app development; show them on the device
        // instead of dying, so the loop survives to render the next attempt.
        return Padding(24, Foreground(Z_COLOR_DANGER,
                                      z_text("Script error (see log)")));
    }

    ZView view = zs_view_of(zs->ctx, result);
    if (!view) {
        JS_FreeValue(zs->ctx, JS_GetException(zs->ctx));
        JS_FreeValue(zs->ctx, result);
        return Padding(24, Foreground(Z_COLOR_DANGER,
                                      z_text("Component returned no view")));
    }
    JS_FreeValue(zs->ctx, result);

    zs_run_jobs(zs);
    return view;
}

// --- entry -----------------------------------------------------------------

// Derive an app_id from the entry path when the caller has none: /a/b/notes.js
// -> "os.zelto.script.notes" (unique enough to window-manage; a packaged app
// passes its manifest id explicitly).
static char *derive_app_id(const char *entry) {
    const char *base = strrchr(entry, '/');
    base = base ? base + 1 : entry;

    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".js") == 0) { len -= 3; }

    char *id = malloc(len + 32);
    if (!id) { return NULL; }
    snprintf(id, len + 32, "os.zelto.script.%.*s", (int)len, base);
    return id;
}

int z_script_main(const char *entry, const char *app_id, const char *title) {
    ZScript *zs = zs_current();
    zs->rt = JS_NewRuntime();
    if (!zs->rt) {
        fprintf(stderr, "zelto-script: cannot create the JS runtime\n");
        return 1;
    }
    zs->ctx = JS_NewContext(zs->rt);
    if (!zs->ctx) {
        fprintf(stderr, "zelto-script: cannot create the JS context\n");
        JS_FreeRuntime(zs->rt);
        return 1;
    }
    zs->handlers = JS_NewArray(zs->ctx);
    zs->pan_sites = JS_NewArray(zs->ctx);
    zs->lp_sites = JS_NewArray(zs->ctx);
    zs->cbs = JS_NewArray(zs->ctx);
    zs->render = JS_UNDEFINED;
    zs->screen_render = JS_UNDEFINED;
    zs->root = JS_UNDEFINED;
    zs->next_timer_id = 1;

    JS_SetModuleLoaderFunc(zs->rt, module_normalize, module_loader, NULL);
    zs_init_native_module(zs->ctx);
    install_globals(zs->ctx);

    int status = 1;
    char *derived_id = NULL;

    // The core module owns the hook cells and the render bridge, so load it
    // first and keep its namespace — the app gets it too, via `import`.
    const char *core_src = zs_builtin_module("zelto");
    JSModuleDef *core = eval_module(zs->ctx, core_src, strlen(core_src),
                                    "zelto");
    if (!core) { goto out; }
    JSValue core_ns = JS_GetModuleNamespace(zs->ctx, core);
    zs->render = JS_GetPropertyStr(zs->ctx, core_ns, "__render");
    // __screen(i) renders one Navigator screen under its OWN hook scope: the C
    // navigator calls a screen's body directly, so the bridge needs its own entry
    // point rather than going through __render.
    zs->screen_render = JS_GetPropertyStr(zs->ctx, core_ns, "__screen");
    JSValue set_root = JS_GetPropertyStr(zs->ctx, core_ns, "__setRoot");
    JS_FreeValue(zs->ctx, core_ns);

    // The app's entry module: its default export is the root component.
    size_t len = 0;
    char *source = read_file(entry, &len);
    if (!source) {
        fprintf(stderr, "zelto-script: cannot read %s\n", entry);
        JS_FreeValue(zs->ctx, set_root);
        goto out;
    }
    JSModuleDef *app_mod = eval_module(zs->ctx, source, len, entry);
    free(source);
    if (!app_mod) {
        JS_FreeValue(zs->ctx, set_root);
        goto out;
    }

    JSValue app_ns = JS_GetModuleNamespace(zs->ctx, app_mod);
    zs->root = JS_GetPropertyStr(zs->ctx, app_ns, "default");
    JS_FreeValue(zs->ctx, app_ns);

    if (!JS_IsFunction(zs->ctx, zs->root)) {
        fprintf(stderr, "zelto-script: %s must `export default` a component "
                        "function\n", entry);
        JS_FreeValue(zs->ctx, set_root);
        goto out;
    }

    JSValue r = JS_Call(zs->ctx, set_root, JS_UNDEFINED, 1,
                        (JSValueConst *)&zs->root);
    JS_FreeValue(zs->ctx, set_root);
    if (JS_IsException(r)) {
        zs_dump_error(zs->ctx);
        JS_FreeValue(zs->ctx, r);
        goto out;
    }
    JS_FreeValue(zs->ctx, r);

    if (!app_id) {
        derived_id = derive_app_id(entry);
        app_id = derived_id;
    }

    // From here the app loop owns the process: body() re-enters JS per build,
    // timers fire from z_after, taps from the tree's handler table.
    status = z_app_main_id(NULL, script_body, title ? title : "Zelto Script",
                           app_id ? app_id : "os.zelto.script");

out:
    while (zs->timers) {
        ZsTimer *t = zs->timers;
        zs->timers = t->next;
        timer_free(zs, t);
    }
    JS_FreeValue(zs->ctx, zs->render);
    JS_FreeValue(zs->ctx, zs->screen_render);
    JS_FreeValue(zs->ctx, zs->root);
    JS_FreeValue(zs->ctx, zs->handlers);
    JS_FreeValue(zs->ctx, zs->pan_sites);
    JS_FreeValue(zs->ctx, zs->lp_sites);
    JS_FreeValue(zs->ctx, zs->cbs);
    for (uint32_t i = 0; i < zs->n_fields; i++) {
        free(zs->fields[i]);
    }
    free(zs->fields);
    JS_FreeContext(zs->ctx);
    JS_FreeRuntime(zs->rt);
    free(derived_id);
    return status;
}
