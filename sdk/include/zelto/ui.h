// C API: UI (<zelto/ui.h>)
//
// The native declarative UI toolkit. An app is a Wayland client linking
// libzelto; its body() returns a ZView tree that the framework lays out and
// renders into its surface. See docs/api-reference/c/ui.md and
// docs/guides/declarative-ui.md.
//
// MVP surface: View/Stack/Rect/Text/Spacer nodes, stack+flex layout, a software
// renderer, and HarfBuzz/FreeType text. Animation, gestures, lists, navigation,
// and the rest of docs/api-reference/c/ui.md are Planned.
#ifndef ZELTO_UI_H
#define ZELTO_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zelto/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif // ZELTO_UI_H

// Opaque handles. A ZView is an arena-allocated node, cheap to create and
// thrown away each rebuild; never freed by the app.
typedef struct ZNode *ZView;
typedef struct ZApp ZApp;

// Persistent (non-arena) handles. Unlike views, these survive rebuilds: the
// framework keeps them in per-screen retained storage so an animated value or a
// scroll position carries across body() calls. Allocated by call order within
// the building screen (the first z_animated_value() call in a screen body maps
// to the same cell every rebuild — a stable identity à la React hooks). See
// docs/contributing/sdk-internals.md ("View tree vs. scene graph").
typedef struct ZAnimated ZAnimated;   // a spring-backed scalar
typedef struct ZScroll ZScroll;       // a scroll position + fling state
typedef struct ZNav ZNav;             // the navigation stack

// ---------------------------------------------------------------------------
// Callbacks & actions.
//
// A ZAction is a tap/activation handler; a ZKeyAction handles a key press. Both
// receive the live ZApp and the app's persistent state struct (the same pointer
// passed to body), so they can mutate state and call z_invalidate. They are
// invoked from the app loop on input, NOT during body(), so they take app/state
// as parameters rather than capturing them.
//
// Handlers are ordinary named functions in C; the JS/Script binding's inline
// closures (docs/guides/gestures.md) cannot be expressed as a portable C macro
// under the project's strict ISO C settings (no GNU statement-expressions /
// nested functions). Define a static function and pass it:
//
//   static void on_tap(ZApp *app, void *state) {
//       App *s = state; s->count++; z_invalidate(app);
//   }
//   ...  Button(on_tap, "Count: %d", s->count)
typedef void (*ZAction)(ZApp *app, void *state);
typedef void (*ZKeyAction)(ZApp *app, void *state, uint32_t keysym);

// Like ZAction but also receives a per-view data pointer bound at build time —
// the way a data-driven row carries its item into the tap handler (closures
// being unavailable in strict C). The data must outlive the tap (point into
// stable state). See OnTapData below.
typedef void (*ZTapAction)(ZApp *app, void *state, void *data);

// Maximum children collected by a single stack literal (see the options trick
// below). Plenty for hand-written UI; List handles large data sets (Planned).
#define Z_MAX_CHILDREN 32

// Cross-axis alignment within a stack.
typedef enum ZAlign {
    Z_ALIGN_LEADING = 0,
    Z_ALIGN_CENTER,
    Z_ALIGN_TRAILING,
} ZAlign;

// Stack direction.
typedef enum ZAxis {
    Z_AXIS_VERTICAL = 0,   // VStack: top -> bottom
    Z_AXIS_HORIZONTAL,     // HStack: leading -> trailing
    Z_AXIS_DEPTH,          // ZStack: back -> front (overlap)
} ZAxis;

// THE SCREEN UNIT. Zelto's surface coordinate is a raw device pixel: the phone
// output is 720x1440 and every metric in the OS is written in those pixels. The
// design reference it was drawn against is a 390pt-wide handset, so one HIG POINT
// is ~1.85 Zelto units (720/390 = 1.846) — which is exactly the proportion the
// layout metrics carry: a 104px app icon is 14.4% of the width where iOS's 60pt
// icon is 15.3%, and a 76px settings row is 5.3% of the height where iOS's 44pt
// row is 5.2%.
//
// The type scale below did NOT carry it. Until P43 it was a 1:1 transcription of
// the HIG POINT ramp (Body 17, Caption2 11) sitting inside pixel metrics — so
// every label in the OS was drawn at ~54% of the size its container was built
// for: a 17px body label centred in a 76px row, an 11px caption under a 104px
// icon, a 13px footnote spanning a third of a 720px screen. Nothing was broken
// enough to look like a bug, which is why it survived thirteen phases: every
// surface was uniformly, quietly under-typed.
//
// The ramp is therefore authored in points, as a designer reads it, and Z_PT
// converts to screen units at the single point of truth. Retuning the whole
// system's type is one numerator.
//
// P43 ALSO GENERALISED FROM TWO METRICS AND GOT IT WRONG, which is why Z_PT
// exists as a name of its own. The two proportions above are real, but they are
// the metrics that were DERIVED from the screen (the icon falls out of a 4-column
// grid on 720 units; the settings row's total falls out of a 44 + 2x16 padding
// that happens to sum to 76). The metrics LIFTED FROM APPLE'S SPEC TABLES had
// exactly the type scale's bug and kept it a phase longer: a 34-unit home
// indicator where the spec says 34 POINTS (2.4% of the display against Apple's
// 4.0%), a 40-unit status bar that is proportionally the CLASSIC 20pt one on a
// design that had otherwise dropped the home button, a keyboard whose keys stood
// 30pt tall against a 44pt touch-target minimum. P44 moved those onto Z_PT too;
// see system/common/safe_areas.h.
//
// So: a number copied off a spec sheet is in POINTS and goes through Z_PT. A
// number derived from the screen (a fraction of the width, a cell that divides a
// grid) is already in screen units and must NOT. Nothing else in the OS is a
// bare pixel constant by choice.
//
// Integer arithmetic, not a float multiply: an enum needs a constant expression.
#define Z_TYPE_NUM 185
#define Z_TYPE_DEN 100

// A HIG point in screen units. Rounds down; the half-unit that costs is below
// what the renderer resolves.
#define Z_PT(pt) ((pt) * Z_TYPE_NUM / Z_TYPE_DEN)

// The type ramp's spelling of the same conversion — a step is points, always.
#define Z_TYPE(pt) Z_PT(pt)

// --- The spacing scale (P52) ---------------------------------------------
// THE THIRD TIME THIS BUG HAS BEEN FOUND. Until P52 there was no spacing scale
// at all: every gap in the OS was an inline literal, and the histogram of those
// literals is the confession. The three commonest values were `spacing = 8`,
// `spacing = 10` and `spacing = 12` — which are, exactly, SwiftUI's default
// VStack spacing (8pt), its default HStack spacing (10pt), and the 12pt step of
// Apple's grid, typed straight into SCREEN UNITS. The paddings tell the same
// story: 16, 20, 24, 32 units, which are the iOS grid's POINT steps. So every
// gap in the OS was drawn at ~54% of the size it was written for, the identical
// defect as the P43 type scale and the P44 safe areas, and invisible for the
// identical reason — it was uniformly wrong, so nothing looked broken. The
// corroboration is that the metrics which had ALREADY been through this (ROW_PAD
// = Z_PT(16), Z_ROW_H = Z_PT(44)) are the ones that are correct today: the row
// that holds a 16pt outer margin was putting a 6.5pt gap between its own icon
// and its label.
//
// THE SCALE IS THE 8PT GRID with a 4pt half-step, which is Apple's convention
// rather than a published token table (there is no enumerated Apple spacing
// scale — see docs/design/ios27-reference.md §2). Two additions to the bare
// grid, each with a reason:
//   - 2XS (2pt) is BELOW the grid on purpose. A title and the line under it are
//     nearly leading-adjacent in every Apple surface; snapping that pair up to
//     4pt loosens every text stack in the OS, which is a redesign, not a regrid.
//   - L (16pt) is the CONTENT MARGIN and is deliberately the same number as
//     ROW_PAD. A gap between siblings and the inset that holds them under a
//     card's edge are the same measurement seen twice, and writing them as one
//     step is what stops them drifting apart again.
// SwiftUI's horizontal default is 10pt against its vertical 8pt; Zelto keeps ONE
// default (S = 8pt). Having a single sibling gap is worth more here than
// reproducing a 2pt axis distinction nothing in this OS reads as intentional.
//
// Pick by ROLE:
//   between siblings in a ROW          S
//   between components in a COLUMN     M
//   a caption under the control it names   XS
//   a subtitle under its title             2XS
//   a container's content margin           L
// A gap that IS a control's own geometry (the passcode dots' pitch, the status
// bar's glyph cluster) takes the NEAREST step rather than the role's reference —
// this scale regrids the gaps BETWEEN views, and redrawing a control is the
// control-metrics stage's job, not a spacing pass's. Two things are deliberately
// outside the scale entirely: the internals of a GLYPH (the battery meter's
// segments, the App Library's 2x2 quad mark) are drawing, not layout, and
// Button/TextField's own insets are control metrics.
//
// A raw number in a layout is the bug this scale exists to stop.
#define Z_SPACE_2XS Z_PT(2)    //  3 — leading-adjacent text: a title and its subtitle
#define Z_SPACE_XS  Z_PT(4)    //  7 — inside one control: a glyph and its label
#define Z_SPACE_S   Z_PT(8)    // 14 — THE DEFAULT gap between siblings
#define Z_SPACE_M   Z_PT(12)   // 22 — between the components of a column
#define Z_SPACE_L   Z_PT(16)   // 29 — content margin / row inset (== ROW_PAD)
#define Z_SPACE_XL  Z_PT(20)   // 37 — between the groups of a grouped list
#define Z_SPACE_2XL Z_PT(24)   // 44 — an app's outer column padding
#define Z_SPACE_3XL Z_PT(32)   // 59 — the widest breathing room in the system

// A standard list row's MINIMUM total height — Apple's 44pt table row, which is
// also the HIG minimum touch target and therefore the floor for anything
// tappable. It is a TOOLKIT metric (List's default row, a settings row), not a
// safe area: the cross-process contracts with the compositor live in
// system/common/safe_areas.h.
//
// A FLOOR, as of P50, not the answer. 44pt is a number off a spec sheet — it
// says how big a finger is, and says nothing about what the row CONTAINS. Body
// is 31 units inside an 81-unit row at the default text size and 42 at the
// largest, and a row that stays 81 either clips the label or centres it in a box
// it no longer fits. So the row height is now the LARGER of this touch-target
// floor and what the type actually needs; see z_row_h().
#define Z_ROW_H Z_PT(44)

// The vertical breathing room a row puts above and below its line of text. 11pt
// is what the shipped row already had — 81 total less Body's ~41-unit line,
// halved — so at the default text size z_row_h() reproduces Z_ROW_H exactly and
// this is not a redesign, it is the same row with its height derived instead of
// declared.
#define Z_ROW_VPAD Z_PT(11)

// A standard list row's TOTAL height at the current text size: the touch-target
// floor, or one line of Body plus Z_ROW_VPAD top and bottom, whichever is
// larger. Needs the app because it asks the FACE for the line height (P46:
// never estimate a text metric — the true line/size ratio runs 1.3182 down to
// 1.2529 across the scale, so a fudge factor over-reserves at one end and
// under-reserves at the other).
float z_row_h(ZApp *app);

// Type scale — a semantic set of steps, à la the platform text styles (Apple HIG
// / Material type scale). Pick by ROLE, not by pixel count, so the OS retypes
// coherently from one place. The ladder is dense in the reading band
// (Footnote→Body→Headline) where hierarchy is finest and coarser above it. Body
// is 17pt (the legibility floor for sustained reading); Caption2 (11pt) is the
// smallest step. Emphasis is a separate axis — pair a step with Weight() (HIG
// leans on size AND weight for hierarchy, so Headline is Body-sized + Semibold).
// The comment after each step is the value in SCREEN UNITS, what the renderer
// actually gets.
//
// P52 RECONCILED THE RAMP AGAINST SF PRO, AND EIGHT OF ELEVEN STEPS ALREADY
// MATCHED — which is the useful result, because it means the three that did not
// were drift rather than a systematic error. (Contrast the P43 finding, where
// EVERY step was wrong by the same factor. A ladder mostly on-spec with a few
// outliers is a different bug from a ladder uniformly off-spec, and the fix is
// per-step rather than to the converter.) The three that moved:
//
//     LARGE_TITLE  40pt -> 34pt   Apple's Large Title. The hero ran +6pt.
//     TITLE2       24pt -> 22pt   Apple's Title 2.
//     CALLOUT      20pt -> 16pt   Apple's Callout. See below — this one had a
//                                 misreading attached to it worth recording.
//
// THE CALLOUT NOTE. The dossier first read Zelto's 20pt "Callout" as actually
// being iOS's TITLE 3 (20pt) — a step the ladder lacks — which would have meant
// renaming it and adding a real 16pt Callout. Checking the twenty-two call sites
// says otherwise: every one of them is a control label (a keyboard function key,
// a consent button, "Done", the remove badge) or emphasised body (a status line,
// a strapline, "No running apps"). Not one is a compact title. So the number was
// Title 3 while every USE was a Callout, and the fix is the number. No
// Z_FONT_TITLE3 is added: a token no surface asks for is a step that will be
// picked by whoever wants "a bit bigger", which is how a ladder stops meaning
// anything.
//
// TRACKING (letter-spacing) IS DELIBERATELY NOT MODELLED, and this is the
// written decision rather than an oversight. iOS auto-tracks: negative on large
// text, crossing zero around Subheadline, slightly positive below. Converted,
// the whole effect is -2 units at Large Title, -1 through the Title/Body band,
// and ZERO below ~16pt where it rounds away — so it is invisible on most of this
// OS and worth about one unit where it is not. Against that: a tracking axis has
// to be threaded through BOTH measure and paint, and if the two ever disagree
// the text overflows its box silently. That is the exact failure this project
// has already paid for twice (the wrap_measure truncation in P45, the fixed
// column holding text in P51). One unit of Title tightening does not buy that
// risk. If it is ever added, it goes in z_text_measure and the glyph loop in the
// same commit, with a test that measures a tracked string and asserts the frame.
typedef enum ZFont {
    Z_FONT_CAPTION2 = Z_TYPE(11),      // 20 — smallest: dense metadata
    Z_FONT_CAPTION = Z_TYPE(12),       // 22 — caption / overline
    Z_FONT_FOOTNOTE = Z_TYPE(13),      // 24 — secondary caption
    Z_FONT_SUBHEAD = Z_TYPE(15),       // 27 — subheading / dense body
    Z_FONT_BODY = Z_TYPE(17),          // 31 — primary reading size (HIG body)
    Z_FONT_HEADLINE = Z_TYPE(17),      // 31 — body-sized, meant with Weight(SEMIBOLD)
    Z_FONT_CALLOUT = Z_TYPE(16),       // 29 — emphasised body / a control label
    Z_FONT_TITLE2 = Z_TYPE(22),        // 40 — section title
    Z_FONT_TITLE = Z_TYPE(28),         // 51 — screen title
    Z_FONT_LARGE_TITLE = Z_TYPE(34),   // 62 — hero / a screen's opening title
    // Above the reading ladder entirely: a number that IS the screen. Only the
    // lock screen's clock uses it — that clock is not a title, it is the reason
    // the screen exists, and at Large Title it reads as a heading with nothing
    // under it. A step this far out belongs in the scale rather than as a cast
    // integer at one call site, so it retypes with everything else.
    //
    // This step is where the point/pixel confusion was easiest to see and hardest
    // to name: it was written as a bare 92, and 92 is the POINT size of the iOS
    // lock clock — the same transcription error as every step above it, but so far
    // out on the ladder that the result still read as "big" beside type that was
    // equally undersized. Measured against the screen it was not: the clock's
    // digits stood 4.6% of the display's height where the phone it copies puts
    // them at ~8.9%. Scaled like everything else it lands at 170.
    Z_FONT_DISPLAY = Z_TYPE(92),       // 170 — the lock clock
} ZFont;

// ---------------------------------------------------------------------------
// DYNAMIC TYPE (P50) — the user's text size, and the one seam it needs.
//
// THE PROBLEM. Every step above is an enum member: a compile-time constant, in
// screen units, baked into 19 surfaces that pass it to Font() directly. A user
// who wants bigger text has nothing to turn — this OS could not make the text
// bigger at all, which is the one accessibility affordance every phone ships.
// The fix cannot be "pass a size through 19 call sites", because then every new
// call site has to remember, and the ones that forget are invisible.
//
// THE SEAM. Exactly one function turns a ZFont STEP into the units the shaper
// gets: z_font_units(). Font(), the default on every node, WrapText,
// EllipsizeText and z_line_height all route through it, and nothing above it
// knows a setting exists — the same shape as P47's classifier seam, where the
// caller asks for a key and the model decides what that means.
//
// WHAT THE STEPS BECOME, AND WHY IT IS NOT A MULTIPLIER. "x1.35 everywhere" is
// the obvious implementation and it is wrong: it turns a 40pt Large Title into
// 54pt (a hero that eats the screen) while a Caption goes 11 -> 15, and the
// small end is the end that needed help. Read off Apple's own Dynamic Type
// table, the rule is not a ratio at all — from the default size (Large) to
// xxxLarge, EVERY style gains exactly SIX POINTS:
//
//        style        L -> xxxL     ratio
//        Caption2     11 -> 17      x1.55
//        Footnote     13 -> 19      x1.46
//        Body         17 -> 23      x1.35
//        Title        28 -> 34      x1.21
//        LargeTitle   34 -> 40      x1.18
//
// A constant ADDITIVE offset is a large ratio on a small number and a small one
// on a large number, which is precisely the "large steps move less" behaviour,
// falling out of one rule instead of a twelve-row table nobody can keep true.
// So: the ladder scales by a per-size-step POINT OFFSET, applied to every step
// alike, floored at Caption2's 11pt (Apple floors there too — the small sizes
// bottom out rather than collapsing).
//
// Because the offset is in POINTS and the enum is in UNITS, the conversion is
// one Z_PT() on the offset — no lossy units->points round trip of the step.
//
// THE RANGE: the seven standard sizes, xSmall .. xxxLarge, index 0..6, default
// 3 (Large — what shipped before this existed, so an unset device is unchanged).
// A value outside 0..6 is CLAMPED, not rejected: this is read from a brokered
// string that any process can write, and a garbage value must degrade to a
// legible screen rather than a 2-unit one.
//
// THE FIVE ACCESSIBILITY SIZES (P51), steps 7..11 — AX1..AX5. P50 shipped the
// seven standard steps and said the five above them were a phase, because they
// are not a bigger number of the same kind: past a point a label and its control
// stop fitting side by side, so a row is no longer a HEIGHT, it is a LAYOUT
// DECISION. See Z_TEXT_SIZE_AX_FIRST / z_text_size_reflows() below — the break is
// ONE predicate the whole OS asks, not a threshold each surface reimplements.
//
// THE OFFSETS ABOVE xxxLarge ARE BODY'S OWN COLUMN, and the additive rule is
// KEPT while being honest about where it frays. Apple's AX columns are not a
// constant offset any more; the gain per style spreads:
//
//        style        L    AX1  AX2  AX3  AX4  AX5
//        Caption2     11   20   24   29   34   38     (+9  +13 +18 +23 +27)
//        Body         17   28   33   40   47   53     (+11 +16 +23 +30 +36)
//        Title1       28   38   43   50   58   64     (+10 +15 +22 +30 +36)
//        LargeTitle   34   44   48   52   56   60     (+10 +14 +18 +22 +26)
//
// Body, Title1 and Caption2 stay within ~2-3pt of each other all the way to AX5;
// only LargeTitle peels off, and it peels DOWNWARD (Apple compresses its hero
// style so it does not eat the screen). Taking Body's column as the offset is
// therefore exact for the style most of the OS is made of, within 3pt for the
// rest of the reading ladder, and 10pt generous on the one style that appears
// once per screen. A second table to shave those 10 points would have to be kept
// true by hand; the ceiling that actually matters — the hero not eating the
// screen — is a LAYOUT constraint and is better enforced where the layout is.
#define Z_TEXT_SIZE_STEPS 12
#define Z_TEXT_SIZE_DEFAULT 3

// The first ACCESSIBILITY step. Below this is the standard ladder. One name, so
// the number is not spelled anywhere else.
#define Z_TEXT_SIZE_AX_FIRST 7

// ---------------------------------------------------------------------------
// THE REFLOW BREAK — the one predicate, and the measurement that placed it.
//
// A row that pairs a label with a control is a HORIZONTAL layout with an
// assumption baked in: that the label, the detail value and the control all fit
// on one line of a 640-unit card. Dynamic Type does not break that assumption
// gradually. It breaks it at a step, and past that step the fix is not a taller
// row — the label has to WRAP and the control has to move UNDER it, which is a
// different layout for the same screen.
//
// WHERE IT BREAKS, measured rather than assumed. Settings ▸ Lock Screen (the
// densest rows in the OS: a switch row, three stepper rows, an action row) was
// booted at every step 3..11 with ZELTO_PROBE_TAPS on, and the widest row —
// 'Screen Off After', a label plus a tabular value plus a two-key pill — is the
// one that says where the break is:
//
//     step 7 (AX1): the pill's '+' key ends at x=671 on a 720 screen. Fits.
//     step 8 (AX2): the same key lands at x=701 and ends at 753. GONE.
//
// A control 33 units off the edge of the screen cannot be pressed at all, and
// nothing on the screen says it is there. That is the break: AX2.
//
// MEASURED AFTER FIXING THE ROW, WHICH IS WHY IT IS NOT AX3. The stepper's value
// column was a literal 56 units — a fixed WIDTH holding text, wrong at the
// default size already ('120s' wants 62) — and a Text that does not fit its box
// paints straight through it rather than pushing anything. Until the column
// scaled, the row APPEARED to survive AX2 while the value quietly drew over the
// label, and the first symptom was the row running out of HEIGHT at AX3. Fixing
// the width moved the break down a step and made it a single, honest one.
//
// The vertical half is gone separately: rows are z_row_h() now, so a row's height
// follows its line at every step. What is left is purely horizontal, which is
// what a reflow is for.
//
// WHY A STEP AND NOT A FIT TEST. The honest condition is per-row: "does this
// label plus this control fit this card". The toolkit cannot ask that — layout
// is one intrinsic-size pass, so a row cannot measure itself and then choose a
// different shape. A step threshold is the approximation, and it is calibrated
// on the WIDEST row in the OS at 720 units, so the rows that break first are the
// ones the number was read off. A surface with a shorter label reflows a step or
// two earlier than it strictly had to; that costs it a taller row, not a
// truncated one.
//
// WHY ONE PREDICATE. The same argument that made z_font_units() one function: a
// threshold each surface reimplements is a threshold that drifts, and the drift
// is invisible because every surface still looks fine at the size its author
// tested. Ask this; never compare z_text_size() to a literal.
#define Z_TEXT_SIZE_REFLOW_FIRST 8

// True when rows must lay their controls out UNDER their labels rather than
// beside them. False for a process that called z_text_scaling_disable(): a
// surface pinned to the default text size never reflows either.
bool z_text_size_reflows(void);

// The brokered keys. These live HERE, not in system/common/settings_defaults.h
// where the other shared key names are, and the layering is the reason: every
// libzelto process reads them from inside the toolkit, and system/common sits
// ABOVE the toolkit (it includes this header). settings_defaults.h points at
// these rather than spelling them again, so there is still one definition.
#define ZELTO_KEY_TEXT_SIZE "sys.text_size"   // 0..11, default Z_TEXT_SIZE_DEFAULT
#define ZELTO_KEY_BOLD_TEXT "sys.bold_text"   // 0/1
// The third accessibility preference libzelto reads for itself, and it is here
// beside the other two for the same layering reason. What it does is a palette
// question, so the numbers and the decision live in <zelto/gfx.h>.
#define ZELTO_KEY_INCREASE_CONTRAST "sys.increase_contrast"   // 0/1

// The per-step offset in POINTS, indexed by the size step. Down from the default
// it is one point per step; up it is two, then two, then two — Apple's own
// spacing, which grows faster above the default because that is the direction
// somebody is actively asking for.
#define Z_TEXT_SIZE_OFFSETS                                                    \
    { -3, -2, -1, 0, +2, +4, +6, +11, +16, +23, +30, +36 }

// A step's size in SCREEN UNITS at the process's current text size. This is the
// only conversion from ZFont to a number the renderer sees.
float z_font_units(ZFont step);

// The current step index (0..Z_TEXT_SIZE_STEPS-1) and its human name.
int z_text_size(void);
const char *z_text_size_name(int step);

// Turn Dynamic Type OFF for this process. Three surfaces must not scale, and it
// is a DECISION, not an oversight:
//
//   - THE STATUS BAR. Its height is an exclusive-zone CONTRACT (ZELTO_BAR_H) that
//     the shade, the dim scrim and the volume HUD each offset by. Bigger type in
//     a bar that cannot get taller is a clock painting through its own strip.
//   - THE KEYBOARD. A cap is a TOUCH TARGET sized in points, not a text box; the
//     letter on it is a label for a finger, not something you read. Growing the
//     caps walks the bottom row off the bottom of the surface, which is the exact
//     P48 bug with a new cause. (iOS does not scale the keyboard either.)
//   - THE HOME INDICATOR. A 62-unit strip with a pill in it and no text at all.
//
// Safe areas are in the same class and need no call: they are PHYSICAL (the
// strip a thumb can reach, the strip the system owns) and Z_PT() converts them
// once, at compile time, without going near this.
void z_text_scaling_disable(void);

// Font weight — the second hierarchy axis. The bundled face is a single Regular;
// heavier steps are synthesised by emboldening the glyph outline (a modest,
// legible faux-bold), so weight works without shipping extra faces. Use it with
// Weight(); Regular is the default on every text node.
typedef enum ZWeight {
    Z_WEIGHT_REGULAR = 0,
    Z_WEIGHT_MEDIUM,
    Z_WEIGHT_SEMIBOLD,
    Z_WEIGHT_BOLD,
} ZWeight;

// ---------------------------------------------------------------------------
// Stacks.
//
// Children are listed first (positionally); options follow as designated
// initializers. This ordering is what C makes robust: positional initializers
// fill `children[0..]` (the first struct member), and the `.field = value`
// options that follow can be given in any subset/order. Unused trailing child
// slots zero-initialize to NULL, which terminates the list. E.g.
//
//   VStack(
//       Text("Title"),
//       Rect(.color = Z_COLOR_PRIMARY, .width = 80, .height = 80),
//       .spacing = Z_SPACE_M, .align = Z_ALIGN_LEADING, .padding = Z_SPACE_L);
// ---------------------------------------------------------------------------
typedef struct ZStackOpts {
    ZView children[Z_MAX_CHILDREN];      // MUST be first (positional children)
    float spacing;
    ZAlign align;
    float padding;
    float grow;                          // flex weight if nested in a stack
} ZStackOpts;

ZView z_stack(ZAxis axis, const ZStackOpts *opts);

#define VStack(...) z_stack(Z_AXIS_VERTICAL, &(ZStackOpts){__VA_ARGS__})
#define HStack(...) z_stack(Z_AXIS_HORIZONTAL, &(ZStackOpts){__VA_ARGS__})
#define ZStack(...) z_stack(Z_AXIS_DEPTH, &(ZStackOpts){__VA_ARGS__})

// A flexible gap that eats free main-axis space.
ZView z_spacer(void);
#define Spacer() z_spacer()

// ---------------------------------------------------------------------------
// Content.
// ---------------------------------------------------------------------------

// A solid (optionally rounded) coloured box.
typedef struct ZRectOpts {
    ZColor color;
    float width, height;   // 0 = size to parent / flex
    float radius;
    float grow;
} ZRectOpts;

ZView z_rect(const ZRectOpts *opts);
#define Rect(...) z_rect(&(ZRectOpts){__VA_ARGS__})

// A line of text (printf-style). ONE line: it measures to its natural width and
// does not wrap (layout is a single intrinsic-size pass). For a run of prose
// wider than its column — a caption, a description, a paragraph — use WrapText,
// which breaks it to a known width.
ZView z_text(const char *fmt, ...);
#define Text(...) z_text(__VA_ARGS__)

// Prose wrapped to a fixed pixel WIDTH. Unlike Text, this is not printf: pass a
// ready string (it is the paragraph as written, '\n's included — a newline is a
// hard break, and a blank line is kept as a paragraph gap). It splits into as
// many Text lines as needed, measuring with the real font so no line exceeds
// `.width`; a word too long to fit alone is broken mid-word rather than allowed
// to run off the edge.
//
// It needs the app because it measures at BUILD time, before the tree the layout
// pass would measure exists — that is the whole reason it can wrap when Text
// cannot. `.width` is required; the rest take the Text defaults. Capped at
// Z_MAX_CHILDREN lines (a screenful of a single paragraph; longer prose belongs
// in a Scroll, and each WrapText inside it wraps its own block).
typedef struct ZWrapOpts {
    float width;       // REQUIRED: the column width in screen units
    ZFont size;        // 0 = Z_FONT_BODY
    ZWeight weight;    // 0 = Regular
    ZColor color;      // .a == 0 = inherit (Z_COLOR_TEXT)
    float line_gap;    // extra px between lines (0 = snug)
} ZWrapOpts;

ZView z_text_wrap(ZApp *app, const char *s, const ZWrapOpts *opts);
#define WrapText(appp, s, ...) \
    z_text_wrap(appp, s, &(ZWrapOpts){__VA_ARGS__})

// ONE line, cut to fit its column with an ellipsis. The other half of the answer
// to "text I did not write, in a box I chose".
//
// WrapText is right for PROSE — a permission sentence, a package description, a
// notification body — where every word matters and the box can grow. It is wrong
// for an IDENTIFIER shown as a label: the share sheet's payload is a URL, and a
// URL wrapped over six lines is not more readable than a URL cut short, it is a
// sheet that has eaten the screen. What that wants is the head of the string and
// a mark saying there is more, which is what every share sheet, tab title and
// file row on every platform does.
//
// Until P46 the toolkit had neither for that case, so system/chooser drew the
// payload as a bare Text in a fixed-height row and it ran off the sheet. P45
// found that, wrote down why WrapText was the wrong tool, and left the overflow
// shipping. Both halves of its reasoning were right and the conclusion was not:
// the missing primitive is a measure-and-cut, and the shaper that does the
// measuring was already here.
//
// The cut lands on a UTF-8 boundary, trailing spaces are trimmed before the
// ellipsis, and the result is a plain Text node whose intrinsic width is
// therefore <= .width — so the row it sits in cannot overflow, which is the
// actual guarantee. A string that already fits comes back untouched, with no
// ellipsis and no measuring beyond the one call that established it fits.
typedef struct ZEllipsizeOpts {
    float width;       // REQUIRED: the column width in screen units
    ZFont size;        // 0 = Z_FONT_BODY
    ZWeight weight;    // 0 = Regular
    ZColor color;      // .a == 0 = inherit (Z_COLOR_TEXT)
} ZEllipsizeOpts;

ZView z_text_ellipsize(ZApp *app, const char *s, const ZEllipsizeOpts *opts);
// Not "ClipText": Clip() already means a subtree paint mask in this toolkit, and
// two different meanings of clipping in one file is how the next person reaches
// for the wrong one.
#define EllipsizeText(appp, s, ...) \
    z_text_ellipsize(appp, s, &(ZEllipsizeOpts){__VA_ARGS__})

// An image: a PNG or SVG loaded from `path` and drawn aspect-fit inside the
// node's frame (letterboxed, never stretched). The decode is cached by path, so
// listing an Image in a body() that rebuilds every frame is cheap. Give it a
// size the usual way (Frame(w, h, Image(path))) — without one it sizes to the
// image's intrinsic pixels. SVGs are rasterized by a minimal in-house stroker
// (line icons, single ink); PNGs go through libpng (translucency honoured).
// A path that fails to load draws nothing (the caller supplies any fallback).
ZView z_image(const char *path);
#define Image(path) z_image(path)

// True if `path` can be decoded (result cached like Image, so this is cheap to
// call in body()). Image() itself draws nothing on failure, so a caller that
// wants a fallback image (an app icon → a placeholder) probes with this first.
bool z_image_loads(const char *path);

// Make an Image aspect-FILL its frame — scale up so the frame is fully covered,
// center-cropping whatever overflows — instead of the default aspect-fit
// (letterboxed). Use for a full-bleed backdrop (a wallpaper) that must leave no
// bars. Respects the node's clip + rounded-corner mask. No-op on a non-Image.
//   Cover(Image(path))                              // fills, may crop
//   Frame(w, h, CornerRadius(r, Cover(Image(path))))
ZView Cover(ZView view);

// A round-capped polyline — the toolkit's vector-mark primitive, for crisp icons
// that rectangles can't draw (a chevron, a circle, a square outline) at any size
// without shipping a bitmap. `points` is an array of x,y pairs in the UNIT box
// [0,1]x[0,1], mapped into the node's frame at paint time, so one shape scales to
// whatever Frame() you give it; `count` is the number of POINTS (so the array has
// 2*count floats). `thickness` is the stroke width in px, `color` the ink, and
// `closed` connects the last point back to the first (a polygon outline). The
// points are copied, so a static/stack array is fine. Give it a size the usual
// way: Frame(w, h, Stroke(...)). Segments are drawn anti-aliased with round caps.
//   static const float chevron[] = {0.6f,0.2f, 0.3f,0.5f, 0.6f,0.8f};
//   Frame(28, 28, Stroke(.points = chevron, .count = 3, .thickness = 3,
//                        .color = Z_COLOR_TEXT));
typedef struct ZStrokeOpts {
    const float *points;   // x0,y0,x1,y1,... in the unit box [0,1]
    int count;             // number of points (array holds 2*count floats)
    float thickness;       // stroke width (px)
    ZColor color;          // ink
    bool closed;           // connect last point back to the first
} ZStrokeOpts;

ZView z_stroke(const ZStrokeOpts *opts);
#define Stroke(...) z_stroke(&(ZStrokeOpts){__VA_ARGS__})

// A tappable control: a rounded, padded label that runs `on_tap` when tapped or
// activated from the keyboard (Enter/Space while focused). printf-style label.
//   Button(on_tap, "Count: %d", s->count);   // on_tap is a static ZAction
ZView z_button(ZAction on_tap, const char *fmt, ...);
#define Button(action, ...) z_button(action, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Text field (editable text) — the on-screen-keyboard front end (P21).
//
// A ZTextField is a retained text buffer + length the app owns (put it in the
// app's state struct so it survives rebuilds). TextField renders it as a padded,
// rounded box showing the text (or a dim placeholder when empty) with a caret
// while the field is focused. Tapping the field focuses it, which — under the
// hood via text-input-v3 — tells the compositor to raise the on-screen keyboard;
// each key the keyboard sends is inserted into the buffer (backspace deletes),
// and the app simply reads `field.text`. No wl_keyboard handling in the app.
// Register an on_change callback to react to edits (NULL to ignore). See
// docs/platform/soft-keyboard.md.
// ---------------------------------------------------------------------------
#define Z_TEXTFIELD_CAP 256

typedef struct ZTextField {
    char text[Z_TEXTFIELD_CAP];   // current contents (NUL-terminated)
    int len;                      // bytes in text (excluding the NUL)
    // Selection + caret (P22). `caret` is the insertion point (byte offset, 0..len)
    // where typed/pasted text lands and backspace deletes; `anchor` is the other
    // end of the selection. When anchor == caret there is NO selection and the
    // caret blinks at that offset; when they differ the run [min,max) is selected
    // (highlighted, with drag handles). A tap focuses + collapses to the caret;
    // long-press selects the word; a drag extends. Both default 0 (caret at start);
    // the framework keeps them in 0..len as the buffer changes.
    int caret;
    int anchor;
    // A PASSWORD field (P47). Two consequences, and the second is the one that
    // matters: the field renders bullets instead of the text, and text-input-v3
    // declares CONTENT_PURPOSE_PASSWORD, which the compositor relays to the
    // keyboard. The keyboard turns off everything that assumes the text is
    // language — adaptive touch targets, auto-capitalisation, the double-space
    // period — because a password is precisely the string those get wrong, and
    // "the keyboard corrected my password" is unfixable from the app's side.
    bool secure;
    // Ask the keyboard to auto-capitalise sentences in this field (P47). It is
    // the FIELD's call and not the keyboard's, which is why text-input-v3 carries
    // a content hint for it: a note wants sentence case, a username does not, and
    // nothing the keyboard can see tells the two apart. Off by default because C
    // zero-initialises the struct — a field that wants it says so.
    bool autocap;
    // Fired from the app loop after the buffer changes (a committed string, a
    // backspace, a paste or a cut), with the live app + the app state pointer.
    // NULL = ignore.
    void (*on_change)(ZApp *app, void *state);
} ZTextField;

// Build an editable text field bound to `f`. `placeholder` shows (dimmed) when
// the field is empty. Takes `app` so the widget can reflect focus/caret state.
ZView z_text_field(ZApp *app, ZTextField *f, const char *placeholder);
#define TextField(appp, f, placeholder) z_text_field(appp, f, placeholder)

// ---------------------------------------------------------------------------
// Slider — a value you DRAG, in the range 0..1.
//
// For anything continuous: brightness, volume, a scrub position. Not for a
// choice (that is a switch) and not for a count (that is a stepper). A slider is
// the right control exactly when the user does not know the number they want and
// is going to hunt for it by watching the result.
//
// Like ZTextField, the state is a struct the APP owns — put it in the app's
// state struct so it survives the per-frame rebuild. Write `value` to set the
// control from outside (a brokered setting changing under you); read it, or the
// `on_change` argument, to follow the finger.
//
//   static void on_bright(ZApp *app, void *state, float v) {
//       z_setting_set_int("sys.brightness", 1 + (int)lroundf(v * 4));
//   }
//   s->bright.value = (level - 1) / 4.0f;
//   s->bright.on_change = on_bright;
//   ... Slider(app, &s->bright, .length = 300)
//
// Dragging is RELATIVE: the value moves with the finger from wherever it was, it
// does not jump to the touch point. on_change fires continuously during the
// drag; on_commit (optional) fires once on release, for a setting too expensive
// to write on every frame.
// ---------------------------------------------------------------------------
typedef void (*ZSliderCb)(ZApp *app, void *state, float value);

typedef struct ZSlider {
    float value;                 // 0..1 — the app reads and writes this
    ZSliderCb on_change;         // fired continuously while dragging (may be NULL)
    ZSliderCb on_commit;         // fired once on release (may be NULL)

    // Internal drag bookkeeping; the toolkit owns these. They live here rather
    // than in a keyed cell because they must survive the rebuilds that happen
    // during the drag itself.
    float drag_base;
    float travel;
    bool dragging;
    bool vertical;
} ZSlider;

typedef struct ZSliderOpts {
    float length;      // px along the drag axis (track width, or slab height)
    float thickness;   // px across it (track thickness, or slab width)
    // The Control Center shape: a wide vertical slab whose FILL is the value,
    // dragged up and down, instead of a thin rail with a knob. Use it when the
    // control is the main subject of the screen rather than one row's worth.
    bool tall;
    ZView glyph;       // optional mark at the foot of a tall slider (may be NULL)
} ZSliderOpts;

ZView z_slider(ZApp *app, ZSlider *s, const ZSliderOpts *opts);
#define Slider(appp, s, ...) z_slider(appp, s, &(ZSliderOpts){__VA_ARGS__})

// The height ONE LINE of text occupies at this size — ascent + descent, straight
// off the face, the same two numbers layout gives a Text node (layout.c measure,
// Z_K_TEXT). Available during body(), which is what lets a RESERVE be computed
// from the type scale instead of estimated from it.
//
// It exists because estimating it does not work. The launcher carried
// `LINE_H(font) = font * 1.31` for exactly one phase, fitted to one measurement
// of one face at one size and documented as "an estimate, deliberately on the
// generous side". Measured across the whole scale, the real ratio runs from
// 1.3182 at Caption (22px) down to 1.2529 at Display (170px) — FreeType rounds
// ascender and descent to whole pixels at each size, so the ratio wobbles and
// then falls away. The estimate over-reserved by 9.7 units at Display and, at
// Caption, UNDER-reserved: 28.82 against a true 29.00, so the one direction it
// promised never to fail in was the direction it failed in.
//
// Measured, and relied on here: the value depends on the SIZE alone. Neither the
// string nor the weight moves it (Body 40.00 regular, 40.00 bold, 40.00 for
// "Wgjq") — weight changes advance, not the face's vertical metrics.
float z_line_height(ZApp *app, ZFont size);

// Focus a field from code (NULL blurs whatever is focused), and ask whether a
// field currently holds focus. Tapping a TextField already does the first, so
// most apps never call these — they exist for the app that has to REACT to its
// own field's focus rather than just host it:
//
//   - The home screen is the one window the compositor sizes to the FULL output
//     rather than the usable area (it draws behind the bars), so the keyboard's
//     exclusive zone does NOT shrink it. Its search field has to inset the page
//     by the keyboard's height itself, which means asking whether it is focused.
//   - The same screen has to BLUR the field when you navigate away from it,
//     otherwise the keyboard stays up over a page that has no field on it.
void z_app_focus_field(ZApp *app, ZTextField *f);
bool z_app_field_active(ZApp *app, const ZTextField *f);

// ---------------------------------------------------------------------------
// Input method (the on-screen keyboard's back end) — P21.
//
// The keyboard app (zelto-keyboard) is the system's single input-method client.
// It binds the input method with z_im_bind and is told when to show/hide: the
// compositor raises the "show" callback when any app's text field is focused and
// "hide" when it blurs (driven by the text-input/input-method handshake). While
// shown, each key the user taps calls z_im_commit_text (insert) or z_im_backspace
// (delete one char before the cursor); the character lands in whatever field owns
// focus, in whatever app, with no cooperation from that app. Only the keyboard
// app calls these. See docs/platform/soft-keyboard.md.
// ---------------------------------------------------------------------------
typedef void (*ZImVisibilityCb)(ZApp *app, void *ud);

// Bind the input method and register show/hide callbacks. No-op (callbacks never
// fire) if the compositor does not advertise input-method-v2 or no seat is bound.
void z_im_bind(ZApp *app, ZImVisibilityCb on_show, ZImVisibilityCb on_hide,
               void *ud);

// Insert `utf8` into the focused text field (a committed string).
void z_im_commit_text(ZApp *app, const char *utf8);

// Delete one byte before the cursor in the focused field (backspace).
void z_im_backspace(ZApp *app);

// Delete `before` bytes before the cursor in one edit. What a held delete key
// switches to once it has been repeating long enough to mean "get rid of that
// word" — as ONE delete_surrounding_text rather than N backspaces, so the field
// sees a single edit and the keyboard is not racing its own surrounding-text
// updates while it decides how much is left to remove.
void z_im_delete(ZApp *app, int before);

// What KIND of field has focus (text-input-v3's content purpose, relayed to the
// input method by the compositor). The keyboard reads it to decide what it is
// allowed to do: a PASSWORD field must not get adaptive touch targets, must not
// auto-capitalise and must not learn from what is typed into it, because none of
// those helps when the text is not a word and all of them leak it.
//
// NORMAL is the default and also what is reported when no field is focused or
// the client never set a content type; OTHER is every purpose this system does
// not treat specially (digits, url, email, ...), reported so a caller can tell
// "not password" from "not told".
typedef enum ZImPurpose {
    Z_IM_PURPOSE_NORMAL = 0,
    Z_IM_PURPOSE_PASSWORD,
    Z_IM_PURPOSE_OTHER,
} ZImPurpose;
ZImPurpose z_im_purpose(ZApp *app);

// Does the focused field want its sentences capitalised (content hint
// AUTO_CAPITALIZATION)? False when no field is focused or the app never asked.
bool z_im_autocap(ZApp *app);

// The focused field's text as the APP last reported it, and where the cursor is
// in it (byte offset). This is the surrounding-text half of text-input-v3, which
// the compositor has been relaying since P21 and nobody read: it is the only way
// the keyboard can know what has already been typed, which is what
// auto-capitalisation, the double-space period and the prediction context are all
// rules about. Returns "" with *cursor = 0 when the app is not sending it.
//
// It is the app's text, not the keyboard's echo of its own keystrokes: a paste, a
// caret move or a programmatic edit all show up here and none of them go through
// a key cap.
const char *z_im_surrounding(ZApp *app, int *cursor);

// ---------------------------------------------------------------------------
// Clipboard + text selection (P22).
//
// The system clipboard is the standard Wayland CLIPBOARD selection (wl_data_device):
// Copy/Cut place text on it; Paste reads it back — so text copied in one app
// pastes into a field in another, with neither app knowing about the other.
//
//   z_clipboard_set(text)      Take ownership of the clipboard, offering `text` as
//                              text/plain. Creates a wl_data_source and sets the
//                              seat selection (needs a recent input event serial,
//                              which the framework tracks — call it from a tap
//                              handler). Copies `text`.
//   z_clipboard_get(cb, ud)    Read the current clipboard asynchronously. The
//                              offer's data is piped from the source client; the
//                              read fd is parked in the app loop (like a permission
//                              reply) and `cb` fires with the text once complete —
//                              nothing blocks the render loop. `text` is NULL/empty
//                              when the clipboard is empty or holds no text. Reads
//                              go through wlr-data-control so a focus-less client
//                              (the on-screen keyboard) can paste too; writes use
//                              the core wl_data_device selection. One get at a time.
//
// Selection lives on a focused ZTextField (anchor+caret byte range). The gestures
// are wired into TextField automatically: a tap focuses and moves the caret,
// long-press selects the word under the finger, and a drag extends the selection.
// A small floating action bar (z_selection_bar) acts on the selection. These
// operate on the app's currently-focused field.
// ---------------------------------------------------------------------------
typedef void (*ZClipboardCb)(ZApp *app, const char *text, void *ud);

void z_clipboard_set(const char *text);
void z_clipboard_get(ZClipboardCb cb, void *ud);

// Selection actions on the focused field. Copy/Cut require a selection (no-op
// without one); Paste inserts the clipboard at the caret, replacing any selection;
// Select-all selects the whole field. Copy keeps the selection; Cut collapses it.
void z_field_copy(ZApp *app);
void z_field_cut(ZApp *app);
void z_field_paste(ZApp *app);
void z_field_select_all(ZApp *app);

// The floating Copy / Cut / Paste / Select-all action bar for the focused field.
// Returns a ZView to drop into your body() (e.g. pinned above the field) while a
// selection is active, or NULL when there is no selection to act on — so
// `z_selection_bar(app)` can be listed directly as an optional child (a NULL child
// terminates a stack's list, so guard it or place it in its own slot). Built from
// ordinary Buttons wired to the z_field_* actions above.
ZView z_selection_bar(ZApp *app);

// ---------------------------------------------------------------------------
// Modifiers. Each wraps a ZView and returns it (apply outermost-last).
// ---------------------------------------------------------------------------
ZView Background(ZColor color, ZView view);
ZView Foreground(ZColor color, ZView view);
ZView Padding(float all, ZView view);
ZView Frame(float width, float height, ZView view);
ZView CornerRadius(float radius, ZView view);

// Confine everything this view's SUBTREE paints to the view's own frame, rounded
// by `radius` (0 = a plain rectangular clip). CornerRadius and Clip are the two
// halves people reach for interchangeably and they are not the same thing:
// CornerRadius rounds the node's OWN paint — its fill, its background, its image
// mask — and says nothing about its children, which happily draw past the corner.
// Clip is the mask, and it applies to the descendants.
//
// Reach for it when a child's silhouette has to be the PARENT's rather than its
// own. The canonical case is a fill that grows inside a rounded slab: the
// Control Center's brightness slab, a progress bar in a pill, artwork bled to a
// card's edge. Rounding the child instead is the trap — it rounds all four
// corners, including the ones in the middle of the slab where the fill's head is
// supposed to be a straight cut, so a half-full slider looks like a lozenge
// floating inside a slot.
//
//   Clip(Z_RADIUS_PANEL,
//       Frame(w, h, ZStack(fill_grown_to_value, glyph)))
//
// The clip nests: a rounded clip inside another intersects with it (each masks
// what the one above it left), so a clipped card inside a clipped scroll viewport
// does the right thing. Cost is per-pixel and only along the rounded edge — the
// straight interior takes the same path as an unclipped node.
ZView Clip(float radius, ZView view);

ZView Font(ZFont size, ZView view);

// Grow shares out the SLACK — whatever main-axis room is left after every child
// has been measured at its own content size. So a Grow child's final size is
// still its content plus a share, and two Grow(1) siblings come out EQUAL ONLY
// IF THEY CONTAIN THE SAME THING.
//
// Share divides the whole axis instead: the child's own intrinsic main-axis size
// is dropped, and the weights alone decide. Two Share(1) siblings are the same
// size whatever is inside them. (Grow is CSS `flex-grow: N`; Share is `flex: N`,
// i.e. flex-grow with flex-basis: 0.)
//
// Reach for Share whenever the row is a GRID and the content is a label that
// happens to sit in it — a keyboard's key caps, a segmented control, a tab bar.
// P46 found the on-screen keyboard's caps built with Grow, so every cap was as
// wide as its own letter: 'w' 69 units against 'i' 49 in the same row, and
// latching shift re-measured the uppercase glyphs and MOVED EVERY KEY IN THE ROW
// under the finger. Nothing looked broken, because a keyboard of slightly
// mis-sized keys still looks like a keyboard.
ZView Grow(float weight, ZView view);
ZView Share(float weight, ZView view);

// Set the text weight (the emphasis axis of the type scale). Heavier weights are
// a synthesised faux-bold (the bundled face is Regular-only), kept modest so text
// stays legible. No-op on a non-Text view; Regular is the default.
ZView Weight(ZWeight weight, ZView view);

// Cast a soft drop shadow behind this view — the toolkit's elevation cue. Pass
// an elevation token (Z_ELEV_1/2/3, the shadow blur in px); the renderer paints
// a blurred rounded-rect penumbra (matching the view's own corner radius, offset
// slightly downward as if lit from above) just before the view's fill, so a card
// / panel / modal / lifted item reads as floating above what it sits on. Apply
// it to the raised node itself (the one carrying the Background + CornerRadius);
// a shadow with no fill behind it still draws (an ambient ring). 0 = flat.
ZView Shadow(float elevation, ZView view);

// Draw a dark, slightly-offset copy of this text under its ink so a light label
// stays legible over a bright or busy backdrop (a home icon caption over the
// wallpaper) without a full scrim behind it. No-op on a non-Text view.
ZView TextShadow(ZView view);
// Expand this view to fill its parent's inner box. A depth stack (ZStack)
// otherwise centres each child at its own content size; Fill is how a
// full-screen layer (e.g. a wallpaper, or a slide-up sheet) covers the stack.
ZView Fill(ZView view);

// Fade this view AND everything under it by an opacity multiplier in [0,1] (1 =
// fully opaque, the default; 0 = invisible). The software renderer multiplies the
// subtree's effective alpha as it descends, so one Opacity fades a whole card,
// panel, or screen at once — the fade half of a slide+fade entrance/exit and the
// Navigator cross-fade. There is no separate animated variant: bind it to a spring
// by passing z_animated_get(v) (body rebuilds each frame while the spring is in
// flight, so the fade tracks it). Not group-correct for overlapping translucent
// children (each leaf is multiplied independently), which is fine for opaque cards.
ZView Opacity(float amount, ZView view);

// Gesture / input modifiers. OnTap makes any view tappable; OnKey makes it a
// keyboard target (keys are delivered to the first focusable view in the tree).
ZView OnTap(ZAction action, ZView view);
ZView OnKey(ZKeyAction action, ZView view);
// Tap handler that carries a data pointer (e.g. the list item a row stands for).
ZView OnTapData(ZTapAction action, void *data, ZView view);

// ---------------------------------------------------------------------------
// Gestures: pan / drag.
//
// A pan recognizer fires as the pointer drags across a view. A press that moves
// less than the slop threshold stays a tap (OnTap); once it crosses the slop it
// becomes a pan and the tap is cancelled. Inside a Scroll/List vertical drags
// scroll automatically; OnPan is for custom drags (swipe-to-dismiss, sliders).
// translation_* is the delta since the gesture began; velocity_* is in px/s
// (used for fling). Handlers are named functions, like ZAction.
// ---------------------------------------------------------------------------
typedef enum ZPanPhase {
    Z_PAN_BEGIN = 0,
    Z_PAN_CHANGED,
    Z_PAN_END,
} ZPanPhase;

typedef struct ZPanEvent {
    float x, y;                       // current pointer pos (surface-local)
    float translation_x, translation_y;
    float velocity_x, velocity_y;     // px/s, valid at Z_PAN_END
    ZPanPhase phase;
} ZPanEvent;

typedef void (*ZPanHandler)(ZApp *app, void *state, const ZPanEvent *e);

ZView OnPan(ZPanHandler handler, ZView view);

// OnPan with a per-view data pointer, the pan counterpart of OnTapData. A plain
// ZPanHandler only receives the app state, so every pannable view in one app
// shares one handler and cannot tell which of them the finger grabbed — fine for
// a C app (one named function per drag), useless for a runtime that maps each
// view to a different closure (Zelto Script) or for data-driven rows. `data` is
// bound at build time; point it at stable state, never at the arena. Like the
// handler, it is cached at the slop-cross, so it survives rebuilds mid-gesture.
typedef void (*ZPanDataHandler)(ZApp *app, void *state, void *data,
                                const ZPanEvent *e);

ZView OnPanData(ZPanDataHandler handler, void *data, ZView view);

// Long-press: fires when a press is held in place past a time threshold without
// crossing the pan slop. It composes with OnTap and OnPan on the same subtree —
// moving past the slop first becomes a pan (no long-press), a quick release
// stays a tap, and a still hold past the threshold fires the long-press and
// suppresses the tap that the release would otherwise have produced. `data` is a
// per-view pointer bound at build time (like OnTapData — point it at stable
// state); `x`/`y` are the surface-local press position (e.g. to place a menu).
typedef void (*ZLongPressHandler)(ZApp *app, void *state, void *data, float x,
                                  float y);

ZView OnLongPress(ZLongPressHandler handler, void *data, ZView view);

// ---------------------------------------------------------------------------
// Scrolling.
//
// Scroll wraps a single content child and clips it to the viewport, translating
// it by a persistent offset that responds to the pointer wheel and to vertical
// pan drags (with momentum/spring settling on release). For long, data-driven
// content use List, which builds only the rows in (and just around) the viewport
// — virtualised by a fixed row_height — and keys rows so the reconciler reuses
// them across scrolls. Both take `app` first (they allocate a retained ZScroll
// cell from the current screen).
// ---------------------------------------------------------------------------
typedef struct ZScrollOpts {
    ZView children[Z_MAX_CHILDREN];   // MUST be first; only children[0] (content) is used
    ZAxis axis;                       // Z_AXIS_VERTICAL (default) or _HORIZONTAL
} ZScrollOpts;

ZView z_scroll_view(ZApp *app, const ZScrollOpts *opts);
#define Scroll(appp, ...) z_scroll_view(appp, &(ZScrollOpts){__VA_ARGS__})

// A row's stable identity and its view, given the item and its index. `data` is
// the array base and `stride` its element size, so any contiguous array works:
// item i lives at (const char *)data + i*stride.
typedef uint64_t (*ZKeyFn)(const void *item, int index);
typedef ZView (*ZRowFn)(ZApp *app, const void *item, int index);

typedef struct ZListOpts {
    const void *data;
    size_t stride;
    int count;
    float row_height;                 // fixed row height (required for virtualisation)
    ZKeyFn key;
    ZRowFn row;
} ZListOpts;

ZView z_list(ZApp *app, const ZListOpts *opts);
#define List(appp, ...) z_list(appp, &(ZListOpts){__VA_ARGS__})

// Programmatic scroll handle (e.g. scroll-to-top). z_scroll allocates/fetches
// the current screen's next retained scroll cell, matching the one a Scroll/List
// built in the same position consumes.
ZScroll *z_scroll(ZApp *app);
void z_scroll_to(ZScroll *sc, float x, float y, bool animated);

// ---------------------------------------------------------------------------
// Animation.
//
// Motion is spring-based. An animated value is a persistent scalar you bind to a
// transform with Offset(); z_animated_spring drives it toward a target and the
// framework advances it on the surface frame callback (continuous repaint while
// in flight, idle once settled). z_with_animation runs a state change under a
// chosen spring (the implicit-animation default for values created within).
// ---------------------------------------------------------------------------
// Named motion tokens — the OS's spring vocabulary (P31, Motion.md). Pick by
// ROLE, not by tuning numbers, so the whole system moves in one consistent
// language and a retune happens in one place (spring_params in animation.c):
//   STANDARD  the default settle — sheets, drawers, screen slides, list reflow
//   SNAPPY    stiffer/quicker — carousel page flips, task-switch, decisive moves
//   PRESS     very quick, tight — the touch-down highlight (grab attention fast,
//             release cleanly); used by the global press-feedback spring
// Under Reduce Motion every profile collapses to an instant jump (Accessibility).
typedef enum ZSpring {
    Z_SPRING_STANDARD = 0,            // default: settles smoothly
    Z_SPRING_SNAPPY,                  // stiffer, quicker
    Z_SPRING_PRESS,                   // tight + fast: press/tap feedback
} ZSpring;

ZAnimated *z_animated_value(ZApp *app, float initial);

// A retained animated value keyed by an explicit IDENTITY (uint64_t), not by call
// order. Unlike z_animated_value (which maps the Nth call in a body to the Nth
// cell), this looks the cell up by `key`, so a value keeps its identity across
// rebuilds even when its position in the body changes every frame — the correct
// primitive for a reorderable list where each item must carry its own spring. A
// cell not requested during a build is garbage-collected, so removed items free
// their cell. `initial` is used only on the first request for a given key.
ZAnimated *z_animated_keyed(ZApp *app, uint64_t key, float initial);

void z_animated_set(ZAnimated *v, float to);        // jump (no animation)
void z_animated_pin(ZAnimated *v, float to);        // jump, without waking the loop
void z_animated_spring(ZAnimated *v, float to);      // spring toward `to`
// Spring toward `to` under an EXPLICIT motion token, independent of the ambient
// z_with_animation profile — the way to pick the role-named curve (SNAPPY for a
// decisive page flip, PRESS for touch feedback) at a single call without wrapping
// it in a z_with_animation block. Honours Reduce Motion (collapses to a jump).
void z_animated_spring_with(ZAnimated *v, float to, ZSpring spring);
// Spring toward `to` with an initial VELOCITY injected — the continuous hand-off a
// gesture release needs. A surface dragged 1:1 should re-fling from the finger's
// live velocity (px/s in the value's units) rather than easing from rest, and a
// spring grabbed mid-flight should carry its momentum forward on release. This is
// how an interruptible, physically-continuous drag settles (P33). Honours Reduce
// Motion (collapses to a jump — a direct-manipulation drag can't be reduced, but
// its RELEASE animation can).
void z_animated_spring_velocity(ZAnimated *v, float to, ZSpring spring,
                                float velocity);
// Grab a (possibly mid-flight) spring: stop it evolving on its own and return its
// current value, so a finger touching a moving surface takes control from where it
// is — no jump, no ignored touch. Drive it 1:1 with z_animated_set from here, then
// hand it back with z_animated_spring_velocity on release. Velocity is preserved.
float z_animated_grab(ZAnimated *v);
float z_animated_get(const ZAnimated *v);            // current value
float z_animated_target(const ZAnimated *v);         // where it's springing to
bool z_animated_active(const ZAnimated *v);          // still springing?

// Rubber-banding at a limit (Motion.md — resist, don't hard-stop). When a drag
// pushes a value PAST a boundary (the top of a scroll, a sheet dragged the wrong
// way, an over-pulled shade), don't clamp it dead: let it move with diminishing
// returns so the boundary feels elastic, then snap back on release. `overshoot` is
// how far past the limit the raw drag went (signed); `dim` is the reference span
// the resistance is scaled against (usually the viewport/screen extent). Returns
// the DAMPED offset to actually apply past the limit — small for a big pull, never
// reaching `dim`. The standard iOS curve b(x)=x·c·d/(x·c+d), c=0.55, pure
// arithmetic (no libm). Sign-symmetric, so it works at either end of a range.
static inline float z_rubber_band(float overshoot, float dim) {
    if (dim <= 0.0f) {
        return overshoot;
    }
    const float c = 0.55f;
    float x = overshoot < 0.0f ? -overshoot : overshoot;
    float r = (x * c * dim) / (x * c + dim);
    return overshoot < 0.0f ? -r : r;
}

void z_with_animation(ZApp *app, ZSpring spring, ZAction change);

// Advance every retained spring/fling by dt seconds; returns true while anything
// is still in motion. The frame loop calls this each frame, so apps rarely need
// it — but a headless test can call it to step motion deterministically (e.g. to
// catch a mid-flight reorder in a still screenshot).
bool z_anim_tick(ZApp *app, float dt);

// Bind an animated value to a horizontal translation: the subtree is shifted by
// (z_animated_get(x), y). Use for gesture-driven drags and screen transitions.
ZView Offset(ZAnimated *x, float y, ZView view);

// Shift a subtree by a STATIC (x, y) in logical px — the 2-D counterpart to
// Offset when neither axis is animated. This is how an absolutely-placed grid
// (the home bento grid: square cells at computed (col,row) pixel positions
// inside one depth ZStack) positions each cell, since a plain ZStack only
// centres its children. Composes with Offset (both add into the node's
// translation), so an animated ghost can sit on top of a statically-placed cell.
ZView OffsetXY(float x, float y, ZView view);

// Bind an animated value to EACH axis of a subtree's translation — the 2-D
// counterpart to Offset (which animates x only). Either pointer may be NULL to
// leave that axis unshifted. The bento reorder uses this so a cell whose packed
// slot changes slides diagonally (both axes spring) rather than teleporting;
// pair it with z_animated_keyed cells so each item's spring survives reorders.
ZView OffsetXYAnimated(ZAnimated *x, ZAnimated *y, ZView view);

// ---------------------------------------------------------------------------
// Navigation.
//
// A Navigator owns a stack of screens and slides between them with the standard
// spring. A screen is a function returning a view; it reads the stack through
// z_navigation(app) and receives the props passed at push time (props must
// outlive the screen — pass a pointer into stable app state). Back is the system
// gesture (edge-swipe) or the Escape/Backspace key; both pop the top screen.
// ---------------------------------------------------------------------------
typedef ZView (*ZScreenFn)(ZApp *app, void *props);

typedef struct ZNavOpts {
    ZScreenFn root;
} ZNavOpts;

ZView z_navigator(ZApp *app, const ZNavOpts *opts);
#define Navigator(appp, ...) z_navigator(appp, &(ZNavOpts){__VA_ARGS__})

ZNav *z_navigation(ZApp *app);
void z_nav_push(ZNav *nav, ZScreenFn screen, void *props);
void z_nav_pop(ZNav *nav);

// How many screens are on the stack (1 = only the root). A C app knows its own
// stack depth, but a runtime hosting screens on behalf of a script does not: the
// back gesture and the Escape key pop WITHOUT going through z_nav_pop's caller,
// so a host that keeps per-screen state alongside the stack (Zelto Script keeps
// each screen's hooks in its own scope) needs to see that a screen was retired in
// order to drop it. Read it at the top of a build and truncate to match.
int z_nav_depth(ZNav *nav);

// Freeze the top screen's push/pop transition at `progress` (0 = fully off-screen
// right + transparent, 1 = fully present) with no further motion — the deterministic
// mid-transition hook for the screenshot harness (like z_animated_pin, but for the
// Navigator's slide+cross-fade). Test/tooling use; a normal app never calls it.
void z_nav_freeze_top(ZNav *nav, float progress);

// ---------------------------------------------------------------------------
// App entry + lifecycle.
// ---------------------------------------------------------------------------

// Body function: returns the view tree for the current state.
typedef ZView (*ZBodyFn)(ZApp *app, void *state);

// Implementation entry used by Z_APP. Connects to Wayland, runs the loop.
int z_app_main(void *state, ZBodyFn body, const char *title);

// Like z_app_main but also sets the xdg app_id (used by Z_APP_ID). The System UI
// launcher sets "os.zelto.launcher" so the compositor's Home chord can find it.
int z_app_main_id(void *state, ZBodyFn body, const char *title,
                  const char *app_id);

// Z_APP(StateType, bodyFn) generates main(). bodyFn has signature:
//   ZView bodyFn(ZApp *app, StateType *state);
#define Z_APP(T, BODYFN)                                                  \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        return z_app_main(&z__state, z__body_trampoline, #BODYFN);        \
    }

// Like Z_APP but with an explicit xdg app_id string.
#define Z_APP_ID(T, BODYFN, APP_ID)                                       \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        return z_app_main_id(&z__state, z__body_trampoline, #BODYFN,      \
                             (APP_ID));                                   \
    }

// ---------------------------------------------------------------------------
// Layer-shell role (System UI).
//
// A normal app maps an xdg_toplevel and is window-managed by the compositor. A
// layer-shell app (the status bar, a launcher background, a notification shade)
// instead anchors itself to a screen edge in a fixed layer with an optional
// exclusive zone that reserves space the compositor keeps clear of app windows.
// Same declarative body()/layout/paint/input loop; only the surface role differs.
// ---------------------------------------------------------------------------
typedef enum ZLayer {
    Z_LAYER_BACKGROUND = 0,   // wallpaper, behind everything
    Z_LAYER_BOTTOM,           // below app windows
    Z_LAYER_TOP,              // above app windows (status bar)
    Z_LAYER_OVERLAY,          // above everything (shade, lock screen)
} ZLayer;

// Anchor bitmask (matches the wlr-layer-shell anchor edges). Anchoring to two
// opposite edges stretches the surface along that axis.
enum {
    Z_ANCHOR_TOP = 1,
    Z_ANCHOR_BOTTOM = 2,
    Z_ANCHOR_LEFT = 4,
    Z_ANCHOR_RIGHT = 8,
};

typedef struct ZLayerOpts {
    ZLayer layer;             // which layer to live in
    uint32_t anchor;          // Z_ANCHOR_* bitmask (0 = centered)
    int32_t exclusive_zone;   // px reserved from the app area (-1 = ignore others)
    int32_t width, height;    // desired size; 0 on an axis = size from anchors
    bool keyboard;            // grab EXCLUSIVE keyboard focus (a modal dialog)
    // Opt out of Dynamic Type: this surface's type is fixed at the shipped size
    // whatever sys.text_size says. Equivalent to z_text_scaling_disable(), in
    // the declaration rather than in a body() — a surface that does not scale is
    // a property of the surface, and the opts literal is where its properties
    // are already written. See z_text_scaling_disable() for the three that set
    // it and why.
    bool fixed_type;
    // Margins (px) inset from the anchored edges, e.g. margin_top to float a
    // surface below the status bar instead of over it.
    int32_t margin_top, margin_right, margin_bottom, margin_left;
} ZLayerOpts;

// Run an app as a layer-shell surface. Returns when the surface is closed.
int z_layer_app_main(void *state, ZBodyFn body, const char *title,
                     const ZLayerOpts *opts);

// Z_LAYER_APP(StateType, bodyFn, layerOptsLiteral) generates main() for a
// layer-shell System-UI app, e.g.
//   Z_LAYER_APP(BarState, bar_body,
//       .layer = Z_LAYER_TOP, .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT |
//       Z_ANCHOR_RIGHT, .exclusive_zone = 40, .height = 40);
#define Z_LAYER_APP(T, BODYFN, ...)                                       \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        ZLayerOpts z__opts = {__VA_ARGS__};                               \
        return z_layer_app_main(&z__state, z__body_trampoline, #BODYFN,   \
                                &z__opts);                                \
    }

// Resize a layer-shell surface at runtime. A layer app's size is normally fixed
// at startup (the Z_LAYER_APP opts), but a surface whose footprint changes — the
// notification shade, which should occupy the top of the screen only while a
// banner is up and reserve/cover nothing when idle — re-requests its size here.
// Passing 0 on an axis lets the compositor size it from the anchors. No-op for
// an xdg (non-layer) app. The new size arrives via the next configure + repaint.
void z_layer_resize(ZApp *app, int width, int height);

// Change a layer surface's exclusive zone at runtime — the px it reserves from
// the app area. A surface that reserves space only some of the time (the on-
// screen keyboard, which reserves its height while shown and nothing while
// hidden, so app content shrinks to keep the focused field visible above it)
// toggles it here. Deduped; applied on the next commit. No-op for a non-layer app.
void z_layer_set_exclusive_zone(ZApp *app, int zone);

// Restrict the surface's INPUT region to a rectangle (surface-local px); pointer
// events outside it fall through to whatever is beneath. A layer surface that is
// visually full-screen but should only CATCH input in a sub-area when idle — the
// pull-down shade, input-transparent except a thin top grab strip until it is
// pulled open — uses this instead of resizing, so its footprint never changes
// mid-gesture (a surface resize races pointer delivery). Pass w<=0 or h<=0 to
// accept input across the WHOLE surface (the default). No-op for a non-layer app;
// applied on the next commit, and a no-op when the region is unchanged.
void z_layer_set_input_region(ZApp *app, int x, int y, int w, int h);

// Make the surface catch NO pointer input at all (an empty input region) — every
// event falls through to whatever is beneath. This is the OPPOSITE of passing
// w<=0/h<=0 above (which means the WHOLE surface). A visually full-screen overlay
// that must paint over the app yet steal none of its taps — the brightness-dim
// scrim (P19) — uses this. No-op for a non-layer app; deduped against the last
// request; applied on the next commit.
void z_layer_set_input_none(ZApp *app);

// ---------------------------------------------------------------------------
// Material — the blurred backdrop under a system surface (P37).
//
// A phone's system surfaces are not opaque panels: the shade, the dock, the
// keyboard and a modal sheet are MATERIALS — a translucent tint through which
// whatever is behind shows softly out of focus. A Wayland client cannot read the
// pixels behind its own surface, so it cannot blur them; z_backdrop asks the
// COMPOSITOR to do it (zelto-backdrop-v1). Declare the rectangle of your surface
// that is a material — surface-local, with the corner radius it is drawn at — and
// zcomp blurs the scene below it and composites the result directly beneath you.
// Then paint your own translucent tint (Z_COLOR_MATERIAL_*) and content over it,
// as usual. The blur strength and corner geometry are the compositor's, so every
// material in the OS matches; the client contributes only the region and the tint.
//
// Re-declare it from body() whenever the region moves (a shade panel being
// dragged); the call dedups, so an unchanged region costs nothing. Pass a width or
// height of 0 to remove the material (a closed shade, a hidden keyboard). A
// compositor that does not implement the protocol simply shows the tint alone, so
// the call is always safe.
//
//   z_backdrop(app, 0, panel_y, z_screen_width(app), panel_h, Z_RADIUS_SHEET);
void z_backdrop(ZApp *app, float x, float y, float w, float h, float radius);

// Toggle EXCLUSIVE keyboard interactivity on a layer surface at runtime. A layer
// app is normally created with a fixed keyboard mode (the Z_LAYER_APP `keyboard`
// opt), but a surface that becomes modal only some of the time — the lock screen,
// which must grab the keyboard while locked and hand it back to the app on unlock
// — flips it here. `exclusive` true routes the keyboard to this surface (a modal
// grab, like the consent dialog); false returns it to the front app toplevel. No-
// op for a non-layer app; deduped; applied on the next commit. See P8/P20.
void z_layer_set_keyboard(ZApp *app, bool exclusive);

// Current surface size in pixels (after the latest configure). A full-screen
// layer that sizes or translates itself — a slide-up app drawer offset by its
// own height — needs the height; both are read fresh inside body().
int z_app_width(ZApp *app);
int z_app_height(ZApp *app);

// Full output (screen) size in pixels, independent of this surface's own size.
// A layer surface that COLLAPSES its own footprint when idle (the pull-down
// shade, only a thin grab strip until pulled) still needs the whole screen
// height to lay out and size its expanded panel before it has grown — z_app_*
// would report only the collapsed strip. Falls back to the surface size if the
// output mode was not advertised.
int z_screen_width(ZApp *app);
int z_screen_height(ZApp *app);

// Request a rebuild (like setState). Schedules a new body()/layout/paint.
void z_invalidate(ZApp *app);

// Force the NEXT paint to repaint the whole surface instead of just the diffed
// damage rects. Call it from body() on a frame where a large subtree is moving
// under an Offset (a slide-up sheet, a drag): the per-rect partial path
// under-damages a big translated subtree and leaves stale pixels, so motion
// frames want a full repaint (the same path screen transitions already take).
void z_full_repaint(ZApp *app);

// Request the app to exit its loop.
void z_app_quit(ZApp *app);

// ---------------------------------------------------------------------------
// Lifecycle.
//
// An app moves between foreground (Active) and background (Inactive) as the user
// switches apps; it is Stopped when the compositor asks it to close. The state
// rides the standard xdg "activated" toplevel state the compositor broadcasts to
// exactly the front window — no custom protocol. Register a handler to save/
// restore work across transitions; the framework also repaints on every change
// so a body() that reads z_app_active() reflects the live state.
// See docs/platform/app-lifecycle.md.
// ---------------------------------------------------------------------------
typedef enum ZLifecycle {
    Z_LC_ACTIVE = 0,   // entered the foreground (xdg activated)
    Z_LC_INACTIVE,     // left the foreground (paused / another app in front)
    Z_LC_STOPPED,      // the compositor asked the app to close
} ZLifecycle;

typedef void (*ZLifecycleHandler)(ZApp *app, void *state, ZLifecycle ev);

// Register the lifecycle handler (one per app). `state` passed to it is the same
// pointer given to body(). Pass NULL to clear.
void z_on_lifecycle(ZApp *app, ZLifecycleHandler handler);

// True while the app is the foreground (activated) window.
bool z_app_active(ZApp *app);

// ---------------------------------------------------------------------------
// Idle notifications (ext-idle-notify-v1).
//
// The compositor reports user activity (any pointer/keyboard event) on the seat.
// A client registers an idle notification with a timeout; its `on_idled` fires
// once the seat has seen no activity for that long, and `on_resumed` fires on the
// next activity. This is how the idle -> dim -> lock -> off state machine
// (zelto-lock) is driven without polling: create one notification per threshold
// (dim / lock / off seconds), each firing at its own time; a single input resumes
// them all. Cancel a notification (e.g. to re-arm with a new timeout, or when the
// lock disarms the lifecycle) with z_idle_cancel. See docs/platform/idle-lock.md.
// ---------------------------------------------------------------------------
typedef struct ZIdle ZIdle;   // one registered idle notification (opaque)
typedef void (*ZIdleCb)(ZApp *app, void *ud);

// Register an idle notification firing `on_idled` after `timeout_ms` of no seat
// activity and `on_resumed` on the next activity. Returns NULL if the compositor
// does not advertise ext-idle-notify or no seat is bound yet. Free with
// z_idle_cancel (the ZIdle is heap-allocated, not arena — it outlives a build).
ZIdle *z_idle_notify(ZApp *app, int timeout_ms, ZIdleCb on_idled,
                     ZIdleCb on_resumed, void *ud);

// Cancel + free an idle notification. Safe on NULL.
void z_idle_cancel(ZIdle *idle);

// ---------------------------------------------------------------------------
// One-shot timer.
//
// Fire `cb` once, `ms` milliseconds from now, from the app loop (never a signal
// context — safe to touch app state + call z_invalidate). Unlike an idle
// notification this is NOT tied to seat activity: it counts down regardless of
// input, so it drives self-dismissing transient UI (e.g. the volume HUD that
// shows on a sys.volume change and hides ~1.5s later). One pending timer per app
// (a second z_after replaces the first); z_after_cancel disarms it. The app loop
// bounds its poll() on the deadline, so no busy-wait.
// ---------------------------------------------------------------------------
typedef void (*ZTimerCb)(ZApp *app, void *ud);
void z_after(ZApp *app, int ms, ZTimerCb cb, void *ud);
void z_after_cancel(ZApp *app);

// The monotonic clock the toolkit's own timers and springs run on (seconds since
// an arbitrary epoch, never jumping). For an app measuring an INTERVAL between
// two events — the keyboard's double-tap window on shift — where wall-clock time
// would be wrong across an NTP step and where a timer is the wrong shape because
// the second event may never come.
double z_now_seconds(void);

// ---------------------------------------------------------------------------
// Repeating tick (widget refresh cadence).
//
// Request a periodic rebuild at `ms` cadence — a heartbeat the framework wakes
// the loop for and z_invalidate()s (no busy-wait, and independent of the single
// one-shot z_after slot). Re-declared every build (call it from body()): the
// SHORTEST interval requested during a build wins, so several widgets with
// different cadences share ONE timer, and a build that requests none disarms it.
// This is what drives a clock widget's once-a-second tick without spinning the
// render loop (a settled frame only re-renders when the interval elapses, not
// every vsync). Use z_after for a one-shot; z_tick_every for a steady beat.
// ---------------------------------------------------------------------------
void z_tick_every(ZApp *app, int ms);

// ---------------------------------------------------------------------------
// Home-screen widgets.
//
// A widget is a self-contained glanceable card: a titled panel whose live
// content a callback builds, refreshed on a declared cadence the SDK drives.
// z_widget wraps the callback's view in standard chrome (a rounded, padded
// Surface card captioned with the title) and — when refresh_ms > 0 — arms a
// z_tick_every so the card re-renders on its own (a clock ticking; a glance
// re-reading a brokered value on a timer). The body callback gets the live app
// and the app's state pointer (the same one body() receives), so it reads state
// / brokered settings and returns the content view. The result composes with
// Frame/Background/Grow like any view, so a host (the launcher) can size and lay
// several out over the wallpaper. A widget that updates from a live source (a
// settings observer, a notification count) needs no cadence — leave refresh_ms 0
// and invalidate from the source; the cadence is only for wall-clock ticking.
// ---------------------------------------------------------------------------
typedef ZView (*ZWidgetFn)(ZApp *app, void *state);

typedef struct ZWidgetOpts {
    const char *title;     // caption atop the card (NULL/"" = untitled)
    ZWidgetFn body;        // builds the card's live content
    int refresh_ms;        // self-refresh cadence (0 = only on invalidate)
} ZWidgetOpts;

ZView z_widget(ZApp *app, const ZWidgetOpts *opts);
#define Widget(appp, ...) z_widget(appp, &(ZWidgetOpts){__VA_ARGS__})

// ---------------------------------------------------------------------------
// Permissions.
//
// Sensitive capabilities (camera, location, ...) are declared in the app's
// manifest and granted at runtime by the user. The grant is brokered by the
// zsysd system daemon over a unix socket; libzelto wraps it so the app never
// touches the IPC. z_perm_status is a quick synchronous query; z_perm_request
// may show a system consent dialog, so it is asynchronous — the reply (after the
// user answers) is delivered to the callback from the app loop, not inline.
// Re-requesting an already-granted permission returns immediately (no dialog).
// See docs/platform/permissions.md + docs/api-reference/c/platform.md.
// ---------------------------------------------------------------------------
typedef enum ZPermStatus {
    Z_PERM_GRANTED = 0,   // the user granted it (cached)
    Z_PERM_DENIED,        // denied (cached) or not declared in the manifest
    Z_PERM_PROMPT,        // declared, no decision yet — request to prompt
} ZPermStatus;

// Delivered the outcome of z_perm_request. `app`/`ud` are the live app and the
// pointer passed to z_perm_request (typically the app's state struct).
typedef void (*ZPermCallback)(ZApp *app, ZPermStatus status, void *ud);

// Current status of a permission for this app (synchronous round-trip).
ZPermStatus z_perm_status(const char *name);   // "camera", "location", ...

// Request a permission. If a decision is cached the callback fires almost
// immediately with it; otherwise the system shows a consent dialog and the
// callback fires once the user answers. Does not block the render loop.
void z_perm_request(const char *name, ZPermCallback cb, void *ud);

// ---------------------------------------------------------------------------
// App-to-app intents: deep links + share targets.
//
// An app hands content to another app without knowing which: z_open_url hands a
// URL to whichever installed app registered that scheme (a deep link), and
// z_share presents a share sheet of apps that accept the payload's MIME type.
// Both are brokered by zsysd, which resolves candidate handlers from manifests
// (the `[links] schemes=` and `[capabilities] share-targets=` fields), shows the
// System-UI chooser when there is a choice, then launches/activates the target
// and delivers the payload to it. The receiving app registers a handler that
// fires when an intent arrives (even if it had to be launched to handle it).
// See docs/platform/ipc-and-intents.md + docs/api-reference/c/platform.md.
// ---------------------------------------------------------------------------

// One item being shared. `mime` is its MIME type (e.g. "text/plain"); `text`
// carries the payload for text MIME types. (Binary payloads are Planned.)
typedef struct ZShareItem {
    const char *mime;
    const char *text;
} ZShareItem;

// Incoming-deep-link handler: the URL another app (or the system) opened on us.
typedef void (*ZUrlCb)(ZApp *app, const char *url, void *ud);

// Incoming-share handler: the items another app shared to us. `items` is valid
// only for the duration of the call (copy what you keep).
typedef void (*ZShareCb)(ZApp *app, const ZShareItem *items, int count,
                         void *ud);

// Hand a URL to the system: zsysd resolves the scheme to its registered handler
// app, launching or activating it, and fires that app's z_on_open_url. No-op if
// no app handles the scheme or the broker is unreachable.
void z_open_url(const char *url);

// Present the share sheet for `items`: zsysd resolves the apps that accept the
// MIME type, the System UI shows a chooser, and the picked app receives the
// payload via z_on_share_target (launched first if it was not running).
void z_share(ZShareItem *items, int count);

// Register this app's incoming-deep-link / incoming-share handlers (one each).
// A queued intent that arrived before the handler was set fires as soon as it
// is registered, so an app launched to handle an intent never misses it.
void z_on_open_url(ZApp *app, ZUrlCb cb, void *ud);
void z_on_share_target(ZApp *app, ZShareCb cb, void *ud);

// ---------------------------------------------------------------------------
// Notifications.
//
// An app posts a notification to the Zelto shade (heads-up banner) via the
// zsysd notification broker; it requires the `notifications` permission (the
// first post with no stored grant shows the system consent dialog, exactly like
// any other permission). A notification carries a title + body, an optional
// channel (importance grouping), an optional tap_route deep link delivered
// through z_open_url when the banner body is tapped, and optional action
// buttons. Tapping an action routes back to the poster's
// z_on_notification_action handler. Build with z_notify_new + the setters, then
// z_notify_post (a synchronous round-trip that returns the assigned id and frees
// the builder). See docs/guides/notifications.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// Channel importance (how prominently the shade presents the notification). The
// shade's per-channel tuning is Planned; the MVP records the channel and treats
// every banner as a heads-up card.
typedef enum ZImportance {
    Z_IMPORTANCE_MIN = 0,
    Z_IMPORTANCE_LOW,
    Z_IMPORTANCE_DEFAULT,
    Z_IMPORTANCE_HIGH,
} ZImportance;

// Opaque notification builder. Allocated by z_notify_new, mutated by the
// setters, consumed (and freed) by z_notify_post.
typedef struct ZNotification ZNotification;

// Register a channel (importance grouping). MVP: recorded by the broker; the
// per-channel importance routing in the shade is Planned.
void z_notify_define_channel(const char *id, const char *name, ZImportance imp);

// Build a notification. Copies title + body; returns a builder to configure and
// post. The MVP supports a single action button (the last z_notify_add_action
// wins).
ZNotification *z_notify_new(const char *title, const char *body);
void z_notify_set_channel(ZNotification *n, const char *channel_id);
void z_notify_set_tap_route(ZNotification *n, const char *url);   // deep link on body tap
void z_notify_add_action(ZNotification *n, const char *id, const char *title);

// Post the notification (synchronous: blocks until the broker — possibly after a
// consent prompt — assigns an id). Returns the global notification id, or -1 if
// the permission was denied / the broker is unreachable. Frees the builder.
int64_t z_notify_post(ZNotification *n);

// Cancel a posted notification by id (removes its banner from the shade).
void z_notify_cancel(int64_t id);

// Set the app-icon badge count (0 clears). Forwarding to the bar/shade is
// Planned; the broker currently records it only.
void z_notify_set_badge(int count);

// An action / body tap delivered back to the poster. action_id is NULL or empty
// when the notification body itself was tapped (the tap_route already routed via
// z_open_url); a non-empty action_id is the id of the action button tapped.
typedef struct ZNotifyActionEvent {
    int64_t notification_id;
    const char *action_id;
} ZNotifyActionEvent;

typedef void (*ZNotifyActionCb)(ZApp *app, const ZNotifyActionEvent *e,
                                void *ud);

// Register this app's notification-action handler (one per app). A queued action
// that arrived before the handler was set fires as soon as it is registered (an
// app launched to handle its own action never misses it).
void z_on_notification_action(ZApp *app, ZNotifyActionCb cb, void *ud);

// ---------------------------------------------------------------------------
// Notification shade sink (System UI).
//
// The shade is an ordinary libzelto OVERLAY layer-shell app that subscribes as
// the single notification sink. The broker pushes a notify_show for every posted
// notification (post-grant) and a notify_hide when one is cancelled/dismissed.
// On a banner body tap the shade opens the tap_route itself (z_open_url) and
// reports the tap so the broker drops the banner; on an action tap it reports
// the action so the broker routes it to the poster, then drops the banner.
// ---------------------------------------------------------------------------
typedef struct ZShownNotification {
    int64_t id;
    const char *app_id;
    const char *title;
    const char *body;
    const char *tap_route;
    const char *action_id;
    const char *action_title;
} ZShownNotification;

typedef void (*ZNotifyShowCb)(ZApp *app, const ZShownNotification *n, void *ud);
typedef void (*ZNotifyHideCb)(ZApp *app, int64_t id, void *ud);

// Subscribe this app as the notification sink (last subscriber wins). The
// callbacks fire from the app loop when the broker pushes a show/hide.
void z_notify_subscribe(ZApp *app, ZNotifyShowCb on_show, ZNotifyHideCb on_hide,
                        void *ud);

// Report a banner body tap (the shade drops the banner; the deep link is routed
// separately via z_open_url) or an action-button tap (routed to the poster).
void z_notify_report_tap(int64_t id);
void z_notify_report_action(int64_t id, const char *action_id);

// ---------------------------------------------------------------------------
// System settings.
//
// A single brokered source of truth for system toggles (Wi-Fi, mute, screen
// brightness, ...). zsysd owns the store, persists it across reboots, and pushes
// a live update to every observer when any value changes — so a toggle flipped
// in the Settings app recolours the shade's quick-settings chip without a
// reboot, and vice versa. Keys are namespaced under `sys.` (sys.wifi / sys.mute
// / sys.bright / sys.airplane / sys.brightness); values are strings (bools as
// "0"/"1"). get is a fast synchronous read; set persists + broadcasts. An
// observer registers a callback that fires from the app loop on every change
// (including ones it made itself — apply changes idempotently, the client never
// loops). See docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// A setting changed: `key` and its new `value` (both valid only for the call).
typedef void (*ZSettingsCb)(ZApp *app, const char *key, const char *value,
                            void *ud);

// Read a setting (fast synchronous round-trip). Returns the broker's value, or
// `fallback` when the key is unset / the broker is unreachable.
const char *z_setting_get_str(const char *key, const char *fallback);
int64_t     z_setting_get_int(const char *key, int64_t fallback);

// Write a setting: the broker persists it (durable across reboot) and broadcasts
// the change to every observer. Fire-and-forget (no reply).
void z_setting_set_str(const char *key, const char *value);
void z_setting_set_int(const char *key, int64_t value);

// Observe live setting changes: `cb` fires from the app loop whenever any value
// changes. One observer per app; pass NULL to stop. Both the shade and the
// Settings app observe at once (the broker fans out to all subscribers).
void z_settings_observe(ZApp *app, ZSettingsCb cb, void *ud);

// ---------------------------------------------------------------------------
// Persistent storage: preferences, files, and SQLite — all scoped to the app's
// private data directory under $ZELTO_DATA_DIR (/var/zelto), keyed by app_id.
//
// Each app gets /var/zelto/apps/<app_id>/{documents,cache}. Preferences live in
// a small key=value store inside documents; SQLite databases are ordinary files
// in documents; cache is the same shape but the system may evict it. The paths
// are created on first use. Storage is direct filesystem access (no zsysd
// round-trip) on a real writable disk, so it survives a reboot.
// See docs/guides/storage.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// A read result. ok == false on failure (missing file, denied); data is a
// heap buffer (NUL-terminated for convenience) the caller frees with free().
typedef struct ZBytes {
    void *data;
    size_t len;
    bool ok;
} ZBytes;

// A directory listing. Free with z_list_free. Each name is a basename within
// the listed directory (not a full path).
typedef struct ZList {
    char **items;
    int count;
} ZList;

void z_list_free(ZList *list);

// --- Preferences (small key/value settings) -------------------------------
bool        z_prefs_set_str(const char *key, const char *value);
const char *z_prefs_get_str(const char *key, const char *fallback);
bool        z_prefs_set_int(const char *key, int64_t value);
int64_t     z_prefs_get_int(const char *key, int64_t fallback);
bool        z_prefs_remove(const char *key);

// --- Files (paths relative to the app's private directory) ----------------
bool   z_file_write(const char *path, const void *data, size_t len);
ZBytes z_file_read(const char *path);          // .ok == false on failure
bool   z_file_delete(const char *path);
ZList *z_file_list(const char *dir);
char  *z_path_documents(const char *rel);      // persistent;  caller frees
char  *z_path_cache(const char *rel);          // evictable;   caller frees

// --- Database (SQLite) -----------------------------------------------------
typedef struct ZDatabase ZDatabase;            // opaque connection handle
typedef struct ZRows ZRows;                    // opaque cursor over a query

// A single bound parameter (int or text). Built via the z_args macro; users
// never construct these directly.
typedef struct ZArg {
    int is_text;          // 0 = integer (i), 1 = text (s)
    int64_t i;
    const char *s;
} ZArg;

// The parameter list passed to z_db_run / z_db_query. Built by z_args(...),
// which tags each argument by type (int vs. string) at the call site.
typedef struct ZArgs {
    int n;
    ZArg v[8];            // up to 8 bound parameters (MVP)
} ZArgs;

// Internal helpers behind the z_args macro (do not call directly).
ZArg  z_arg_int_(int64_t x);
ZArg  z_arg_str_(const char *x);
ZArgs z_args_make_(int n, const ZArg *v);

// Tag one argument by its C type. `+0` forces array-to-pointer decay so a string
// literal (char[N]) selects the text branch; integers fall through to default.
#define Z_ARG_(x) _Generic((x) + 0,           \
        char *:       z_arg_str_,             \
        const char *: z_arg_str_,             \
        default:      z_arg_int_)((x) + 0)

#define Z_CAT_(a, b) a##b
#define Z_CAT(a, b) Z_CAT_(a, b)

// Count 1..6 macro arguments. (We can't count zero: invoking a `...` macro with
// no arguments is itself ill-formed under -std=c17 -Wpedantic, so the no-binding
// case is spelled Z_NO_ARGS instead of z_args().)
#define Z_NTH6_(_1, _2, _3, _4, _5, _6, N, ...) N
#define Z_NARG1(...) Z_NTH6_(__VA_ARGS__, 6, 5, 4, 3, 2, 1)

// Map each argument through Z_ARG_ (trailing comma so the sentinel follows).
#define Z_MAP1(a) Z_ARG_(a),
#define Z_MAP2(a, b) Z_ARG_(a), Z_ARG_(b),
#define Z_MAP3(a, b, c) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c),
#define Z_MAP4(a, b, c, d) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d),
#define Z_MAP5(a, b, c, d, e) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d), Z_ARG_(e),
#define Z_MAP6(a, b, c, d, e, f) \
    Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d), Z_ARG_(e), Z_ARG_(f),

// z_args("Buy milk", 0) -> a ZArgs binding text then integer parameters (1..6).
// The trailing {0,0,0} keeps the compound-literal array non-empty (valid ISO C);
// z_args_make_ copies only the first Z_NARG1 entries. For a query/statement with
// no bound parameters, pass Z_NO_ARGS.
#define z_args(...)                                                     \
    z_args_make_(Z_NARG1(__VA_ARGS__),                                  \
                 (ZArg[]){Z_CAT(Z_MAP, Z_NARG1(__VA_ARGS__))(__VA_ARGS__){0, 0, 0}})

// No bound parameters (object-like, so it sidesteps the zero-arg variadic-macro
// restriction). Use where the docs show z_args(): z_db_query(db, sql, Z_NO_ARGS).
#define Z_NO_ARGS (z_args_make_(0, (const ZArg *)0))

ZDatabase *z_db_open(const char *name);              // <documents>/<name>(.db)
bool       z_db_exec(ZDatabase *db, const char *sql);          // schema / DDL
bool       z_db_run(ZDatabase *db, const char *sql, ZArgs args);   // writes
ZRows     *z_db_query(ZDatabase *db, const char *sql, ZArgs args); // reads
void       z_db_close(ZDatabase *db);

bool        z_rows_next(ZRows *r);                   // advance; false at end
int64_t     z_rows_int(ZRows *r, int col);
const char *z_rows_str(ZRows *r, int col);           // valid until next/free
void        z_rows_free(ZRows *r);

// ---------------------------------------------------------------------------
// Networking.
//
// Apps make asynchronous HTTP requests and open WebSockets, gated by the
// `network` permission (declared in the manifest; the first send with no stored
// grant shows the system consent dialog, like any permission). Nothing blocks
// the render loop: the socket is driven from the app loop and the callback fires
// once the response is complete. The MVP is plain HTTP/1.x over TCP to a numeric
// IPv4 host; DNS and TLS/https are Planned.
// See docs/guides/networking.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// A heap request builder + in-flight handle. Created by z_net_get/z_net_request,
// configured with the setters, consumed by z_net_send (which owns and frees it
// once the callback fires) or released by z_net_cancel.
typedef struct ZNetRequest ZNetRequest;

// A completed response, passed to the callback. Valid only for the duration of
// the call — copy what you keep. Read `status` (HTTP status code; 0 on a
// transport error) and `ok` (true for 2xx) directly; reach the body/headers
// through z_net_body / z_net_header. The trailing fields are framework-internal.
typedef struct ZNetResponse {
    int status;
    bool ok;
    char *raw;          // owns the whole response (headers + body); internal
    size_t raw_len;
    char *body;         // into raw (NUL-terminated); internal
    size_t body_len;
    char *hdr_end;      // points at the CRLFCRLF; internal
    char scratch[256];  // z_net_header return storage; internal
} ZNetResponse;

// Result callback: `res` is the response; `ud` is the pointer passed to send.
typedef void (*ZNetCallback)(ZNetResponse *res, void *ud);

// Build a GET / arbitrary-method request to `url` ("http://10.0.2.2:8080/x").
// Returns NULL on a malformed URL.
ZNetRequest *z_net_get(const char *url);
ZNetRequest *z_net_request(const char *method, const char *url);

// Add a request header / set the request body (both copied).
void z_net_set_header(ZNetRequest *r, const char *k, const char *v);
void z_net_set_body(ZNetRequest *r, const void *data, size_t len);

// Send asynchronously. Checks the `network` permission first (showing the
// consent dialog if undecided); on grant it connects, sends, and fires `cb` from
// the app loop when the response is complete (or on error, with res->status==0).
// Frees the request after the callback returns.
void z_net_send(ZNetRequest *r, ZNetCallback cb, void *ud);

// Cancel an in-flight (or unsent) request; no callback fires. Frees it.
void z_net_cancel(ZNetRequest *r);

// Response accessors, valid only until the callback returns. z_net_body returns
// the body bytes (NUL-terminated for convenience); z_net_header looks up a
// response header by name (case-insensitive), NULL if absent.
ZBytes      z_net_body(ZNetResponse *res);
const char *z_net_header(ZNetResponse *res, const char *name);

// Iterate every response header (name, value), in the order the server sent them.
// z_net_header answers "what is the content type"; this answers "what headers are
// there at all", which is what a runtime binding needs to hand a script a whole
// `res.headers` object without reaching into ZNetResponse's internal fields.
// Both name and value are valid only for the duration of the callback.
typedef void (*ZNetHeaderCb)(const char *name, const char *value, void *ud);
void z_net_headers(ZNetResponse *res, ZNetHeaderCb cb, void *ud);

// WebSockets (unfragmented text frames; binary/fragmented are Planned). Gated by
// the `network` permission (typically already granted via a prior z_net_send).
typedef struct ZWebSocket ZWebSocket;
typedef void (*ZWsMessageCb)(ZWebSocket *ws, const char *text, size_t len,
                             void *ud);

// Open a WebSocket to `url` ("ws://10.0.2.2:8081/echo"): performs the HTTP/1.1
// Upgrade handshake and parks the socket in the app loop. NULL on failure.
ZWebSocket *z_ws_open(const char *url);
void z_ws_on_message(ZWebSocket *ws, ZWsMessageCb cb, void *ud);
void z_ws_send(ZWebSocket *ws, const void *data, size_t len);  // masked text frame
void z_ws_close(ZWebSocket *ws);

// ---------------------------------------------------------------------------
// Sensors + location (P38).
//
// The device sensors (accelerometer, gyroscope, magnetometer, ...) and the GPS
// are streamed from the zsysd sensor source. A one-shot fix (z_loc_get) is a fast
// synchronous read; a continuous stream (z_sensor_open / z_loc_watch) parks a
// subscription that pushes samples to a callback from the app loop, at a
// requested refresh rate clamped to [Z_SENSOR_RATE_MIN, Z_SENSOR_RATE_MAX] Hz.
// Sensor access needs the `sensors` permission; location needs `location`
// (request it with z_perm_request before opening a stream, exactly like network).
// A denied capability streams nothing rather than aborting. See
// docs/system-apis/location.md.
// ---------------------------------------------------------------------------

// The device sensors Zelto exposes (a modern-phone set). Values are SI units:
// acceleration m/s^2, rotation rad/s, magnetic field microtesla, orientation
// degrees [azimuth,pitch,roll], light lux, proximity cm, pressure hPa, step
// counter a cumulative count. Unknown/absent sensors report .present == false.
typedef enum ZSensorType {
    Z_SENSOR_ACCELEROMETER = 0,
    Z_SENSOR_GYROSCOPE,
    Z_SENSOR_MAGNETOMETER,
    Z_SENSOR_ORIENTATION,
    Z_SENSOR_GRAVITY,
    Z_SENSOR_LINEAR_ACCEL,
    Z_SENSOR_ROTATION_VECTOR,
    Z_SENSOR_LIGHT,
    Z_SENSOR_PROXIMITY,
    Z_SENSOR_PRESSURE,
    Z_SENSOR_STEP_COUNTER,
    Z_SENSOR_COUNT,
} ZSensorType;

// Refresh-rate bounds. A request outside this range is clamped; Android's
// SENSOR_DELAY_FASTEST maps to the max (its contract is "device-dependent
// fastest", so a 60 Hz cap is compliant). Named presets mirror the Android
// delays for the andemu bridge.
#define Z_SENSOR_RATE_MIN     1
#define Z_SENSOR_RATE_MAX     60
#define Z_SENSOR_RATE_NORMAL  5
#define Z_SENSOR_RATE_UI      16
#define Z_SENSOR_RATE_GAME    50
#define Z_SENSOR_RATE_FASTEST 60

// One sensor reading. `n` is how many of `v[]` are meaningful (1 for scalar
// sensors like light/pressure, 3 for vectors); `t` is a monotonic timestamp (ms);
// `accuracy` is 0..3 (unreliable..high), mirroring Android's SensorEvent.accuracy.
typedef struct ZSensorSample {
    ZSensorType type;
    float v[3];
    int n;
    int accuracy;
    int64_t t;
} ZSensorSample;

// Static capability of a sensor: whether it is present and its rate bounds.
typedef struct ZSensorCaps {
    bool present;
    int min_hz;
    int max_hz;
} ZSensorCaps;

// A location fix. `ok` is false when the fix is unavailable (permission denied /
// no source). Angles in degrees, accuracy/altitude in metres, speed in m/s,
// bearing in degrees; `t` is a monotonic timestamp (ms).
typedef struct ZLocation {
    double lat, lng;
    float accuracy;
    float altitude;
    float speed;
    float bearing;
    int64_t t;
    bool ok;
} ZLocation;

// Stream callbacks, fired from the app loop. The sample/fix is valid only for the
// call (copy what you keep).
typedef void (*ZSensorCb)(ZApp *app, const ZSensorSample *s, void *ud);
typedef void (*ZLocationCb)(ZApp *app, const ZLocation *loc, void *ud);

// Static capability of a sensor type (no round-trip).
ZSensorCaps z_sensor_info(ZSensorType type);

// Open a sensor stream at `rate_hz` (clamped to [MIN,MAX]); `cb` fires per sample
// from the app loop. Returns a handle (>0) or 0 on failure / unknown sensor.
// Needs the `sensors` grant — a denied stream simply never delivers.
int z_sensor_open(ZApp *app, ZSensorType type, int rate_hz, ZSensorCb cb, void *ud);

// Change an open stream's rate, or close it. Closing fires no further callbacks.
void z_sensor_set_rate(int handle, int rate_hz);
void z_sensor_close(int handle);

// A single location fix (fast synchronous round-trip to the broker). `.ok` is
// false when the `location` grant is missing or there is no fix.
ZLocation z_loc_get(ZApp *app);

// Continuous location: `cb` fires from the app loop at ~`rate_hz` (clamped).
// Returns a handle (>0) or 0 on failure. Needs the `location` grant. Stop with
// z_loc_stop.
int z_loc_watch(ZApp *app, int rate_hz, ZLocationCb cb, void *ud);
void z_loc_stop(int handle);

// ---------------------------------------------------------------------------
// Task switcher (running apps).
//
// A client (the launcher) can list every other running app window and switch to
// or close it, over the standard wlr-foreign-toplevel-management protocol. The
// app's own window is filtered out by app_id. The list mutates as windows open,
// change title, gain/lose focus, and close; each change repaints the caller.
// ---------------------------------------------------------------------------
typedef struct ZTask {
    const char *title;    // window title (may be NULL until reported)
    const char *app_id;   // window app_id (may be NULL until reported)
    bool active;          // true if this is the foreground window
    void *handle;         // opaque foreign-toplevel handle (for activate/close)
} ZTask;

// A stable snapshot of the running apps (excluding this app's own window). Valid
// until the next z_running_apps call; read it fresh inside body(). *count gets
// the entry count.
const ZTask *z_running_apps(ZApp *app, int *count);

// Switch to / close a running app from the snapshot.
void z_task_activate(ZApp *app, const ZTask *task);
void z_task_close(ZApp *app, const ZTask *task);

// A picture of a running app's WINDOW, as it looked when it was last on screen.
//
// Returns a key to pass straight to Image() / Cover(), or NULL when no picture
// is available — which is the normal case for a window that has never been
// backgrounded, for the first frame after asking (the image arrives
// asynchronously and repaints the caller when it lands), and on any compositor
// that does not implement zelto-toplevel-capture-v1. So a caller must ALWAYS
// have a fallback; the app's icon is the intended one.
//
//   const char *shot = z_snapshot(app, task);
//   ZView poster = shot ? Cover(Image(shot)) : Image(icon_for(task->app_id));
//
// The image is deliberately STALE — it shows the window as it was, not as it is,
// because a backgrounded window is not being drawn. That is what makes it useful:
// it identifies the window far better than an icon can. It is also a reduced-
// resolution copy, so scale it to fit rather than assuming a size.
//
// Asking is what subscribes, so a client that never calls this costs nothing.
const char *z_snapshot(ZApp *app, const ZTask *task);

// ---------------------------------------------------------------------------
// Layout probe (P46) — drive a surface by IDENTITY instead of by coordinates.
//
// Every harness this project has thrown away died the same death: it tapped a
// number read off a screenshot, the layout moved, and the tap started landing on
// whatever was there instead — silently, because a tap that hits nothing still
// returns. The fix is not "stop testing surfaces", it is to stop being the one
// who decides WHERE a control is. The layout already knows: after arrange() every
// node carries its frame. So a test names the control by its HANDLER (which is
// code, and moves when the code moves) and the toolkit answers with the frame.
//
// z_probe_tap does three separate things and reports each, because they are three
// separate claims and only the first is about the test:
//
//   found    — a tappable node with this handler (and, for a data handler, this
//              data) exists in the laid-out tree at all. False = the test named
//              something that is not on screen; nothing below it means anything.
//   hit_same — the REAL hit walk, run at the centre of that frame, came back with
//              that same node. This is the claim a coordinate tap was always
//              making implicitly and never checking: that the control is where
//              the layout says AND that pressing there reaches it rather than
//              something painted over it.
//   ran      — the handler on the node THE HIT WALK FOUND was dispatched. It is
//              deliberately the hit node and not the resolved one: if the two
//              disagree, the wrong thing must actually happen, so the assertion
//              downstream (a character, a toggle) fails too rather than passing
//              on a technicality.
//
// Frames are surface pixels. Call it from a timer or an event callback, never
// from body() — it reads the tree the LAST build laid out, and inside body() that
// tree is the previous frame's, mid-teardown.
typedef struct ZProbeTap {
    bool found;
    float x, y, w, h;   // the resolved node's frame (surface px)
    bool hit_same;
    bool ran;
} ZProbeTap;

// Resolve by handler. Pass on_data + data for an OnTapData node (data is compared
// by pointer value, which is how a per-item handler is told apart from its
// siblings), or on_plain for an OnTap node. Exactly one of the two is used.
ZProbeTap z_probe_tap(ZApp *app, ZTapAction on_data, void *data, ZAction on_plain);

// Every tappable node in the laid-out tree, front-to-back, with its frame — for
// the assertions that are about the SET of controls rather than one of them
// (a touch-target audit, a "nothing overlaps" check). The handler pointers come
// back so the caller can name what it is looking at.
typedef void (*ZProbeVisitor)(void *ud, ZTapAction on_data, void *data,
                              ZAction on_plain, float x, float y, float w,
                              float h);
void z_probe_taps(ZApp *app, ZProbeVisitor fn, void *ud);

// Resolve a control's FRAME without pressing it. Same lookup as z_probe_tap and
// none of the consequences — for a caller that wants to aim somewhere OTHER than
// the centre (the keyboard's deliberate-miss harness) and must therefore ask
// where the centre is first.
ZProbeTap z_probe_frame(ZApp *app, ZTapAction on_data, void *data,
                        ZAction on_plain);

// What the PLAIN geometric hit walk finds at a point — the rectangles, and
// nothing else. No dispatch, and deliberately NOT routed through a tap resolver
// (below): this is the control that says what a press at this point would have
// done before anything clever was put in front of it. `found` false means the
// point is in a dead gutter and a real finger there would have done nothing.
typedef struct ZProbeHit {
    bool found;
    float x, y, w, h;
    ZTapAction on_data;
    void *data;
    ZAction on_plain;
} ZProbeHit;
ZProbeHit z_probe_at(ZApp *app, float x, float y);

// ---------------------------------------------------------------------------
// Tap resolver (P47) — a surface that decides for itself what a press meant.
//
// The default answer to "what did this press hit?" is the rectangle under it,
// and for almost everything that is right. It is wrong for a keyboard. Key caps
// are ~32pt wide because a ten-key row on a 390pt phone cannot make them wider,
// which is under the finger's own contact patch — so a keyboard that resolves
// presses by rectangle is a keyboard you have to aim at. The fix every usable
// phone keyboard has shipped since 2007 is to keep the ART fixed and let the
// TARGETS move: the region that yields 'l' grows after "hel" and the region that
// yields 'k' shrinks, without a pixel changing. Rectangles cannot express that
// (grow two neighbours and they overlap; grow neither and the gutter between them
// is dead), so the surface answers the question itself.
//
// The resolver is called on the release of a tap, with the surface-local point,
// BEFORE the default hit walk. Return true to say "I handled this"; return false
// to fall through to the ordinary rectangle dispatch. It is also called on the
// release that ends a LONG PRESS, which is what makes press-hold-slide-release
// (the accent popup) expressible — the default path swallows that release.
//
// One resolver per app. Passing NULL removes it.
typedef bool (*ZTapResolver)(ZApp *app, void *state, float x, float y);
void z_tap_resolver(ZApp *app, ZTapResolver fn);

// ---------------------------------------------------------------------------
// Press phases (P47) — the whole gesture, not just its end.
//
// A tap handler hears about a press once, when it is over. That is enough for a
// button and not enough for a keyboard, which has three things to do WHILE a
// finger is down: show the preview callout above it, notice that it has been
// there long enough to mean "hold", and follow it as it slides onto an accent in
// the popup that hold opened.
//
// The toolkit already has an OnLongPress recognizer (P16) and this is not a
// replacement for it: OnLongPress fires once, on a node, and tells you nothing
// about the finger before or after. A surface that owns its own press timing —
// because its keys repeat, accelerate and then change what they delete — needs
// the phases.
//
//   Z_PRESS_DOWN   a finger landed, at (x, y) in surface pixels
//   Z_PRESS_MOVE   it moved, still down (every motion event, not only past slop)
//   Z_PRESS_UP     it lifted. Fired BEFORE the tap path, so a surface can decide
//                  here whether the release still means a tap; a tap resolver
//                  that returns true then suppresses the ordinary dispatch.
//
// One hook per app. NULL removes it.
typedef enum ZPressPhase {
    Z_PRESS_DOWN,
    Z_PRESS_MOVE,
    Z_PRESS_UP,
} ZPressPhase;
typedef void (*ZPressCb)(ZApp *app, void *state, ZPressPhase phase, float x,
                         float y);
void z_press_hook(ZApp *app, ZPressCb fn);

// Drive one phase, exactly as the pointer listener does — what a test presses and
// holds with. Pair it with z_probe_press for the release: DOWN, wait, (MOVE,)
// then z_probe_press, which is the same UP-then-resolve the finger takes.
void z_probe_press_phase(ZApp *app, ZPressPhase phase, float x, float y);

// Drive the surface's tap path at a point, exactly as a finger's release does:
// the resolver first, the rectangle walk if it declines. This is what a test
// presses with — the same function the pointer listener calls, so there is no
// second code path to keep honest.
void z_probe_press(ZApp *app, float x, float y);

#ifdef __cplusplus
}
#endif // ZELTO_UI_H

#endif // ZELTO_UI_H  // ZELTO_UI_H
