// libzelto animation: spring-backed animated values and the per-frame tick that
// advances them. Springs do not run on their own thread (the render-thread split
// is documented but out of scope) — instead the app loop calls z_anim_tick once
// per frame off the wl_surface frame callback, so motion is continuous while any
// value is in flight and the loop idles once everything has settled.
// See docs/guides/animation.md and docs/contributing/sdk-internals.md.
#include <math.h>
#include <time.h>

#include "internal.h"

// Spring profiles (critically-ish damped). Tuned for the design language.
static void spring_params(ZSpring s, float *k, float *c, float *m) {
    switch (s) {
    case Z_SPRING_SNAPPY:
        *k = 300.0f; *c = 30.0f; *m = 1.0f;
        break;
    case Z_SPRING_STANDARD:
    default:
        *k = 170.0f; *c = 26.0f; *m = 1.0f;
        break;
    }
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

void z_animated_set(ZAnimated *v, float to) {
    v->value = v->target = to;
    v->velocity = 0.0f;
    v->animating = false;
    if (v->app) {
        z_invalidate(v->app);
    }
}

void z_animated_spring(ZAnimated *v, float to) {
    ZUI *ui = v->app ? z_app_ui(v->app) : NULL;
    spring_params(ui ? ui->anim_spring : Z_SPRING_STANDARD, &v->stiffness,
                  &v->damping, &v->mass);
    v->target = to;
    v->animating = true;
    if (v->app) {
        z_invalidate(v->app);  // wake the loop so the tick starts integrating
    }
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
    }
    if (advance_spring(&s->trans, dt)) {
        active = true;
    }
    return active;
}

bool z_anim_tick(ZApp *app, float dt) {
    ZUI *ui = z_app_ui(app);
    bool active = tick_screen(&ui->implicit, dt);
    for (int i = 0; i < ui->nav.depth; i++) {
        if (tick_screen(&ui->nav.stack[i], dt)) {
            active = true;
        }
    }
    return active;
}
