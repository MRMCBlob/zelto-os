// libzelto navigation: a stack of screens with an animated slide transition.
// The Navigator view rebuilds the top screen (and, while a transition is in
// flight, the one below it), offsetting each horizontally by its transition
// progress so a push slides the new screen in from the right and a pop slides it
// back out. Progress is a spring (animation.c) advanced on the frame callback.
// Back is the Escape/Backspace key or an edge-swipe, both routed by app.c to
// z_nav_pop. See docs/guides/navigation.md.
#include <string.h>

#include "internal.h"

extern ZArena *z_build_arena;

static ZView new_node(ZKind kind) {
    struct ZNode *n = z_arena_alloc(z_build_arena, sizeof(*n));
    n->kind = kind;
    n->fg = Z_COLOR_TEXT;
    n->font_size = (float)Z_FONT_BODY;
    return n;
}

ZNav *z_navigation(ZApp *app) { return &z_app_ui(app)->nav; }

void z_nav_push(ZNav *nav, ZScreenFn screen, void *props) {
    if (!nav || nav->depth >= Z_MAX_SCREENS) {
        return;
    }
    ZScreen *s = &nav->stack[nav->depth++];
    memset(s, 0, sizeof(*s));
    s->fn = screen;
    s->props = props;
    s->op = 1;                 // entering
    s->trans.app = nav->app;
    s->trans.used = true;
    s->trans.value = 0.0f;     // start off-screen (right)
    // Push/pop ride the STANDARD motion token explicitly (P32), so screen slides
    // move in the same language as sheets and drawers regardless of any ambient
    // z_with_animation profile. Reduce Motion collapses it to an instant swap.
    z_animated_spring_with(&s->trans, 1.0f, Z_SPRING_STANDARD);
}

void z_nav_pop(ZNav *nav) {
    if (!nav || nav->depth <= 1) {
        return;
    }
    ZScreen *top = &nav->stack[nav->depth - 1];
    top->op = 2;               // exiting
    top->trans.app = nav->app;
    z_animated_spring_with(&top->trans, 0.0f, Z_SPRING_STANDARD);
}

void z_nav_freeze_top(ZNav *nav, float progress) {
    if (!nav || nav->depth < 1) {
        return;
    }
    ZScreen *top = &nav->stack[nav->depth - 1];
    top->trans.value = top->trans.target = progress;
    top->trans.velocity = 0.0f;
    top->trans.animating = false;   // frozen mid-transition for a still shot
}

ZView z_navigator(ZApp *app, const ZNavOpts *opts) {
    ZUI *ui = z_app_ui(app);
    ZNav *nav = &ui->nav;
    ui->nav_used = true;

    if (!nav->inited) {
        nav->inited = true;
        nav->app = app;
        nav->depth = 1;
        ZScreen *root = &nav->stack[0];
        memset(root, 0, sizeof(*root));
        root->fn = opts->root;
        root->props = NULL;
        root->op = 0;
        root->trans.app = app;
        root->trans.used = true;
        root->trans.value = root->trans.target = 1.0f;  // root is fully present
    }

    // Retire a popped screen once it has slid fully off.
    ZScreen *cur_top = &nav->stack[nav->depth - 1];
    if (nav->depth > 1 && cur_top->op == 2 && !cur_top->trans.animating &&
        z_animated_get(&cur_top->trans) <= 0.001f) {
        memset(cur_top, 0, sizeof(*cur_top));
        nav->depth--;
    }

    float W = (float)z_app_width(app);
    ZScreen *top = &nav->stack[nav->depth - 1];
    float top_p = z_animated_get(&top->trans);
    bool transitioning = top->trans.animating || top_p < 0.999f;
    ui->transitioning = transitioning;  // the app loop forces a full repaint while sliding

    ZView container = new_node(Z_K_STACK);
    container->axis = Z_AXIS_DEPTH;

    int lo = (transitioning && nav->depth >= 2) ? nav->depth - 2 : nav->depth - 1;
    for (int i = lo; i < nav->depth; i++) {
        ZScreen *s = &nav->stack[i];
        ZScreen *prev = ui->cur;
        ui->cur = s;
        s->anim_cursor = 0;
        s->scroll_cursor = 0;
        z_keyed_frame_begin(s);
        ZView v = s->fn(app, s->props);
        z_keyed_frame_end(s);
        ui->cur = prev;

        v->fill = true;
        if (i == nav->depth - 1) {
            v->off_x += (1.0f - top_p) * W;        // top: slide in from the right
            // Coordinated cross-fade (P32): the entering/leaving screen also fades
            // with its slide progress (fade = 1 - fill at rest), so a push reads as
            // the new screen materialising rather than only sliding. (fade stored
            // as 1 - opacity; top_p 1 at rest -> fade 0 -> fully opaque.)
            v->fade = 1.0f - top_p;
        } else {
            v->off_x += -top_p * W * 0.25f;        // below: subtle parallax left
        }
        if (container->n_children < Z_MAX_CHILDREN) {
            container->children[container->n_children++] = v;
        }
    }
    return container;
}
