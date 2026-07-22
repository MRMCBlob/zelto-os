// Zelto System UI — the share sheet.
//
// A modal shown by zsysd when an intent (a share, or a multi-handler deep link)
// needs the user to say where the content goes. zsysd fork/execs it with argv =
// the candidate app_ids (one per target); the user picks one; the process exits
// with that target's 1-based index, which zsysd maps back to the app_id. A cancel
// (or no pick) exits 0. The exit code is the whole IPC — no result socket,
// exactly like the consent dialog. See docs/platform/ipc-and-intents.md.
//
// WHY A BOTTOM SHEET AND NOT A CENTRED CARD. This used to be a centred modal card
// listing "os.zelto.notes", "os.zelto.store" as text rows over a dim backdrop —
// the shape of a desktop "Open with..." dialog. Three things were wrong with it:
//
//   1. It printed reverse-DNS IDS. A share sheet exists to answer "where is this
//      going?", and answering it with a database key is answering a different
//      question. Targets are shown as their ICON and their display NAME now, the
//      same marks they wear on the home screen, because recognising the app you
//      want is a visual act and not a reading one.
//   2. It never showed WHAT was being shared. You were asked to confirm a
//      destination for content you could no longer see. The sheet now previews the
//      payload at the top (zsysd passes it in the environment — see the note on
//      ZELTO_SHARE_* below), so the question is complete.
//   3. It was centred, so the targets sat in the middle of a phone screen and the
//      thumb had to travel up to them. A sheet rises from the bottom edge, where
//      the thumb already is, and it comes from the direction the gesture that
//      dismisses it goes.
//
// The targets are a HORIZONTAL row, not a vertical list: a target is an icon, and
// icons in a row are scanned in one sweep, where a vertical list of icon+name
// rows makes each one a separate stop. Actions (things that are not a
// destination) go BELOW the row, in the grouped list shape the rest of the OS
// uses, because they are a different kind of answer.
//
// THE PAYLOAD IS PASSED IN THE ENVIRONMENT, NOT IN ARGV. argv is the candidate
// list and its INDICES are the IPC — the exit code means "argv[N]". Threading a
// preview through argv would mean an offset that both sides have to agree on, and
// getting it wrong delivers the share to the wrong app. zsysd setenv()s
// ZELTO_SHARE_MIME / ZELTO_SHARE_PAYLOAD in the forked child instead, so the
// index mapping is untouched and an unset variable simply means no preview.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

// Relative, not "common/app_icons.h" on a system/ include path: this target has
// no extra include_directories and does not need one for a single header.
#include "../common/app_icons.h"

#define MAX_CANDIDATES 24

// Sheet geometry. The sheet is full-width and sized to its CONTENT — a share
// sheet with three targets should not be as tall as one with twelve, and a
// fixed-height sheet with empty material under the last row reads as a panel that
// failed to load.
#define SHEET_PAD 20.0f
#define SHEET_ROW_H 60.0f
#define TARGET_ICON 68.0f
#define TARGET_CELL 96.0f    // the fixed column a target occupies (icon + name)
#define TARGET_MAX 6         // targets shown before the row would overflow 720px
#define PREVIEW_ICON 52.0f
// Drag-to-dismiss: a downward drag past DISMISS_THRESH px (or a firm down-fling)
// cancels, the way a sheet is dismissed everywhere else on the phone.
#define DISMISS_THRESH 90.0f
#define DISMISS_DIST 260.0f

typedef struct ChooserState {
    char **ids;   // candidate app_ids (argv tail)
    int n;        // candidate count

    ZAnimated *enter;   // 0 -> 1 sheet rise + backdrop fade
    ZAnimated *drag;    // live downward drag toward dismissal
    bool armed;
    bool dragging;
} ChooserState;

// A row resolves the sheet by the process exit code zsysd reads: its 1-based
// index. The index is carried as the per-row data pointer (boxed as an int).
static void on_pick(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    exit((int)(intptr_t)data);
}
static void on_cancel(ZApp *app, void *state) {
    (void)app;
    (void)state;
    exit(0);
}

// Drag the sheet down to cancel. Like the consent modal's flick-to-deny, the
// resolution is IMMEDIATE (exit) with no exit animation: zsysd is blocked in
// waitpid on this process, so anything we do after the decision is time the
// sharing app spends frozen.
static void on_sheet_pan(ZApp *app, void *state, const ZPanEvent *e) {
    ChooserState *s = state;
    if (!s->drag) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        s->dragging = true;
        z_animated_grab(s->drag);
    } else if (e->phase == Z_PAN_CHANGED) {
        // Downward only; an upward drag resists (there is nothing above the sheet).
        float t = e->translation_y;
        z_animated_set(s->drag, t > 0.0f ? t : z_rubber_band(t, 0.4f));
    } else {   // Z_PAN_END
        s->dragging = false;
        float d = z_animated_get(s->drag);
        if (d > DISMISS_THRESH || e->velocity_y > 700.0f) {
            exit(0);   // cancelled
        }
        z_animated_spring_velocity(s->drag, 0.0f, Z_SPRING_STANDARD,
                                   e->velocity_y);
    }
    z_invalidate(app);
}

// One share target: the app's ICON over its display NAME, in a fixed-width cell.
// The cell width is FIXED, not grow-weighted: a grow row redistributes by child
// count, so a sheet offering two targets would place them at different x's than
// one offering four and the row would jump between shares of different types.
static ZView target_cell(const char *app_id, int index) {
    char ipath[256];
    const char *icon =
        (zelto_icon_for_app_id(app_id, ipath, sizeof(ipath)) &&
         z_image_loads(ipath))
            ? ipath
            : zelto_placeholder_icon();
    char name[96];
    const char *who = zelto_name_for_app_id(app_id, name, sizeof(name))
                          ? name
                          : app_id;

    return Frame(TARGET_CELL, 0.0f,
        VStack(
            // OnTap sits on the ICON, not on the icon-plus-name column: the press
            // veil is masked to the tapped node's own corner radius, so a handler
            // on the column would paint a tall rounded box over a squircle icon.
            OnTapData(on_pick, (void *)(intptr_t)index,
                Frame(TARGET_ICON, TARGET_ICON,
                    CornerRadius(TARGET_ICON * Z_RADIUS_ICON, Image(icon)))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION2, Text("%s", who))),
            .spacing = 8, .align = Z_ALIGN_CENTER));
}

// The preview of what is being shared: the payload as the headline with its MIME
// type under it, behind a generic document mark. Without ZELTO_SHARE_PAYLOAD (an
// older zsysd, or a deep link rather than a share) it degrades to the action.
static ZView preview_row(void) {
    const char *payload = getenv("ZELTO_SHARE_PAYLOAD");
    const char *mime = getenv("ZELTO_SHARE_MIME");
    bool have = payload && payload[0];

    return HStack(
        Frame(PREVIEW_ICON, PREVIEW_ICON,
            CornerRadius(PREVIEW_ICON * Z_RADIUS_ICON,
                Background(Z_COLOR_SURFACE_3,
                    ZStack(
                        Foreground(Z_COLOR_TEXT,
                            Weight(Z_WEIGHT_SEMIBOLD,
                                Font(Z_FONT_CALLOUT, Text("\xe2\x86\x91")))),
                        .align = Z_ALIGN_CENTER)))),
        Grow(1.0f,
            VStack(
                Weight(Z_WEIGHT_SEMIBOLD,
                    Foreground(Z_COLOR_TEXT,
                        Font(Z_FONT_HEADLINE,
                             Text("%s", have ? payload : "Share")))),
                Foreground(Z_COLOR_TEXT_MUTED,
                    Font(Z_FONT_SUBHEAD,
                         Text("%s", (mime && mime[0]) ? mime
                                                      : "Choose a destination"))),
                .spacing = 2, .align = Z_ALIGN_LEADING)),
        .spacing = 14, .align = Z_ALIGN_CENTER);
}

// An action row: full-width, centred label, in the grouped-list card shape the
// Settings screen uses. An action is not a destination, so it does not get an
// icon in the target row.
static ZView action_row(ZAction act, const char *label) {
    return OnTap(act,
        Background(Z_COLOR_SURFACE,
            CornerRadius(Z_RADIUS_CARD,
                Frame(0.0f, SHEET_ROW_H,
                    ZStack(
                        Weight(Z_WEIGHT_SEMIBOLD,
                            Foreground(Z_COLOR_TEXT,
                                Font(Z_FONT_BODY, Text("%s", label)))),
                        .align = Z_ALIGN_CENTER)))));
}

// A fixed gap: NOT Frame(w, h, Spacer()), which keeps its grow flag.
static ZView sgap(float h) {
    return Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0)));
}

static ZView chooser_body(ZApp *app, ChooserState *s) {
    // The entrance + dismiss-drag springs, allocated first and unconditionally
    // (call-order cells) so their retained identity is stable across rebuilds.
    s->enter = z_animated_value(app, 0.0f);
    s->drag = z_animated_value(app, 0.0f);
    if (!s->armed) {
        s->armed = true;
        // ZELTO_CHOOSER_ENTER=<0..1> pins the rise mid-flight for a still shot;
        // otherwise spring it up on the first build.
        const char *en = getenv("ZELTO_CHOOSER_ENTER");
        if (en && en[0]) {
            z_animated_pin(s->enter, (float)atof(en));
        } else {
            z_animated_spring_with(s->enter, 1.0f, Z_SPRING_STANDARD);
        }
        // ZELTO_CHOOSER_DRAG=<px> pins the sheet at a held downward drag (toward
        // dismissal), seating the entrance so the mid-drag frame is verifiable.
        const char *dr = getenv("ZELTO_CHOOSER_DRAG");
        if (dr && dr[0]) {
            z_animated_set(s->enter, 1.0f);
            z_animated_pin(s->drag, (float)atof(dr));
            s->dragging = true;
        }
    }
    z_full_repaint(app);   // full-screen modal moving/fading over the app

    int n = s->n < TARGET_MAX ? s->n : TARGET_MAX;

    // --- the sheet's contents ---
    ZStackOpts col = {.padding = SHEET_PAD, .spacing = 0,
                      .align = Z_ALIGN_LEADING};
    int k = 0;
    // The grabber: the mark that says this surface is draggable, and the only
    // affordance the dismiss gesture gets. It is centred, so it is its own row.
    col.children[k++] = HStack(Spacer(),
        Rect(.color = Z_COLOR_TEXT_FAINT, .width = 44, .height = 5, .radius = 3),
        Spacer(), .spacing = 0, .align = Z_ALIGN_CENTER);
    col.children[k++] = sgap(16.0f);
    col.children[k++] = preview_row();
    col.children[k++] = sgap(16.0f);
    // The hairline. The growing Rect must sit in a HORIZONTAL stack: `.grow`
    // expands along its parent's MAIN axis, so a grow-Rect dropped straight into
    // this vertical column stretches DOWNWARD and paints a 40px grey slab where a
    // 1px rule belongs.
    col.children[k++] = Frame(0.0f, 1.0f,
        HStack(Rect(.color = Z_COLOR_MATERIAL_EDGE, .grow = 1.0f),
               .spacing = 0, .align = Z_ALIGN_CENTER));
    col.children[k++] = sgap(18.0f);

    // The target row. Cells are fixed-width and the row is LEADING-aligned, so a
    // sheet with two targets puts them where the first two of four would be.
    ZStackOpts row = {.spacing = 10.0f, .align = Z_ALIGN_LEADING};
    for (int i = 0; i < n; i++) {
        row.children[i] = target_cell(s->ids[i], i + 1);
    }
    col.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    if (s->n > n) {
        col.children[k++] = sgap(10.0f);
        col.children[k++] = Foreground(Z_COLOR_TEXT_FAINT,
            Font(Z_FONT_FOOTNOTE,
                 Text("%d more app%s can handle this", s->n - n,
                      s->n - n == 1 ? "" : "s")));
    }
    col.children[k++] = sgap(20.0f);
    col.children[k++] = action_row(on_cancel, "Cancel");
    col.children[k++] = sgap(14.0f);   // minimum clearance under the last row
    // sheet_h below is a SUM of the parts, and a sum of type metrics is never
    // exact to the pixel. Any slack lands here, at the bottom of the sheet, where
    // a few extra px of material reads as breathing room rather than as a gap in
    // the layout. (Without it the stack's spare space goes wherever a grow-flagged
    // node happens to be.)
    col.children[k++] = Grow(1.0f, Spacer());

    // --- the sheet ---
    // A MATERIAL over the app that is sharing, not an opaque slab: the compositor
    // blurs the rectangle the sheet occupies and this tints it, so you can still
    // see WHERE the content is coming from behind the question of where it goes.
    float e = z_animated_get(s->enter);
    float d = z_animated_get(s->drag);
    float sw = (float)z_app_width(app);
    float sh = (float)z_app_height(app);
    // Content height: the sheet is sized by what is in it. The rise distance is
    // that height, so the sheet starts exactly off the bottom edge whatever it
    // holds — a fixed rise would either overshoot or leave a gap on the first
    // frame. Each addend below mirrors one child above.
    float sheet_h = SHEET_PAD * 2.0f + 5.0f + 16.0f + PREVIEW_ICON + 16.0f + 1.0f +
                    18.0f + (TARGET_ICON + 8.0f + 14.0f) + 20.0f + SHEET_ROW_H +
                    14.0f;
    if (s->n > n) {
        sheet_h += 10.0f + 16.0f;
    }
    float dy = (1.0f - e) * sheet_h + (d > 0.0f ? d : 0.0f);
    float dprog = d > 0.0f ? d / DISMISS_DIST : 0.0f;
    if (dprog > 1.0f) {
        dprog = 1.0f;
    }
    z_backdrop(app, 0.0f, sh - sheet_h + dy, sw, sheet_h, Z_RADIUS_SHEET);

    ZView sheet = Shadow(Z_ELEV_3,
        Background(Z_COLOR_MATERIAL_SHEET,
            CornerRadius(Z_RADIUS_SHEET,
                Frame(sw, sheet_h, z_stack(Z_AXIS_VERTICAL, &col)))));

    // The sheet takes the dismiss drag and rides the entrance offset; the backdrop
    // behind it does NOT move — only the sheet leaves.
    ZView risen = OnPan(on_sheet_pan, OffsetXY(0.0f, dy, sheet));

    // Tapping the dim backdrop above the sheet cancels, the way it does on every
    // sheet on the phone. The whole modal fades in on `enter`.
    //
    // The backdrop is Grow(1, Spacer()) and NOT Fill(Spacer()): Fill expands a node
    // to its parent's whole inner box, which in a vertical stack means it takes the
    // full height and leaves the sheet zero — the sheet renders nowhere and all you
    // see is the dim. Grow gives it the space the sheet does not want.
    return Opacity(e,
        Background(Z_COLOR_SCRIM,
            VStack(
                Grow(1.0f, OnTap(on_cancel, Spacer())),
                Opacity(1.0f - dprog, risen),
                .spacing = 0, .align = Z_ALIGN_CENTER)));
}

static ZView body_tr(ZApp *app, void *state) {
    return chooser_body(app, (ChooserState *)state);
}

int main(int argc, char **argv) {
    static ChooserState s;
    s.ids = &argv[1];
    s.n = argc - 1;
    if (s.n > MAX_CANDIDATES) {
        s.n = MAX_CANDIDATES;
    }

    ZLayerOpts opts = {
        .layer = Z_LAYER_OVERLAY,
        .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
        .exclusive_zone = 0,
        .width = 0,
        .height = 0,
        .keyboard = true,
    };
    return z_layer_app_main(&s, body_tr, "Chooser", &opts);
}
