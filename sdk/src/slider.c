// libzelto Slider — a continuous control you DRAG.
//
// Until P42 the toolkit had no slider, and the two places in the OS that most
// need one had both routed around it: Settings drove brightness with a [-|+]
// STEPPER, and the Control Center drove brightness and volume with round on/off
// TOGGLES. Both are buttons. Brightness and volume are the two controls iOS puts
// at the top of its Control Center, and neither of them is a button, because
// neither is a decision — you push them until it looks or sounds right, watching
// the thing change as you go. A stepper makes that five taps and a toggle makes
// it impossible.
//
// TWO SHAPES, ONE CONTROL (ZSliderOpts.tall):
//   - the classic track: a thin rounded rail, filled up to the value, with a
//     round knob riding it. This is the list-row shape — it sits in the control
//     column of a Settings row next to switches and steppers.
//   - the TALL shape: a wide rounded slab whose FILL IS the value, dragged
//     vertically, with a glyph at the foot. This is what iOS's Control Center
//     uses, and the reason is that it is a thumb target the size of the control
//     itself — you grab it anywhere and push, without aiming at a knob.
//
// DRAGGING IS RELATIVE, NOT ABSOLUTE. The value moves by how far the finger
// moved from where it went down, starting from the value at that moment — it
// does not jump to wherever you touched. Jump-to-touch means brushing the
// control slams the screen to full brightness, and it makes a big target hostile
// precisely because it is big. This is also why the value at the gesture's start
// is captured in the ZSlider rather than recomputed: the tree is rebuilt every
// frame mid-drag, so anything derived per-frame would drift.
#include <stdlib.h>

#include "internal.h"

// Where the pan handler stashes the value the drag started from. Kept on the
// caller's ZSlider (which is retained app state) rather than in a keyed cell,
// because it must survive the rebuilds that happen during the drag itself.
static void slider_pan(ZApp *app, void *state, void *data,
                       const ZPanEvent *e) {
    ZSlider *s = data;
    if (!s) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->drag_base = s->value;
        s->dragging = true;
        return;
    }
    if (!s->dragging) {
        return;
    }
    float travel = s->travel > 1.0f ? s->travel : 1.0f;
    // Vertical sliders grow UPWARD: dragging toward the top of the screen (a
    // negative y translation) must raise the value, hence the sign flip.
    float delta = s->vertical ? -e->translation_y / travel
                              : e->translation_x / travel;
    float v = s->drag_base + delta;
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    if (v != s->value) {
        s->value = v;
        if (s->on_change) {
            s->on_change(app, state, v);
        }
        z_invalidate(app);
    }
    if (e->phase == Z_PAN_END) {
        s->dragging = false;
        if (s->on_commit) {
            s->on_commit(app, state, s->value);
        }
    }
}

ZView z_slider(ZApp *app, ZSlider *s, const ZSliderOpts *opts) {
    (void)app;
    if (!s || !opts) {
        return Spacer();
    }
    float v = s->value < 0.0f ? 0.0f : (s->value > 1.0f ? 1.0f : s->value);
    s->vertical = opts->tall;

    if (opts->tall) {
        // --- Control Center shape -------------------------------------------
        float w = opts->thickness > 0.0f ? opts->thickness : 150.0f;
        float h = opts->length > 0.0f ? opts->length : 320.0f;
        s->travel = h;

        // The fill is a rounded rect anchored at the FOOT of the slab, grown to
        // the value. It carries the slab's own radius so its head stays a
        // continuous curve rather than a hard edge cutting across the slab.
        float fill_h = h * v;
        float radius = Z_RADIUS_PANEL;
        // Grow(1, Spacer()), not Fill(Spacer()): in a vertical stack a filling
        // spacer takes the WHOLE height and the fill below it gets none.
        ZView stack = VStack(
            Grow(1.0f, Spacer()),
            Frame(w, fill_h,
                Rect(.color = Z_COLOR_PRIMARY, .radius = radius)),
            .spacing = 0, .align = Z_ALIGN_CENTER);

        // The glyph sits at the foot, over whichever of track/fill reaches it.
        ZView foot = opts->glyph
                         ? VStack(Grow(1.0f, Spacer()),
                                  Frame(w, 56.0f,
                                        ZStack(opts->glyph,
                                               .align = Z_ALIGN_CENTER)),
                                  .spacing = 0, .align = Z_ALIGN_CENTER)
                         : Spacer();

        return OnPanData(slider_pan, s,
            Frame(w, h,
                Background(Z_COLOR_SURFACE_3,
                    CornerRadius(radius,
                        ZStack(stack, foot, .align = Z_ALIGN_CENTER)))));
    }

    // --- list-row shape ------------------------------------------------------
    float w = opts->length > 0.0f ? opts->length : 200.0f;
    float t = opts->thickness > 0.0f ? opts->thickness : 6.0f;
    float knob = t * 4.0f;
    // The knob's centre travels the track MINUS its own width, so it stops flush
    // with each end instead of hanging off.
    float travel = w - knob;
    s->travel = travel;
    float knob_x = -travel * 0.5f + travel * v;

    ZView track = Frame(w, t, Rect(.color = Z_COLOR_SURFACE_3, .radius = t * 0.5f));
    // The filled portion is drawn from the leading edge; it is its own rect
    // rather than a clipped child because the toolkit's CornerRadius rounds a
    // node's own paint and does not clip a subtree.
    ZView fill = HStack(
        Frame(knob * 0.5f + travel * v, t,
              Rect(.color = Z_COLOR_PRIMARY, .radius = t * 0.5f)),
        Grow(1.0f, Spacer()),
        .spacing = 0, .align = Z_ALIGN_CENTER);

    ZView thumb = OffsetXY(knob_x, 0.0f,
        Shadow(Z_ELEV_1,
            Frame(knob, knob,
                Rect(.color = Z_COLOR_TEXT, .radius = knob * 0.5f))));

    // The hit area is the full knob height, not the hairline track: a 6px-tall
    // target is not a target.
    return OnPanData(slider_pan, s,
        Frame(w, knob,
            ZStack(track, fill, thumb, .align = Z_ALIGN_CENTER)));
}
