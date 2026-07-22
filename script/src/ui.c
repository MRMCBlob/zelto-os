// Native bindings: the "zelto:native" module.
//
// This is the whole C surface Zelto Script talks to — thin, boring functions
// that build ZViews and read/write app state. Everything ergonomic (chainable
// modifiers, hooks, the `zelto/ui` names) is JS on top of it (modules.c), which
// keeps the C side small enough to audit and the sugar cheap to change.
//
// Lifetime rule: a ZView is arena memory owned by the current build. Wrappers
// carry the build generation they were made in, and any binding that takes a
// view rejects one from an older generation instead of dereferencing freed
// arena memory (a script CAN stash a view in state; it just cannot use it).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "zelto/gfx.h"
#include "zelto/ui.h"

// --- ZView wrapper class ---------------------------------------------------

typedef struct {
    ZView view;
    uint64_t gen;
} ZsViewRef;

static JSClassID zs_view_class_id;

static void view_finalizer(JSRuntime *rt, JSValue val) {
    ZsViewRef *ref = JS_GetOpaque(val, zs_view_class_id);
    js_free_rt(rt, ref);   // the ZView itself is arena-owned; nothing to free
}

static JSClassDef zs_view_class = {
    "ZView",
    .finalizer = view_finalizer,
};

JSValue zs_view_new(JSContext *ctx, ZView v) {
    if (!v) { return JS_NULL; }
    JSValue obj = JS_NewObjectClass(ctx, (int)zs_view_class_id);
    if (JS_IsException(obj)) { return obj; }
    ZsViewRef *ref = js_malloc(ctx, sizeof(*ref));
    if (!ref) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    ref->view = v;
    ref->gen = zs_current()->gen;
    JS_SetOpaque(obj, ref);
    return obj;
}

ZView zs_view_of(JSContext *ctx, JSValueConst val) {
    ZsViewRef *ref = JS_GetOpaque2(ctx, val, zs_view_class_id);
    if (!ref) { return NULL; }
    if (ref->gen != zs_current()->gen) {
        JS_ThrowTypeError(ctx, "view is from a previous render (views cannot "
                               "be stored across renders)");
        return NULL;
    }
    return ref->view;
}

// --- small helpers ---------------------------------------------------------

// A view is arena-allocated out of the CURRENT build, and the app handle only
// exists once the loop is running — so every binding that touches either is
// only callable from inside a render. Module top-level code that tries anyway
// (`const header = Text("hi")` outside a component) gets a clear exception
// instead of dereferencing a null arena.
static bool require_render(JSContext *ctx) {
    ZScript *zs = zs_current();
    if (!zs->building || !zs->app) {
        JS_ThrowTypeError(ctx, "views are only available during a render (call "
                               "this inside a component)");
        return false;
    }
    return true;
}

// Weaker: the app loop is running. A timer or a tap handler runs BETWEEN builds
// and may legitimately ask for the app's size or invalidate it — it just cannot
// do so before the loop exists (module top-level).
static bool require_app(JSContext *ctx) {
    if (!zs_current()->app) {
        JS_ThrowTypeError(ctx, "the app is not running yet (call this from a "
                               "component, an effect, or a handler)");
        return false;
    }
    return true;
}

static float arg_f(JSContext *ctx, JSValueConst v, float dflt) {
    double d;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { return dflt; }
    if (JS_ToFloat64(ctx, &d, v) < 0) { return dflt; }
    return (float)d;
}

static int32_t arg_i(JSContext *ctx, JSValueConst v, int32_t dflt) {
    int32_t i;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { return dflt; }
    if (JS_ToInt32(ctx, &i, v) < 0) { return dflt; }
    return i;
}

// A colour crosses the boundary as a packed 0xRRGGBBAA number (JS has no
// structs, and a plain number keeps the JS-side palette a table of constants).
static ZColor color_of(JSContext *ctx, JSValueConst v, ZColor dflt) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) { return dflt; }
    uint32_t rgba;
    if (JS_ToUint32(ctx, &rgba, v) < 0) { return dflt; }
    return z_rgba((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                  (uint8_t)(rgba >> 8), (uint8_t)rgba);
}

static uint32_t color_pack(ZColor c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) |
           (uint32_t)c.a;
}

// Read `obj.name` as a float/int/colour, leaving the default when absent.
static float prop_f(JSContext *ctx, JSValueConst obj, const char *name,
                    float dflt) {
    if (!JS_IsObject(obj)) { return dflt; }
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    float f = arg_f(ctx, v, dflt);
    JS_FreeValue(ctx, v);
    return f;
}

static int32_t prop_i(JSContext *ctx, JSValueConst obj, const char *name,
                      int32_t dflt) {
    if (!JS_IsObject(obj)) { return dflt; }
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int32_t i = arg_i(ctx, v, dflt);
    JS_FreeValue(ctx, v);
    return i;
}

static ZColor prop_color(JSContext *ctx, JSValueConst obj, const char *name,
                         ZColor dflt) {
    if (!JS_IsObject(obj)) { return dflt; }
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    ZColor c = color_of(ctx, v, dflt);
    JS_FreeValue(ctx, v);
    return c;
}

// --- stacks ----------------------------------------------------------------

// stack(axis, childrenArray, opts) -> view
static JSValue js_stack(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 2) { return JS_ThrowTypeError(ctx, "stack(axis, children, opts)"); }

    ZStackOpts opts = {0};
    uint32_t len = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
    if (JS_ToUint32(ctx, &len, lenv) < 0) { len = 0; }
    JS_FreeValue(ctx, lenv);

    if (len > Z_MAX_CHILDREN) {
        return JS_ThrowRangeError(ctx, "a stack takes at most %d children (got %u)",
                                  Z_MAX_CHILDREN, len);
    }

    int n = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue child = JS_GetPropertyUint32(ctx, argv[1], i);
        // null/undefined children are skipped, so `cond && View(...)` works the
        // way it does in every other declarative UI.
        if (JS_IsNull(child) || JS_IsUndefined(child) || JS_IsBool(child)) {
            JS_FreeValue(ctx, child);
            continue;
        }
        ZView v = zs_view_of(ctx, child);
        JS_FreeValue(ctx, child);
        if (!v) { return JS_EXCEPTION; }
        opts.children[n++] = v;
    }

    JSValueConst o = argc > 2 ? argv[2] : JS_UNDEFINED;
    opts.spacing = prop_f(ctx, o, "spacing", 0.0f);
    opts.padding = prop_f(ctx, o, "padding", 0.0f);
    opts.grow = prop_f(ctx, o, "grow", 0.0f);
    opts.align = (ZAlign)prop_i(ctx, o, "align", Z_ALIGN_LEADING);

    return zs_view_new(ctx, z_stack((ZAxis)arg_i(ctx, argv[0], Z_AXIS_VERTICAL),
                                    &opts));
}

static JSValue js_spacer(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    return zs_view_new(ctx, z_spacer());
}

// --- content ---------------------------------------------------------------

static JSValue js_text(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) { return JS_ThrowTypeError(ctx, "text(string)"); }
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) { return JS_EXCEPTION; }
    // "%s": the script's string is DATA, never a format string.
    JSValue v = zs_view_new(ctx, z_text("%s", s));
    JS_FreeCString(ctx, s);
    return v;
}

static JSValue js_rect(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    JSValueConst o = argc > 0 ? argv[0] : JS_UNDEFINED;
    ZRectOpts opts = {
        .color = prop_color(ctx, o, "color", Z_COLOR_SURFACE),
        .width = prop_f(ctx, o, "width", 0.0f),
        .height = prop_f(ctx, o, "height", 0.0f),
        .radius = prop_f(ctx, o, "radius", 0.0f),
        .grow = prop_f(ctx, o, "grow", 0.0f),
    };
    return zs_view_new(ctx, z_rect(&opts));
}

static JSValue js_image(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) { return JS_ThrowTypeError(ctx, "image(path)"); }
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) { return JS_EXCEPTION; }
    JSValue v = zs_view_new(ctx, z_image(path));
    JS_FreeCString(ctx, path);
    return v;
}

static JSValue js_image_loads(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_FALSE; }
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) { return JS_EXCEPTION; }
    bool ok = z_image_loads(path);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

static JSValue js_scroll(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) { return JS_ThrowTypeError(ctx, "scroll(content, axis)"); }
    ZView content = zs_view_of(ctx, argv[0]);
    if (!content) { return JS_EXCEPTION; }
    ZScrollOpts opts = {
        .children = {content},
        .axis = (ZAxis)(argc > 1 ? arg_i(ctx, argv[1], Z_AXIS_VERTICAL)
                                 : Z_AXIS_VERTICAL),
    };
    return zs_view_new(ctx, z_scroll_view(zs_current()->app, &opts));
}

// --- modifiers -------------------------------------------------------------
//
// Each takes (view, ...args) and returns the same view, so the JS wrapper can
// chain: Text("hi").font(TITLE).color(ACCENT).padding(8).

#define ZS_MODIFIER(NAME, BODY)                                             \
    static JSValue NAME(JSContext *ctx, JSValueConst this_val, int argc,    \
                        JSValueConst *argv) {                               \
        (void)this_val; (void)argc;                                         \
        ZView v = argc > 0 ? zs_view_of(ctx, argv[0]) : NULL;               \
        if (!v) { return JS_EXCEPTION; }                                    \
        BODY                                                                \
        return JS_DupValue(ctx, argv[0]);                                   \
    }

ZS_MODIFIER(js_padding,      Padding(arg_f(ctx, argv[1], 0.0f), v);)
ZS_MODIFIER(js_corner_radius, CornerRadius(arg_f(ctx, argv[1], 0.0f), v);)
ZS_MODIFIER(js_grow,         Grow(arg_f(ctx, argv[1], 1.0f), v);)
ZS_MODIFIER(js_opacity,      Opacity(arg_f(ctx, argv[1], 1.0f), v);)
ZS_MODIFIER(js_shadow,       Shadow(arg_f(ctx, argv[1], Z_ELEV_2), v);)
ZS_MODIFIER(js_fill,         Fill(v);)
ZS_MODIFIER(js_cover,        Cover(v);)
// wrapText(string, width [, size]) — prose broken to a pixel column.
//
// A script app could not wrap text AT ALL before P46, which is why the JS Demo's
// two paragraphs painted through the right edge of their cards: a plain Text
// measures to one line however long, and the toolkit's WrapText needs the ZApp to
// measure with at build time, which nothing exposed to JS. The catalogue audit
// found it (ZELTO_PROBE_TAPS: 'Zelto Script: gestures, motion, text input, and
// the system APIs.' wanted 728 units in a 680 box). This is the same
// z_text_wrap() a C app calls; the width is required for the same reason.
static JSValue js_wrap_text(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "wrapText(string, width [, size])");
    }
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) { return JS_EXCEPTION; }
    float w = arg_f(ctx, argv[1], 0.0f);
    ZFont size = (ZFont)arg_i(ctx, argv[2], Z_FONT_BODY);
    JSValue v = zs_view_new(ctx, z_text_wrap(zs_current()->app, s,
                                             &(ZWrapOpts){.width = w,
                                                          .size = size}));
    JS_FreeCString(ctx, s);
    return v;
}

// ellipsizeText(string, width [, size]) — an identifier cut to fit one line.
//
// The other half of P46's finding, left undone: a script app got wrapText and
// still could not TRUNCATE. The two are not interchangeable and the difference is
// about who wrote the string. Prose you wrote wraps; a filename, an app id or a
// contact's name — a string the app did not choose, in a row whose height is
// fixed — has to be cut, because wrapping it makes the row grow by however many
// lines the data happens to need. That is the case a script app is MOST likely to
// hit, since almost everything a script app displays came from somewhere else.
static JSValue js_ellipsize_text(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "ellipsizeText(string, width [, size])");
    }
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) { return JS_EXCEPTION; }
    float w = arg_f(ctx, argv[1], 0.0f);
    ZFont size = (ZFont)arg_i(ctx, argv[2], Z_FONT_BODY);
    JSValue v = zs_view_new(ctx,
        z_text_ellipsize(zs_current()->app, s,
                         &(ZEllipsizeOpts){.width = w, .size = size}));
    JS_FreeCString(ctx, s);
    return v;
}

ZS_MODIFIER(js_text_shadow,  TextShadow(v);)
ZS_MODIFIER(js_frame,        Frame(arg_f(ctx, argv[1], 0.0f),
                                   arg_f(ctx, argv[2], 0.0f), v);)
ZS_MODIFIER(js_background,   Background(color_of(ctx, argv[1], Z_COLOR_SURFACE), v);)
ZS_MODIFIER(js_foreground,   Foreground(color_of(ctx, argv[1], Z_COLOR_TEXT), v);)
ZS_MODIFIER(js_font,         Font((ZFont)arg_i(ctx, argv[1], Z_FONT_BODY), v);)
ZS_MODIFIER(js_weight,       Weight((ZWeight)arg_i(ctx, argv[1], Z_WEIGHT_REGULAR), v);)

// A tap handler is a JS closure, but a ZTapAction carries a void* — so the
// closure is parked in the per-build handler array and passed by index. The
// array is rebuilt every render, so an entry lives exactly as long as the tree
// that can fire it.
static void tap_trampoline(ZApp *app, void *state, void *data) {
    (void)app; (void)state;
    ZScript *zs = zs_current();
    JSValue fn = JS_GetPropertyUint32(zs->ctx, zs->handlers,
                                      (uint32_t)(uintptr_t)data);
    // A handler exists to change state, and script state is opaque to the
    // framework, so every tap ends in a rebuild (zs_call_handler repaints).
    zs_call_handler(fn, 0, NULL);
    JS_FreeValue(zs->ctx, fn);
}

static JSValue js_on_tap(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "onTap(view, fn)");
    }
    ZView v = zs_view_of(ctx, argv[0]);
    if (!v) { return JS_EXCEPTION; }

    ZScript *zs = zs_current();
    uint32_t idx = zs->n_handlers++;
    JS_SetPropertyUint32(ctx, zs->handlers, idx, JS_DupValue(ctx, argv[1]));

    OnTapData(tap_trampoline, (void *)(uintptr_t)idx, v);
    return JS_DupValue(ctx, argv[0]);
}

// --- gestures --------------------------------------------------------------
//
// Unlike a tap, a gesture's data pointer is latched when the gesture BEGINS and
// reused for the rest of it, across any rebuild that happens mid-drag — so these
// closures live in the by-call-site tables, not the per-build one (internal.h).

static void pan_trampoline(ZApp *app, void *state, void *data,
                           const ZPanEvent *e) {
    (void)app; (void)state;
    ZScript *zs = zs_current();
    JSValue fn = JS_GetPropertyUint32(zs->ctx, zs->pan_sites,
                                      (uint32_t)(uintptr_t)data);

    JSValue ev = JS_NewObject(zs->ctx);
    JS_SetPropertyStr(zs->ctx, ev, "x", JS_NewFloat64(zs->ctx, e->x));
    JS_SetPropertyStr(zs->ctx, ev, "y", JS_NewFloat64(zs->ctx, e->y));
    JS_SetPropertyStr(zs->ctx, ev, "dx",
                      JS_NewFloat64(zs->ctx, e->translation_x));
    JS_SetPropertyStr(zs->ctx, ev, "dy",
                      JS_NewFloat64(zs->ctx, e->translation_y));
    JS_SetPropertyStr(zs->ctx, ev, "vx", JS_NewFloat64(zs->ctx, e->velocity_x));
    JS_SetPropertyStr(zs->ctx, ev, "vy", JS_NewFloat64(zs->ctx, e->velocity_y));
    JS_SetPropertyStr(zs->ctx, ev, "phase", JS_NewInt32(zs->ctx, (int)e->phase));

    zs_call_handler(fn, 1, (JSValueConst *)&ev);
    JS_FreeValue(zs->ctx, ev);
    JS_FreeValue(zs->ctx, fn);
}

static JSValue js_on_pan(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "onPan(view, fn)");
    }
    ZView v = zs_view_of(ctx, argv[0]);
    if (!v) { return JS_EXCEPTION; }

    ZScript *zs = zs_current();
    uint32_t site = zs->n_pan_sites++;
    JS_SetPropertyUint32(ctx, zs->pan_sites, site, JS_DupValue(ctx, argv[1]));

    OnPanData(pan_trampoline, (void *)(uintptr_t)site, v);
    return JS_DupValue(ctx, argv[0]);
}

static void long_press_trampoline(ZApp *app, void *state, void *data, float x,
                                  float y) {
    (void)app; (void)state;
    ZScript *zs = zs_current();
    JSValue fn = JS_GetPropertyUint32(zs->ctx, zs->lp_sites,
                                      (uint32_t)(uintptr_t)data);

    JSValue ev = JS_NewObject(zs->ctx);
    JS_SetPropertyStr(zs->ctx, ev, "x", JS_NewFloat64(zs->ctx, x));
    JS_SetPropertyStr(zs->ctx, ev, "y", JS_NewFloat64(zs->ctx, y));

    zs_call_handler(fn, 1, (JSValueConst *)&ev);
    JS_FreeValue(zs->ctx, ev);
    JS_FreeValue(zs->ctx, fn);
}

static JSValue js_on_long_press(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "onLongPress(view, fn)");
    }
    ZView v = zs_view_of(ctx, argv[0]);
    if (!v) { return JS_EXCEPTION; }

    ZScript *zs = zs_current();
    uint32_t site = zs->n_lp_sites++;
    JS_SetPropertyUint32(ctx, zs->lp_sites, site, JS_DupValue(ctx, argv[1]));

    OnLongPress(long_press_trampoline, (void *)(uintptr_t)site, v);
    return JS_DupValue(ctx, argv[0]);
}

// --- animated values -------------------------------------------------------
//
// An animated value is a RETAINED spring cell owned by the toolkit, so — unlike a
// ZView — it deliberately outlives the build that asked for it: that is the whole
// point (a spring in flight survives the rebuilds it drives). It is therefore
// wrapped WITHOUT a generation check.
//
// The cell is looked up by identity (z_animated_keyed), not by call order, and the
// key is the hook's cell index (core.js). The pointer is resolved ONCE, during the
// render, and cached in the wrapper: keyed cells are swept per screen, and a
// handler running between builds has no current screen, so re-resolving a key
// outside a render would look in the wrong table. A cell whose hook stops being
// called is swept — so, exactly as in C, do not keep an animated value past the
// life of the component that owns it.
typedef struct {
    ZAnimated *v;
} ZsAnimRef;

static JSClassID zs_anim_class_id;

static void anim_finalizer(JSRuntime *rt, JSValue val) {
    ZsAnimRef *ref = JS_GetOpaque(val, zs_anim_class_id);
    js_free_rt(rt, ref);   // the cell is toolkit-owned; nothing to free
}

static JSClassDef zs_anim_class = {
    "ZAnimated",
    .finalizer = anim_finalizer,
};

static ZAnimated *anim_of(JSContext *ctx, JSValueConst val) {
    ZsAnimRef *ref = JS_GetOpaque2(ctx, val, zs_anim_class_id);
    if (!ref) { return NULL; }
    return ref->v;
}

// animatedValue(key, initial) -> ZAnimated wrapper (only inside a render).
static JSValue js_animated_value(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "animatedValue(key, initial)");
    }
    int64_t key = 0;
    if (JS_ToInt64(ctx, &key, argv[0]) < 0) { return JS_EXCEPTION; }
    float initial = argc > 1 ? arg_f(ctx, argv[1], 0.0f) : 0.0f;

    ZAnimated *v = z_animated_keyed(zs_current()->app, (uint64_t)key, initial);
    if (!v) { return JS_NULL; }

    JSValue obj = JS_NewObjectClass(ctx, (int)zs_anim_class_id);
    if (JS_IsException(obj)) { return obj; }
    ZsAnimRef *ref = js_malloc(ctx, sizeof(*ref));
    if (!ref) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    ref->v = v;
    JS_SetOpaque(obj, ref);
    return obj;
}

// The motion verbs. `spring` is a named token (STANDARD / SNAPPY / PRESS), never
// raw numbers — and every one of these honours Reduce Motion inside the toolkit
// (it collapses a spring to a jump), so a script gets the accessibility
// behaviour for free rather than having to check sys.reduce_motion itself.
#define ZS_ANIM_OP(NAME, BODY)                                              \
    static JSValue NAME(JSContext *ctx, JSValueConst this_val, int argc,    \
                        JSValueConst *argv) {                               \
        (void)this_val; (void)argc;                                         \
        ZAnimated *v = argc > 0 ? anim_of(ctx, argv[0]) : NULL;             \
        if (!v) { return JS_EXCEPTION; }                                    \
        BODY                                                                \
    }

ZS_ANIM_OP(js_anim_get, return JS_NewFloat64(ctx, z_animated_get(v));)
ZS_ANIM_OP(js_anim_target, return JS_NewFloat64(ctx, z_animated_target(v));)
ZS_ANIM_OP(js_anim_active, return JS_NewBool(ctx, z_animated_active(v));)
ZS_ANIM_OP(js_anim_set, z_animated_set(v, arg_f(ctx, argv[1], 0.0f));
           return JS_UNDEFINED;)
ZS_ANIM_OP(js_anim_pin, z_animated_pin(v, arg_f(ctx, argv[1], 0.0f));
           return JS_UNDEFINED;)
ZS_ANIM_OP(js_anim_grab, return JS_NewFloat64(ctx, z_animated_grab(v));)
ZS_ANIM_OP(js_anim_spring,
           z_animated_spring_with(v, arg_f(ctx, argv[1], 0.0f),
                                  (ZSpring)arg_i(ctx, argv[2],
                                                 Z_SPRING_STANDARD));
           return JS_UNDEFINED;)
ZS_ANIM_OP(js_anim_spring_velocity,
           z_animated_spring_velocity(v, arg_f(ctx, argv[1], 0.0f),
                                      (ZSpring)arg_i(ctx, argv[2],
                                                     Z_SPRING_STANDARD),
                                      arg_f(ctx, argv[3], 0.0f));
           return JS_UNDEFINED;)

// offset(view, animX|null, y) / offsetXY(view, x, y) /
// offsetXYAnimated(view, animX|null, animY|null)
static JSValue js_offset(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) { return JS_ThrowTypeError(ctx, "offset(view, x, y)"); }
    ZView view = zs_view_of(ctx, argv[0]);
    if (!view) { return JS_EXCEPTION; }
    ZAnimated *x = anim_of(ctx, argv[1]);
    if (!x) { JS_FreeValue(ctx, JS_GetException(ctx)); }   // null = no x anim
    Offset(x, arg_f(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, 0.0f), view);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_offset_xy(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) { return JS_ThrowTypeError(ctx, "offsetXY(view, x, y)"); }
    ZView view = zs_view_of(ctx, argv[0]);
    if (!view) { return JS_EXCEPTION; }
    OffsetXY(arg_f(ctx, argv[1], 0.0f), arg_f(ctx, argv[2], 0.0f), view);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue js_offset_xy_animated(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 3) {
        return JS_ThrowTypeError(ctx, "offsetXYAnimated(view, x, y)");
    }
    ZView view = zs_view_of(ctx, argv[0]);
    if (!view) { return JS_EXCEPTION; }
    // Either axis may be null (leave it unshifted), so a failed unwrap is not an
    // error here — clear the pending exception and pass NULL.
    ZAnimated *x = anim_of(ctx, argv[1]);
    if (!x) { JS_FreeValue(ctx, JS_GetException(ctx)); }
    ZAnimated *y = anim_of(ctx, argv[2]);
    if (!y) { JS_FreeValue(ctx, JS_GetException(ctx)); }
    OffsetXYAnimated(x, y, view);
    return JS_DupValue(ctx, argv[0]);
}

// --- navigation ------------------------------------------------------------
//
// One C trampoline serves every screen: ZScreenFn is a plain function pointer, so
// the screen's JS component is identified by the INDEX handed through `props`.
// core.js owns the instance array (and each screen's own hook scope); C only
// passes the integer through. The root screen is index 0 — z_navigator gives the
// root props == NULL, which is exactly (uintptr_t)0.
static ZView screen_trampoline(ZApp *app, void *props) {
    (void)app;
    ZScript *zs = zs_current();
    JSValue arg = JS_NewUint32(zs->ctx, (uint32_t)(uintptr_t)props);
    JSValue r = JS_Call(zs->ctx, zs->screen_render, JS_UNDEFINED, 1,
                        (JSValueConst *)&arg);
    JS_FreeValue(zs->ctx, arg);

    if (JS_IsException(r)) {
        zs_dump_error(zs->ctx);
        JS_FreeValue(zs->ctx, r);
        return Padding(24, Foreground(Z_COLOR_DANGER,
                                      z_text("Screen error (see log)")));
    }
    ZView v = zs_view_of(zs->ctx, r);
    JS_FreeValue(zs->ctx, r);
    if (!v) {
        JS_FreeValue(zs->ctx, JS_GetException(zs->ctx));
        return Padding(24, Foreground(Z_COLOR_DANGER,
                                      z_text("Screen returned no view")));
    }
    zs_run_jobs(zs);
    return v;
}

static JSValue js_navigator(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    return zs_view_new(ctx, Navigator(zs_current()->app,
                                      .root = screen_trampoline));
}

static JSValue js_nav_push(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) { return JS_ThrowTypeError(ctx, "navPush(index)"); }
    uint32_t idx = (uint32_t)arg_i(ctx, argv[0], 0);
    z_nav_push(z_navigation(zs_current()->app), screen_trampoline,
               (void *)(uintptr_t)idx);
    return JS_UNDEFINED;
}

static JSValue js_nav_pop(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    z_nav_pop(z_navigation(zs_current()->app));
    return JS_UNDEFINED;
}

static JSValue js_nav_depth(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!zs_current()->app) { return JS_NewInt32(ctx, 1); }
    return JS_NewInt32(ctx, z_nav_depth(z_navigation(zs_current()->app)));
}

// --- text fields -----------------------------------------------------------
//
// A ZTextField is state the toolkit WRITES INTO between builds (the keyboard
// commits into it), so it cannot be arena memory: the host heap-allocates one per
// field and hands JS an index. The buffer is the single source of truth — a
// script reads `field.text` during a render and the on-screen keyboard fills it,
// with no key handling in the app at all (P21).
struct ZsField {
    ZTextField f;
    char last[Z_TEXTFIELD_CAP];   // snapshot, to see WHICH field changed
    int32_t cb;                   // registry index of the JS onChange (-1 = none)
};

// ZTextField.on_change carries no user-data pointer, so one C callback serves
// every field and cannot say which one moved. Diffing each buffer against its
// snapshot answers that — a handful of strcmp on a couple of fields, on a human
// keystroke, is not a cost worth designing around.
void zs_fields_changed(ZApp *app, void *state) {
    (void)app; (void)state;
    ZScript *zs = zs_current();
    for (uint32_t i = 0; i < zs->n_fields; i++) {
        ZsField *fl = zs->fields[i];
        if (strcmp(fl->f.text, fl->last) == 0) { continue; }
        snprintf(fl->last, sizeof(fl->last), "%s", fl->f.text);
        if (fl->cb < 0) { continue; }

        JSValue fn = zs_cb_get(zs->ctx, (uint32_t)fl->cb);
        JSValue arg = JS_NewString(zs->ctx, fl->f.text);
        zs_call_handler(fn, 1, (JSValueConst *)&arg);
        JS_FreeValue(zs->ctx, arg);
        JS_FreeValue(zs->ctx, fn);
    }
    if (zs->app) { z_invalidate(zs->app); }
}

static ZsField *field_at(JSContext *ctx, JSValueConst v) {
    ZScript *zs = zs_current();
    int32_t i = arg_i(ctx, v, -1);
    if (i < 0 || (uint32_t)i >= zs->n_fields) {
        JS_ThrowRangeError(ctx, "no such text field");
        return NULL;
    }
    return zs->fields[i];
}

// fieldNew(initialText) -> index
static JSValue js_field_new(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    ZScript *zs = zs_current();

    ZsField **grown = realloc(zs->fields, (zs->n_fields + 1) * sizeof(*grown));
    if (!grown) { return JS_ThrowOutOfMemory(ctx); }
    zs->fields = grown;

    ZsField *fl = calloc(1, sizeof(*fl));
    if (!fl) { return JS_ThrowOutOfMemory(ctx); }
    fl->cb = -1;
    fl->f.on_change = zs_fields_changed;

    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s) {
            snprintf(fl->f.text, sizeof(fl->f.text), "%s", s);
            JS_FreeCString(ctx, s);
        }
        fl->f.len = (int)strlen(fl->f.text);
        fl->f.caret = fl->f.anchor = fl->f.len;
        snprintf(fl->last, sizeof(fl->last), "%s", fl->f.text);
    }

    zs->fields[zs->n_fields] = fl;
    return JS_NewUint32(ctx, zs->n_fields++);
}

static JSValue js_field_text(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "fieldText(index)"); }
    ZsField *fl = field_at(ctx, argv[0]);
    if (!fl) { return JS_EXCEPTION; }
    return JS_NewString(ctx, fl->f.text);
}

static JSValue js_field_set_text(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) { return JS_ThrowTypeError(ctx, "fieldSetText(index, text)"); }
    ZsField *fl = field_at(ctx, argv[0]);
    if (!fl) { return JS_EXCEPTION; }
    const char *s = JS_ToCString(ctx, argv[1]);
    if (!s) { return JS_EXCEPTION; }

    snprintf(fl->f.text, sizeof(fl->f.text), "%s", s);
    JS_FreeCString(ctx, s);
    fl->f.len = (int)strlen(fl->f.text);
    // Keep the caret inside the new buffer and drop any selection: a programmatic
    // rewrite invalidates whatever the user had highlighted.
    fl->f.caret = fl->f.anchor = fl->f.len;
    snprintf(fl->last, sizeof(fl->last), "%s", fl->f.text);   // not a user edit

    if (zs_current()->app) { z_invalidate(zs_current()->app); }
    return JS_UNDEFINED;
}

static JSValue js_field_on_change(JSContext *ctx, JSValueConst this_val, int argc,
                                  JSValueConst *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "fieldOnChange(index, fn)");
    }
    ZsField *fl = field_at(ctx, argv[0]);
    if (!fl) { return JS_EXCEPTION; }
    if (fl->cb >= 0) { zs_cb_del(ctx, (uint32_t)fl->cb); }
    fl->cb = (int32_t)zs_cb_add(ctx, argv[1]);
    return JS_UNDEFINED;
}

// textField(index, placeholder) -> view
static JSValue js_text_field(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val;
    if (!require_render(ctx)) { return JS_EXCEPTION; }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "textField(index, placeholder)");
    }
    ZsField *fl = field_at(ctx, argv[0]);
    if (!fl) { return JS_EXCEPTION; }

    const char *placeholder = NULL;
    if (argc > 1 && JS_IsString(argv[1])) {
        placeholder = JS_ToCString(ctx, argv[1]);
    }
    JSValue v = zs_view_new(ctx, TextField(zs_current()->app, &fl->f,
                                           placeholder ? placeholder : ""));
    if (placeholder) { JS_FreeCString(ctx, placeholder); }
    return v;
}

// --- app / host ------------------------------------------------------------

static JSValue js_app_width(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    return JS_NewInt32(ctx, z_app_width(zs_current()->app));
}

static JSValue js_app_height(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    return JS_NewInt32(ctx, z_app_height(zs_current()->app));
}

static JSValue js_invalidate(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    // Not an error before the loop exists: a setState during the first render
    // (or from an effect that runs then) has nothing to invalidate yet — the
    // build it feeds is the one already in flight.
    if (zs_current()->app) { z_invalidate(zs_current()->app); }
    (void)ctx;
    return JS_UNDEFINED;
}

static JSValue js_quit(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!require_app(ctx)) { return JS_EXCEPTION; }
    z_app_quit(zs_current()->app);
    return JS_UNDEFINED;
}

// --- storage (prefs) -------------------------------------------------------

static JSValue js_prefs_get(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "prefsGet(key)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    const char *val = z_prefs_get_str(key, NULL);
    JS_FreeCString(ctx, key);
    return val ? JS_NewString(ctx, val) : JS_NULL;
}

static JSValue js_prefs_set(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) { return JS_ThrowTypeError(ctx, "prefsSet(key, value)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!val) {
        JS_FreeCString(ctx, key);
        return JS_EXCEPTION;
    }
    bool ok = z_prefs_set_str(key, val);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    return JS_NewBool(ctx, ok);
}

static JSValue js_prefs_remove(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) { return JS_ThrowTypeError(ctx, "prefsRemove(key)"); }
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) { return JS_EXCEPTION; }
    bool ok = z_prefs_remove(key);
    JS_FreeCString(ctx, key);
    return JS_NewBool(ctx, ok);
}

// --- module table ----------------------------------------------------------

static const JSCFunctionListEntry zs_native_funcs[] = {
    // views
    JS_CFUNC_DEF("stack", 3, js_stack),
    JS_CFUNC_DEF("spacer", 0, js_spacer),
    JS_CFUNC_DEF("text", 1, js_text),
    JS_CFUNC_DEF("wrapText", 2, js_wrap_text),
    JS_CFUNC_DEF("ellipsizeText", 2, js_ellipsize_text),
    JS_CFUNC_DEF("rect", 1, js_rect),
    JS_CFUNC_DEF("image", 1, js_image),
    JS_CFUNC_DEF("imageLoads", 1, js_image_loads),
    JS_CFUNC_DEF("scroll", 2, js_scroll),
    // modifiers
    JS_CFUNC_DEF("padding", 2, js_padding),
    JS_CFUNC_DEF("frame", 3, js_frame),
    JS_CFUNC_DEF("background", 2, js_background),
    JS_CFUNC_DEF("foreground", 2, js_foreground),
    JS_CFUNC_DEF("cornerRadius", 2, js_corner_radius),
    JS_CFUNC_DEF("font", 2, js_font),
    JS_CFUNC_DEF("weight", 2, js_weight),
    JS_CFUNC_DEF("grow", 2, js_grow),
    JS_CFUNC_DEF("opacity", 2, js_opacity),
    JS_CFUNC_DEF("shadow", 2, js_shadow),
    JS_CFUNC_DEF("fill", 1, js_fill),
    JS_CFUNC_DEF("cover", 1, js_cover),
    JS_CFUNC_DEF("textShadow", 1, js_text_shadow),
    JS_CFUNC_DEF("onTap", 2, js_on_tap),
    // gestures
    JS_CFUNC_DEF("onPan", 2, js_on_pan),
    JS_CFUNC_DEF("onLongPress", 2, js_on_long_press),
    // animation
    JS_CFUNC_DEF("animatedValue", 2, js_animated_value),
    JS_CFUNC_DEF("animGet", 1, js_anim_get),
    JS_CFUNC_DEF("animTarget", 1, js_anim_target),
    JS_CFUNC_DEF("animActive", 1, js_anim_active),
    JS_CFUNC_DEF("animSet", 2, js_anim_set),
    JS_CFUNC_DEF("animPin", 2, js_anim_pin),
    JS_CFUNC_DEF("animGrab", 1, js_anim_grab),
    JS_CFUNC_DEF("animSpring", 3, js_anim_spring),
    JS_CFUNC_DEF("animSpringVelocity", 4, js_anim_spring_velocity),
    JS_CFUNC_DEF("offset", 3, js_offset),
    JS_CFUNC_DEF("offsetXY", 3, js_offset_xy),
    JS_CFUNC_DEF("offsetXYAnimated", 3, js_offset_xy_animated),
    // navigation
    JS_CFUNC_DEF("navigator", 0, js_navigator),
    JS_CFUNC_DEF("navPush", 1, js_nav_push),
    JS_CFUNC_DEF("navPop", 0, js_nav_pop),
    JS_CFUNC_DEF("navDepth", 0, js_nav_depth),
    // text input
    JS_CFUNC_DEF("fieldNew", 1, js_field_new),
    JS_CFUNC_DEF("fieldText", 1, js_field_text),
    JS_CFUNC_DEF("fieldSetText", 2, js_field_set_text),
    JS_CFUNC_DEF("fieldOnChange", 2, js_field_on_change),
    JS_CFUNC_DEF("textField", 2, js_text_field),
    // app
    JS_CFUNC_DEF("appWidth", 0, js_app_width),
    JS_CFUNC_DEF("appHeight", 0, js_app_height),
    JS_CFUNC_DEF("invalidate", 0, js_invalidate),
    JS_CFUNC_DEF("quit", 0, js_quit),
    // storage
    JS_CFUNC_DEF("prefsGet", 1, js_prefs_get),
    JS_CFUNC_DEF("prefsSet", 2, js_prefs_set),
    JS_CFUNC_DEF("prefsRemove", 1, js_prefs_remove),
};

// The design tokens (gfx.h) and the type/weight/axis enums, exported as plain
// numbers so the JS palette in zelto/ui is a re-export rather than a second
// source of truth for the colours.
static void export_tokens(JSContext *ctx, JSModuleDef *m) {
    struct { const char *name; uint32_t rgba; } colors[] = {
        {"COLOR_BG", color_pack(Z_COLOR_BG)},
        {"COLOR_SURFACE", color_pack(Z_COLOR_SURFACE)},
        {"COLOR_SURFACE_2", color_pack(Z_COLOR_SURFACE_2)},
        {"COLOR_SURFACE_3", color_pack(Z_COLOR_SURFACE_3)},
        {"COLOR_BORDER", color_pack(Z_COLOR_BORDER)},
        {"COLOR_PRIMARY", color_pack(Z_COLOR_PRIMARY)},
        {"COLOR_ACCENT", color_pack(Z_COLOR_ACCENT)},
        {"COLOR_TEXT", color_pack(Z_COLOR_TEXT)},
        {"COLOR_TEXT_MUTED", color_pack(Z_COLOR_TEXT_MUTED)},
        {"COLOR_TEXT_FAINT", color_pack(Z_COLOR_TEXT_FAINT)},
        {"COLOR_TEXT_INV", color_pack(Z_COLOR_TEXT_INV)},
        {"COLOR_SUCCESS", color_pack(Z_COLOR_SUCCESS)},
        {"COLOR_WARN", color_pack(Z_COLOR_WARN)},
        {"COLOR_DANGER", color_pack(Z_COLOR_DANGER)},
        {"COLOR_SUCCESS_DIM", color_pack(Z_COLOR_SUCCESS_DIM)},
        {"COLOR_WARN_DIM", color_pack(Z_COLOR_WARN_DIM)},
        {"COLOR_DANGER_DIM", color_pack(Z_COLOR_DANGER_DIM)},
        {"COLOR_ACCENT_DIM", color_pack(Z_COLOR_ACCENT_DIM)},
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        JS_SetModuleExport(ctx, m, colors[i].name,
                           JS_NewUint32(ctx, colors[i].rgba));
    }

    struct { const char *name; int32_t value; } ints[] = {
        {"AXIS_VERTICAL", Z_AXIS_VERTICAL},
        {"AXIS_HORIZONTAL", Z_AXIS_HORIZONTAL},
        {"AXIS_DEPTH", Z_AXIS_DEPTH},
        {"ALIGN_LEADING", Z_ALIGN_LEADING},
        {"ALIGN_CENTER", Z_ALIGN_CENTER},
        {"ALIGN_TRAILING", Z_ALIGN_TRAILING},
        {"FONT_CAPTION2", Z_FONT_CAPTION2},
        {"FONT_CAPTION", Z_FONT_CAPTION},
        {"FONT_FOOTNOTE", Z_FONT_FOOTNOTE},
        {"FONT_SUBHEAD", Z_FONT_SUBHEAD},
        {"FONT_BODY", Z_FONT_BODY},
        {"FONT_HEADLINE", Z_FONT_HEADLINE},
        {"FONT_CALLOUT", Z_FONT_CALLOUT},
        {"FONT_TITLE2", Z_FONT_TITLE2},
        {"FONT_TITLE", Z_FONT_TITLE},
        {"FONT_LARGE_TITLE", Z_FONT_LARGE_TITLE},
        {"WEIGHT_REGULAR", Z_WEIGHT_REGULAR},
        {"WEIGHT_MEDIUM", Z_WEIGHT_MEDIUM},
        {"WEIGHT_SEMIBOLD", Z_WEIGHT_SEMIBOLD},
        {"WEIGHT_BOLD", Z_WEIGHT_BOLD},
        // Motion tokens (P31): a script picks a spring by ROLE, like a C app.
        {"SPRING_STANDARD", Z_SPRING_STANDARD},
        {"SPRING_SNAPPY", Z_SPRING_SNAPPY},
        {"SPRING_PRESS", Z_SPRING_PRESS},
        {"PAN_BEGIN", Z_PAN_BEGIN},
        {"PAN_CHANGED", Z_PAN_CHANGED},
        {"PAN_END", Z_PAN_END},
        {"PERM_GRANTED", Z_PERM_GRANTED},
        {"PERM_DENIED", Z_PERM_DENIED},
        {"PERM_PROMPT", Z_PERM_PROMPT},
        {"IMPORTANCE_MIN", Z_IMPORTANCE_MIN},
        {"IMPORTANCE_LOW", Z_IMPORTANCE_LOW},
        {"IMPORTANCE_DEFAULT", Z_IMPORTANCE_DEFAULT},
        {"IMPORTANCE_HIGH", Z_IMPORTANCE_HIGH},
    };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
        JS_SetModuleExport(ctx, m, ints[i].name, JS_NewInt32(ctx, ints[i].value));
    }

    struct { const char *name; double value; } floats[] = {
        {"ELEV_1", Z_ELEV_1},
        {"ELEV_2", Z_ELEV_2},
        {"ELEV_3", Z_ELEV_3},
    };
    for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); i++) {
        JS_SetModuleExport(ctx, m, floats[i].name,
                           JS_NewFloat64(ctx, floats[i].value));
    }
}

static const char *const zs_token_names[] = {
    "COLOR_BG", "COLOR_SURFACE", "COLOR_SURFACE_2", "COLOR_SURFACE_3",
    "COLOR_BORDER", "COLOR_PRIMARY", "COLOR_ACCENT", "COLOR_TEXT",
    "COLOR_TEXT_MUTED", "COLOR_TEXT_FAINT", "COLOR_TEXT_INV", "COLOR_SUCCESS",
    "COLOR_WARN", "COLOR_DANGER", "COLOR_SUCCESS_DIM", "COLOR_WARN_DIM",
    "COLOR_DANGER_DIM", "COLOR_ACCENT_DIM",
    "AXIS_VERTICAL", "AXIS_HORIZONTAL", "AXIS_DEPTH",
    "ALIGN_LEADING", "ALIGN_CENTER", "ALIGN_TRAILING",
    "FONT_CAPTION2", "FONT_CAPTION", "FONT_FOOTNOTE", "FONT_SUBHEAD",
    "FONT_BODY", "FONT_HEADLINE", "FONT_CALLOUT", "FONT_TITLE2", "FONT_TITLE",
    "FONT_LARGE_TITLE",
    "WEIGHT_REGULAR", "WEIGHT_MEDIUM", "WEIGHT_SEMIBOLD", "WEIGHT_BOLD",
    "ELEV_1", "ELEV_2", "ELEV_3",
    "SPRING_STANDARD", "SPRING_SNAPPY", "SPRING_PRESS",
    "PAN_BEGIN", "PAN_CHANGED", "PAN_END",
    "PERM_GRANTED", "PERM_DENIED", "PERM_PROMPT",
    "IMPORTANCE_MIN", "IMPORTANCE_LOW", "IMPORTANCE_DEFAULT", "IMPORTANCE_HIGH",
};

#define ZS_NATIVE_FUNC_COUNT \
    ((int)(sizeof(zs_native_funcs) / sizeof(zs_native_funcs[0])))

// The module is assembled from BOTH binding files: the view/gesture half here and
// the system-API half in sys.c. One module, two subjects.
static int native_module_init(JSContext *ctx, JSModuleDef *m) {
    JS_SetModuleExportList(ctx, m, zs_native_funcs, ZS_NATIVE_FUNC_COUNT);
    JS_SetModuleExportList(ctx, m, zs_sys_funcs, zs_sys_funcs_count);
    JS_SetModuleExportList(ctx, m, zs_sensor_funcs, zs_sensor_funcs_count);
    export_tokens(ctx, m);
    return 0;
}

void zs_init_native_module(JSContext *ctx) {
    JS_NewClassID(&zs_view_class_id);
    JS_NewClass(JS_GetRuntime(ctx), zs_view_class_id, &zs_view_class);
    JS_NewClassID(&zs_anim_class_id);
    JS_NewClass(JS_GetRuntime(ctx), zs_anim_class_id, &zs_anim_class);

    JSModuleDef *m = JS_NewCModule(ctx, "zelto:native", native_module_init);
    if (!m) { return; }
    JS_AddModuleExportList(ctx, m, zs_native_funcs, ZS_NATIVE_FUNC_COUNT);
    JS_AddModuleExportList(ctx, m, zs_sys_funcs, zs_sys_funcs_count);
    JS_AddModuleExportList(ctx, m, zs_sensor_funcs, zs_sensor_funcs_count);
    for (size_t i = 0; i < sizeof(zs_token_names) / sizeof(zs_token_names[0]); i++) {
        JS_AddModuleExport(ctx, m, zs_token_names[i]);
    }
}
