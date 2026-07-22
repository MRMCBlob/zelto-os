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
#include <stdio.h>
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
// The gaps between the sheet's rows, named so that sheet_height() below can be an
// expression over the SAME constants the rows are built from. They were bare
// literals written out twice — once in the column and once in the sum — which is
// the shape of every reserve this project has had to go back and fix.
#define SHEET_GRAB_H 5.0f          // the drag grabber's bar
#define SHEET_PREVIEW_GAP 16.0f    // above and below the preview row
#define SHEET_RULE_H 1.0f          // the hairline
#define SHEET_RULE_GAP 18.0f       // hairline to the target row
#define SHEET_NAME_GAP 8.0f        // a target's icon to its name
#define SHEET_MORE_GAP 10.0f       // above the "N more apps" line
#define SHEET_ACTION_GAP 20.0f     // above the Cancel row
#define SHEET_FOOT 14.0f           // clearance under the last row
#define TARGET_ICON 68.0f
#define TARGET_CELL 96.0f    // the fixed column a target occupies (icon + name)
#define TARGET_MAX 6         // targets shown before the row would overflow 720px
#define PREVIEW_ICON 52.0f
#define PREVIEW_GAP 14.0f          // the mark to the text column
#define PREVIEW_LINE_GAP 2.0f      // payload line to MIME line
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
            .spacing = SHEET_NAME_GAP, .align = Z_ALIGN_CENTER));
}

// The width the preview's text column gets: the sheet less its padding, less the
// document mark and the gap after it. Needed in two places — to cut the text to
// it, and to know how tall the row comes out — so it is a function, not a literal
// repeated at both.
static float preview_text_w(float sw) {
    return sw - 2.0f * SHEET_PAD - PREVIEW_ICON - PREVIEW_GAP;
}

// The preview of what is being shared: the payload as the headline with its MIME
// type under it, behind a generic document mark. Without ZELTO_SHARE_PAYLOAD (an
// older zsysd, or a deep link rather than a share) it degrades to the action.
//
// THE PAYLOAD IS CUT TO THE COLUMN, NOT WRAPPED. It was a bare Text, which
// measures to one line however long that line is, so a shared URL — the single
// most likely thing to arrive here — ran off the right edge of the sheet and off
// the screen. P45 found this, wrote down that WrapText was the wrong tool for it
// (a share preview wants one line, and a sheet whose height is a sum of its rows
// cannot absorb a paragraph), and left the overflow shipping because the toolkit
// had no truncation. It has one now: EllipsizeText measures with the real shaper
// and cuts on a character boundary, so the node's own width is bounded by the
// column and the row cannot overflow whatever it is handed. The MIME line takes
// the same treatment — it is shorter, but it is not bounded either.
static ZView preview_row(ZApp *app, float sw) {
    const char *payload = getenv("ZELTO_SHARE_PAYLOAD");
    const char *mime = getenv("ZELTO_SHARE_MIME");
    bool have = payload && payload[0];
    float tw = preview_text_w(sw);

    return HStack(
        Frame(PREVIEW_ICON, PREVIEW_ICON,
            CornerRadius(PREVIEW_ICON * Z_RADIUS_ICON,
                Background(Z_COLOR_SURFACE_3,
                    ZStack(
                        Foreground(Z_COLOR_TEXT,
                            Weight(Z_WEIGHT_SEMIBOLD,
                                Font(Z_FONT_CALLOUT, Text("\xe2\x86\x91")))),
                        .align = Z_ALIGN_CENTER)))),
        Frame(tw, 0.0f,
            VStack(
                EllipsizeText(app, have ? payload : "Share", .width = tw,
                              .size = Z_FONT_HEADLINE,
                              .weight = Z_WEIGHT_SEMIBOLD,
                              .color = Z_COLOR_TEXT),
                EllipsizeText(app,
                              (mime && mime[0]) ? mime : "Choose a destination",
                              .width = tw, .size = Z_FONT_SUBHEAD,
                              .color = Z_COLOR_TEXT_MUTED),
                .spacing = PREVIEW_LINE_GAP, .align = Z_ALIGN_LEADING)),
        .spacing = PREVIEW_GAP, .align = Z_ALIGN_CENTER);
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

// How tall the sheet really stands, MEASURED. Every addend names the row it pays
// for and every text row asks the face how tall a line is (z_line_height), so
// this and the column it describes cannot disagree.
//
// It used to be a hand-written sum with the type metrics guessed: a target's name
// was budgeted 14 units where a Caption2 line stands 26, and the preview row was
// budgeted the height of its ICON (52) where its two lines of text stand 77. Off
// by 40 units in a 372-unit sheet, and — this is why nobody saw it — off in a way
// that does not overflow anything. The stack simply measured taller than the
// Frame asked for and stood at its own height, so the sheet LOOKED right. What
// was wrong was everything computed FROM sheet_h:
//
//   - the backdrop blur (z_backdrop) covered a rectangle 40 units shorter than
//     the sheet, so the top strip of the material sat over unblurred wallpaper;
//   - the entrance rise is `(1 - e) * sheet_h`, and a sheet that rises by 40 less
//     than its own height does not start off the bottom edge — it starts with its
//     top 40 units already on screen and pops. The comment on that line claimed
//     the opposite, and was the reason to compute the height at all.
static float sheet_height(ZApp *app, bool more) {
    // The preview row is as tall as the taller of its two columns.
    float text_h = z_line_height(app, Z_FONT_HEADLINE) + PREVIEW_LINE_GAP +
                   z_line_height(app, Z_FONT_SUBHEAD);
    float preview_h = text_h > PREVIEW_ICON ? text_h : PREVIEW_ICON;
    float target_h = TARGET_ICON + SHEET_NAME_GAP +
                     z_line_height(app, Z_FONT_CAPTION2);
    float h = 2.0f * SHEET_PAD + SHEET_GRAB_H + SHEET_PREVIEW_GAP + preview_h +
              SHEET_PREVIEW_GAP + SHEET_RULE_H + SHEET_RULE_GAP + target_h +
              SHEET_ACTION_GAP + SHEET_ROW_H + SHEET_FOOT;
    if (more) {
        h += SHEET_MORE_GAP + z_line_height(app, Z_FONT_FOOTNOTE);
    }
    return h;
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
        Rect(.color = Z_COLOR_TEXT_FAINT, .width = 44, .height = SHEET_GRAB_H,
             .radius = 3),
        Spacer(), .spacing = 0, .align = Z_ALIGN_CENTER);
    col.children[k++] = sgap(SHEET_PREVIEW_GAP);
    col.children[k++] = preview_row(app, (float)z_app_width(app));
    col.children[k++] = sgap(SHEET_PREVIEW_GAP);
    // The hairline. The growing Rect must sit in a HORIZONTAL stack: `.grow`
    // expands along its parent's MAIN axis, so a grow-Rect dropped straight into
    // this vertical column stretches DOWNWARD and paints a 40px grey slab where a
    // 1px rule belongs.
    col.children[k++] = Frame(0.0f, SHEET_RULE_H,
        HStack(Rect(.color = Z_COLOR_MATERIAL_EDGE, .grow = 1.0f),
               .spacing = 0, .align = Z_ALIGN_CENTER));
    col.children[k++] = sgap(SHEET_RULE_GAP);

    // The target row. Cells are fixed-width and the row is LEADING-aligned, so a
    // sheet with two targets puts them where the first two of four would be.
    ZStackOpts row = {.spacing = 10.0f, .align = Z_ALIGN_LEADING};
    for (int i = 0; i < n; i++) {
        row.children[i] = target_cell(s->ids[i], i + 1);
    }
    col.children[k++] = z_stack(Z_AXIS_HORIZONTAL, &row);
    if (s->n > n) {
        col.children[k++] = sgap(SHEET_MORE_GAP);
        col.children[k++] = Foreground(Z_COLOR_TEXT_FAINT,
            Font(Z_FONT_FOOTNOTE,
                 Text("%d more app%s can handle this", s->n - n,
                      s->n - n == 1 ? "" : "s")));
    }
    col.children[k++] = sgap(SHEET_ACTION_GAP);
    col.children[k++] = action_row(on_cancel, "Cancel");
    col.children[k++] = sgap(SHEET_FOOT);   // clearance under the last row
    // Any slack lands HERE, at the bottom of the sheet, rather than wherever a
    // grow-flagged node happens to sit. It used to be load-bearing: sheet_h was a
    // hand-written sum with the type metrics guessed, so there was always slack.
    // Now that sheet_height() measures, there should be none — this stays as the
    // place for a rounding unit to go, not as the absorber for a wrong number.
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
    // frame. sheet_height() measures; see the note over it for what it cost when
    // it guessed. Said out loud once, so the number can be checked against where
    // the rows actually land (ZELTO_PROBE_TAPS dumps the laid-out frames on the
    // first SETTLED build).
    float sheet_h = sheet_height(app, s->n > n);
    static bool said;
    if (!said) {
        said = true;
        fprintf(stderr, "[chooser] sheet_h=%.1f screen=%.0fx%.0f targets=%d\n",
                sheet_h, sw, sh, n);
        fflush(stderr);
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
                // MINUS the padding: a Frame's fixed height is an INNER height
                // when the node it sizes also carries a .padding — measure()
                // does `n->h = fixed_h + 2 * padding` (sdk/src/layout.c), so
                // handing it the outer height makes the sheet stand a full
                // SHEET_PAD taller on each side than the number every other user
                // of sheet_h believes. That is 40 units here, and it is where the
                // old backdrop seam and the 40-unit entrance pop came from: they
                // were not two bugs, they were this one, seen twice.
                Frame(sw, sheet_h - 2.0f * SHEET_PAD,
                      z_stack(Z_AXIS_VERTICAL, &col)))));

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
