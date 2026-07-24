// libzelto animation: spring-backed animated values and the per-frame tick that
// advances them. Springs do not run on their own thread (the render-thread split
// is documented but out of scope) — instead the app loop calls z_anim_tick once
// per frame off the wl_surface frame callback, so motion is continuous while any
// value is in flight and the loop idles once everything has settled.
// See docs/guides/animation.md and docs/contributing/sdk-internals.md.
#include <math.h>
#include <time.h>

#include "internal.h"

// --- The spring vocabulary, in the two numbers that describe it (P52) -------
//
// These were hand-tuned stiffness/damping pairs, which are the wrong units to
// think in: nobody can look at `k = 300, c = 30` and say how long the motion
// takes or whether it overshoots. Apple's spring API names the two things that
// actually matter — RESPONSE (how long the motion takes) and BOUNCE (how much it
// overshoots, where bounce = 1 - dampingFraction) — and derives the physics.
// Same move as Z_PT and z_font_units: author in the units the value is reasoned
// about in, convert at one seam.
//
//     omega = 2*pi / response      k = m * omega^2
//     zeta  = 1 - bounce           c = 2 * zeta * sqrt(k * m)
//
// THE REFACTOR IS VALUE-NEUTRAL, WHICH IS THE FINDING. Converting the shipped
// constants back through those formulas lands within a few percent of numbers a
// designer would have chosen deliberately:
//
//     SNAPPY  k 300 c 30  ->  response 0.36, bounce 0.15   (derived: 304.6, 29.67)
//     PRESS   k 520 c 34  ->  response 0.28, bounce 0.25   (derived: 503.6, 33.66)
//
// So the hand-tuning had been consistent with this model all along; it just could
// not be read. STANDARD is the one that moves at all: its true response was
// 0.482s and its damping ratio 0.997 — a hair off the critical damping it was
// meant to be. It is exactly 1.0 now, and 0.5s, which is Apple's `.smooth`.
//
// THE DURATIONS DIVERGE FROM iOS DELIBERATELY. Apple gives all three of
// smooth/snappy/bouncy the SAME 0.5s and lets bounce be the only axis. Zelto
// keeps SNAPPY at 0.36s and PRESS at 0.28s, because those two exist for moments
// where the motion is answering the finger rather than presenting a surface — a
// carousel page flip and a touch-down highlight. A press veil that takes half a
// second to appear reads as lag, not as polish. STANDARD, which IS the
// "presenting a surface" case, matches Apple exactly.
typedef struct SpringSpec {
    float response;   // seconds for the motion to make its move
    float bounce;     // 0 = critically damped, higher = more overshoot
} SpringSpec;

static SpringSpec spring_spec(ZSpring s) {
    switch (s) {
    case Z_SPRING_PRESS:
        // Tight + fast so touch-down registers immediately and release snaps back.
        return (SpringSpec){0.28f, 0.25f};
    case Z_SPRING_SNAPPY:
        return (SpringSpec){0.36f, 0.15f};
    case Z_SPRING_STANDARD:
    default:
        return (SpringSpec){0.50f, 0.0f};   // Apple's .smooth
    }
}

static void spring_params(ZSpring s, float *k, float *c, float *m) {
    SpringSpec spec = spring_spec(s);
    float omega = 6.2831853f / spec.response;
    float zeta = 1.0f - spec.bounce;
    *m = 1.0f;
    *k = omega * omega * *m;
    *c = 2.0f * zeta * sqrtf(*k * *m);
}

double z_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// --- animated value -------------------------------------------------------
ZAnimated *z_animated_value(ZApp *app, float initial) {
    ZUI *ui = z_app_ui(app);
    ZScreen *s = ui->cur;
    int i = s->anim_cursor++;
    if (i >= Z_MAX_CELLS) {
        i = Z_MAX_CELLS - 1;  // clamp: shared cell, but never out of bounds
    }
    ZAnimated *v = &s->anims[i];
    if (!v->used) {
        v->used = true;
        v->value = v->target = initial;
        v->velocity = 0.0f;
        v->animating = false;
        spring_params(Z_SPRING_STANDARD, &v->stiffness, &v->damping, &v->mass);
        if (s->anim_count <= i) {
            s->anim_count = i + 1;
        }
    }
    v->app = app;
    return v;
}

// --- keyed animated value (identity-keyed retained cell) ------------------
// Looked up by an explicit key in the current screen's keyed table, so a value's
// identity survives rebuilds even when its call-order position changes. Marks the
// cell requested for this build's GC sweep; allocates (reusing a swept slot) on a
// key miss. This is the ONLY correct way to give each reflowing item its own
// spring across reorders (the call-order trap, writ large).
ZAnimated *z_animated_keyed(ZApp *app, uint64_t key, float initial) {
    ZUI *ui = z_app_ui(app);
    ZScreen *s = ui->cur;
    for (int i = 0; i < s->keyed_count; i++) {
        if (s->keyed[i].used && s->keyed[i].key == key) {
            s->keyed[i].requested = true;
            s->keyed[i].v.app = app;
            return &s->keyed[i].v;
        }
    }
    int slot = -1;
    for (int i = 0; i < s->keyed_count; i++) {
        if (!s->keyed[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s->keyed_count < Z_MAX_KEYED) {
            slot = s->keyed_count++;
        } else {
            slot = Z_MAX_KEYED - 1;  // clamp: shared cell, but never out of bounds
        }
    }
    ZKeyed *kc = &s->keyed[slot];
    kc->key = key;
    kc->used = true;
    kc->requested = true;
    ZAnimated *v = &kc->v;
    v->value = v->target = initial;
    v->velocity = 0.0f;
    v->animating = false;
    spring_params(Z_SPRING_STANDARD, &v->stiffness, &v->damping, &v->mass);
    v->app = app;
    return v;
}

void z_keyed_frame_begin(ZScreen *s) {
    for (int i = 0; i < s->keyed_count; i++) {
        s->keyed[i].requested = false;
    }
}

void z_keyed_frame_end(ZScreen *s) {
    for (int i = 0; i < s->keyed_count; i++) {
        if (s->keyed[i].used && !s->keyed[i].requested) {
            s->keyed[i].used = false;
        }
    }
}

bool z_animated_active(const ZAnimated *v) { return v->animating; }

float z_animated_target(const ZAnimated *v) { return v->target; }

void z_animated_set(ZAnimated *v, float to) {
    v->value = v->target = to;
    v->velocity = 0.0f;
    v->animating = false;
    if (v->app) {
        z_invalidate(v->app);
    }
}

// Like z_animated_set, but does NOT wake the loop. For a value written fresh on
// every build (e.g. a ghost pinned to the live finger position, or a test freeze):
// the caller already invalidated for its own reason, so self-invalidating here
// would spin a full-speed repaint loop.
void z_animated_pin(ZAnimated *v, float to) {
    v->value = v->target = to;
    v->velocity = 0.0f;
    v->animating = false;
}

void z_animated_spring(ZAnimated *v, float to) {
    ZUI *ui = v->app ? z_app_ui(v->app) : NULL;
    // Reduce Motion (Accessibility): collapse every spring to an instant jump, so
    // sheets/reflow/press all snap rather than glide. The target still lands in the
    // same place, so nothing else in the UI needs to know motion was suppressed.
    if (ui && ui->reduce_motion) {
        z_animated_set(v, to);
        return;
    }
    spring_params(ui ? ui->anim_spring : Z_SPRING_STANDARD, &v->stiffness,
                  &v->damping, &v->mass);
    v->target = to;
    v->animating = true;
    if (v->app) {
        z_invalidate(v->app);  // wake the loop so the tick starts integrating
    }
}

// Spring a value under an EXPLICIT profile, independent of the ambient
// z_with_animation profile (ui->anim_spring). The press-feedback spring uses this
// so touch-down always moves with the PRESS token no matter what an app set. Honours
// Reduce Motion like z_animated_spring.
void z_animated_spring_with(ZAnimated *v, float to, ZSpring spring) {
    ZUI *ui = v->app ? z_app_ui(v->app) : NULL;
    if (ui && ui->reduce_motion) {
        z_animated_set(v, to);
        return;
    }
    spring_params(spring, &v->stiffness, &v->damping, &v->mass);
    v->target = to;
    v->animating = true;
    if (v->app) {
        z_invalidate(v->app);
    }
}

// Spring toward `to` with an EXPLICIT initial velocity injected into the
// integrator — the continuous hand-off a gesture release needs. When a finger
// lets go of a surface it was dragging 1:1, the surface should re-fling from the
// finger's live velocity, not ease from a standstill; and touching a spring
// mid-flight then releasing should carry the value's current velocity forward.
// This is the one entry point that seeds v->velocity rather than leaving it. Same
// Reduce-Motion collapse (an instant jump, no fling) as the other spring starters.
void z_animated_spring_velocity(ZAnimated *v, float to, ZSpring spring,
                                float velocity) {
    ZUI *ui = v->app ? z_app_ui(v->app) : NULL;
    if (ui && ui->reduce_motion) {
        z_animated_set(v, to);
        return;
    }
    spring_params(spring, &v->stiffness, &v->damping, &v->mass);
    v->target = to;
    v->velocity = velocity;   // seed the release velocity for a fluid hand-off
    v->animating = true;
    if (v->app) {
        z_invalidate(v->app);
    }
}

// Grab a spring that may be mid-flight: freeze it at its CURRENT value and return
// that value, so a finger touching a moving surface takes control from where it
// is (no jump, no ignored touch) and drives it 1:1 from here. The velocity is
// retained (not zeroed) so a later z_animated_spring_velocity can carry it — but
// the value stops evolving on its own until the drag sets it. Does not wake the
// loop (the caller is handling live input and will invalidate).
float z_animated_grab(ZAnimated *v) {
    v->target = v->value;
    v->animating = false;
    return v->value;
}

float z_animated_get(const ZAnimated *v) { return v->value; }

void z_with_animation(ZApp *app, ZSpring spring, ZAction change) {
    ZUI *ui = z_app_ui(app);
    ZSpring prev = ui->anim_spring;
    ui->anim_spring = spring;          // springs started in `change` use this profile
    change(app, z_app_state(app));
    ui->anim_spring = prev;
    z_invalidate(app);
}

// --- per-frame integration ------------------------------------------------
// Semi-implicit Euler with sub-stepping for stability at large dt.
static bool advance_spring(ZAnimated *v, float dt) {
    if (!v->animating) {
        return false;
    }
    const float eps_x = 0.25f, eps_v = 0.5f;
    float steps = ceilf(dt / 0.008f);
    if (steps < 1.0f) {
        steps = 1.0f;
    }
    float h = dt / steps;
    for (int i = 0; i < (int)steps; i++) {
        float a = (-v->stiffness * (v->value - v->target) - v->damping * v->velocity)
                  / v->mass;
        v->velocity += a * h;
        v->value += v->velocity * h;
    }
    if (fabsf(v->value - v->target) < eps_x && fabsf(v->velocity) < eps_v) {
        v->value = v->target;
        v->velocity = 0.0f;
        v->animating = false;
        return false;
    }
    return true;
}

// Fling integration: exponential-decay velocity, clamped to the content range,
// with a soft stop at the edges.
static bool advance_fling(ZScroll *sc, float dt) {
    if (!sc->flinging) {
        return false;
    }
    float max = sc->content_h - sc->viewport_h;
    if (max < 0.0f) {
        max = 0.0f;
    }
    // Friction: lose ~5%/ms -> decay factor per frame.
    float decay = expf(-6.0f * dt);
    sc->offset += sc->velocity * dt;
    sc->velocity *= decay;
    bool settled = false;
    if (sc->offset < 0.0f) {
        sc->offset = 0.0f;
        sc->velocity = 0.0f;
        settled = true;
    } else if (sc->offset > max) {
        sc->offset = max;
        sc->velocity = 0.0f;
        settled = true;
    }
    if (fabsf(sc->velocity) < 6.0f) {
        settled = true;
    }
    if (settled) {
        sc->flinging = false;
        sc->velocity = 0.0f;
        sc->raw = sc->offset;   // keep the un-damped drag base in sync for the wheel
        return false;
    }
    return true;
}

// Elastic snap-back from an over-pull (P33): after a rubber-banded drag is released
// past a bound, ease the offset to the nearest edge. Exponential approach — no
// velocity term needed (the release into the wall has none), and it reads as the
// standard rubber-band recoil. Reduce Motion never reaches here: an over-pull under
// it can't happen because the drag itself is 1:1 but the release just clamps below.
static bool advance_settle(ZScroll *sc, float dt) {
    if (!sc->settling) {
        return false;
    }
    float a = 1.0f - expf(-14.0f * dt);
    sc->offset += (sc->settle_target - sc->offset) * a;
    if (fabsf(sc->offset - sc->settle_target) < 0.5f) {
        sc->offset = sc->settle_target;
        sc->raw = sc->offset;
        sc->settling = false;
        return false;
    }
    return true;
}

static bool tick_screen(ZScreen *s, float dt) {
    bool active = false;
    for (int i = 0; i < s->anim_count; i++) {
        if (advance_spring(&s->anims[i], dt)) {
            active = true;
        }
    }
    for (int i = 0; i < s->scroll_count; i++) {
        if (advance_fling(&s->scrolls[i], dt)) {
            active = true;
        }
        if (advance_settle(&s->scrolls[i], dt)) {
            active = true;
        }
    }
    for (int i = 0; i < s->keyed_count; i++) {
        if (s->keyed[i].used && advance_spring(&s->keyed[i].v, dt)) {
            active = true;
        }
    }
    if (advance_spring(&s->trans, dt)) {
        active = true;
    }
    return active;
}

bool z_anim_tick(ZApp *app, float dt) {
    ZUI *ui = z_app_ui(app);
    bool active = tick_screen(&ui->implicit, dt);
    // The global press-feedback spring rides alongside the per-screen cells.
    if (advance_spring(&ui->press, dt)) {
        active = true;
    }
    for (int i = 0; i < ui->nav.depth; i++) {
        if (tick_screen(&ui->nav.stack[i], dt)) {
            active = true;
        }
    }
    return active;
}
