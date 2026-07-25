// C API: Graphics (<zelto/gfx.h>)
//
// Low-level types shared by the UI toolkit and (eventually) custom-draw views.
// The MVP exposes colours and rectangles; the canvas/path/GPU-surface API
// documented in docs/api-reference/c/gfx.md is Planned and lands in a later phase.
// Conventions: docs/api-reference/conventions.md.
#ifndef ZELTO_GFX_H
#define ZELTO_GFX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Straight-alpha 8-bit-per-channel colour.
typedef struct ZColor {
    uint8_t r, g, b, a;
} ZColor;

// A rectangle in logical pixels.
typedef struct ZRect {
    float x, y, w, h;
} ZRect;

// Build a colour from RGBA components (0-255).
static inline ZColor z_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (ZColor){r, g, b, a};
}

// A black scrim at the given opacity — the modal/lock dimming layer. Not a
// palette hue (pure black + alpha), so it lives as a helper rather than a
// colour token; use it wherever a surface is darkened rather than tinted.
static inline ZColor z_scrim(uint8_t a) { return z_rgba(0, 0, 0, a); }

// The same colour at a different opacity. For the case where one token has two
// strengths and they are strengths of the SAME thing — the drag drop-target's
// empty slot and the placeholder under the icon in hand. Two tokens for that
// would be two things to retint and one of them would be forgotten.
static inline ZColor z_fade(ZColor c, uint8_t a) {
    c.a = a;
    return c;
}

// Linearly interpolate between two colours (t in [0,1]: 0 = a, 1 = b), each
// channel independently — the cross-fade a state-change animation drives (a
// toggle chip recolouring off->on on a spring-backed t rather than hard-swapping).
static inline ZColor z_color_lerp(ZColor a, ZColor b, float t) {
    if (t <= 0.0f) { return a; }
    if (t >= 1.0f) { return b; }
    return z_rgba(
        (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t + 0.5f),
        (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t + 0.5f),
        (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t + 0.5f),
        (uint8_t)((float)a.a + ((float)b.a - (float)a.a) * t + 0.5f));
}

// --- Design tokens — the Zelto design system -----------------------------
// A true-black base, a soft-white ink, and NO brand hue: the colour in the OS
// comes from the wallpaper and the app icons, never from the chrome. This is the
// quiet end of the phone-design spectrum (iOS's dark system palette), chosen so
// that the one thing that IS coloured on screen — an app's icon — is the thing
// the eye goes to. Von Restorff: an accent only accents if it is rare.
//
// This header is the single source of truth for the token NAMES and for what
// each one means; sdk/src/theme.c holds the VALUES, one column per appearance.
// Every system surface and app draws from these tokens (no ad-hoc hexes), so
// retinting the OS is a matter of editing that table.
//
// EVERY TOKEN IS A RUNTIME LOOKUP (P54). Until P54 they were compile-time
// constants and exactly four of them — z_ink_muted, z_ink_faint, z_hairline,
// z_on_fill — could change at runtime, because P51 needed those four for
// Increase Contrast and generalised nothing. A phone that cannot be light is
// not a themed phone, it is a dark phone. So the P51 shape is now the shape of
// the whole palette: each macro expands to `z_token(Z_TOKEN_*)`, which is a
// function call it already was (z_rgba is a static inline), and not one of the
// 121 migrated call sites changed or learned that an appearance exists.
//
// WHAT THAT COSTS AND WHY IT IS FINE: a token read is now an indexed load out of
// a static table instead of an immediate. The toolkit builds a tree per frame at
// 40Hz on a phone-sized surface; the palette is read a few hundred times per
// build against a keyboard prediction pass that was measured in HUNDREDS of
// microseconds. It is not on the list of things that cost anything here.
//
// A NOTE ON WHERE VALUES LIVE. Two appearances in one header, side by side in
// the comments, is how a palette drifts: the eye reads the column it is looking
// for. In theme.c the two are one table with two columns, which is the form a
// palette is actually reviewed in — and the WCAG table in test_contrast_tokens
// is computed over both of them, so neither column can quietly go wrong.
//
// SURFACES step AWAY FROM THE PAGE, from BG (the root) through SURFACE_3 (an
// input, a key, a chip); BORDER is the hairline between them. Which DIRECTION
// that is belongs to the appearance, not to the token: dark climbs from true
// black (on an OLED phone, the panel switched off) and light descends from white
// down Apple's systemGray ramp. What a step MEANS is the same either way, and
// that invariant is why the ladder stays monotone in both — see the note over
// the table in theme.c for the measurement that settled it.
//
// Ink is DELIBERATELY not pure white in dark: #f2f2f7 on #000 is still far past
// AA and is markedly easier to sit in front of for an hour than 21:1
// white-on-black. Light's #1c1c1e on #fff is the same choice mirrored.
//
// The interactive fill (PRIMARY) is the page's OWN POLARITY INVERTED, with
// ON_PRIMARY as its ink — near-white on black, near-black on white. An active
// toggle reads as "lit", the way a Control Center chip does, without introducing
// a hue. SUCCESS/WARN/DANGER stay coloured because their whole job is to be
// exceptional, each with a *_DIM panel fill for a state-tinted surface.

// --- The appearance -------------------------------------------------------
// Process-global and applied at startup and on change, exactly like the text
// size. A process draws one surface in this OS, so one global is one surface's
// appearance. z_theme_apply returns true when the value moved, so the caller
// knows a repaint is owed — and unlike Dynamic Type it is NOT gated on
// z_text_scaling_disable(): a colour swap moves no geometry, so the status bar,
// the keyboard and the home indicator take it too.
//
// THERE IS NO "AUTO", AND THAT IS A DECISION RATHER THAN AN OMISSION.
//
// The tempting one is the AMBIENT LIGHT SENSOR, which exists end to end in this
// OS already (Z_SENSOR_LIGHT, sim-scriptable through ZELTO_SIM_LIGHT). Three
// reasons it is not wired to the appearance:
//
//   1. NO PHONE DOES THIS, and not for want of a sensor. iOS's "Automatic" and
//      Android's are SCHEDULES — sunset to sunrise — not photometry. A dim
//      office at two in the afternoon is not a request for dark mode, and an
//      appearance that changes when someone walks past a window is worse than
//      one that never changes at all.
//   2. THE SENSOR HERE IS SYNTHETIC. zsysd fabricates the reading from an
//      environment variable (sensor_synth, default 300 lux) and re-reads it per
//      sample, so it is constant for the life of a process. A hysteresis band
//      guarding a constant is a mechanism no test in this repo could exercise —
//      it would be a number nobody ever crossed, which is how a threshold ends
//      up wrong for years.
//   3. A SCHEDULE NEEDS A REAL CLOCK AND A LOCATION and the OS has neither.
//
// What would have to become true first: a physical ambient sensor whose reading
// varies, or a location and a real-time clock. Until then two values, chosen.
typedef enum ZTheme {
    Z_THEME_DARK = 0,
    Z_THEME_LIGHT = 1,
} ZTheme;

bool z_theme_apply(ZTheme theme);
ZTheme z_theme(void);

// THE TOKENS, as an enumeration. The macros below are the way to say one of
// these; this enum exists so the table in theme.c can be indexed by it (a
// designated initialiser per token, so the table cannot silently depend on the
// order things are written in) and so a test can walk the whole palette.
typedef enum ZToken {
    Z_TOKEN_BG,
    Z_TOKEN_SURFACE,
    Z_TOKEN_SURFACE_2,
    Z_TOKEN_SURFACE_3,
    Z_TOKEN_SURFACE_4,
    Z_TOKEN_BORDER,
    Z_TOKEN_PRIMARY,
    Z_TOKEN_ON_PRIMARY,
    Z_TOKEN_ACCENT,
    Z_TOKEN_TEXT,
    Z_TOKEN_TEXT_MUTED,
    Z_TOKEN_TEXT_FAINT,
    Z_TOKEN_SUCCESS,
    Z_TOKEN_WARN,
    Z_TOKEN_DANGER,
    Z_TOKEN_SUCCESS_DIM,
    Z_TOKEN_WARN_DIM,
    Z_TOKEN_DANGER_DIM,
    Z_TOKEN_ACCENT_DIM,
    Z_TOKEN_SCRIM,
    Z_TOKEN_MATERIAL_THIN,
    Z_TOKEN_MATERIAL_REGULAR,
    Z_TOKEN_MATERIAL_THICK,
    Z_TOKEN_MATERIAL_SHEET,
    Z_TOKEN_MATERIAL_EDGE,
    Z_TOKEN_SHADOW,
    Z_TOKEN_PRESS,
    Z_TOKEN_PRESS_INV,
    Z_TOKEN_DROP_TARGET,
    Z_TOKEN_COUNT,
} ZToken;

// The palette, resolved for the appearance this process is drawing in and for
// whether Increase Contrast is on. Out-of-range returns opaque magenta rather
// than reading past the table: a token that does not exist should be the
// loudest thing on the screen, not black-on-black.
ZColor z_token(ZToken token);

// Base surfaces (deepest -> highest elevation).
#define Z_COLOR_BG         z_token(Z_TOKEN_BG)         // root background
#define Z_COLOR_SURFACE    z_token(Z_TOKEN_SURFACE)    // raised panel
#define Z_COLOR_SURFACE_2  z_token(Z_TOKEN_SURFACE_2)  // card / row
#define Z_COLOR_SURFACE_3  z_token(Z_TOKEN_SURFACE_3)  // input / key / chip
// One step further from the page again, for a control that must stand OFF a
// surface which is itself already raised — the character caps on the keyboard's
// material, where SURFACE_3 sits too close to the field of keys to read as a key.
#define Z_COLOR_SURFACE_4  z_token(Z_TOKEN_SURFACE_4)  // key cap on a panel
#define Z_COLOR_BORDER     z_token(Z_TOKEN_BORDER)     // hairline / divider

// The interactive fill. Not a hue — a light, "lit" surface. Text and glyphs on it
// use ON_PRIMARY (dark), NOT TEXT_INV.
#define Z_COLOR_PRIMARY    z_token(Z_TOKEN_PRIMARY)    // filled action / active
#define Z_COLOR_ON_PRIMARY z_token(Z_TOKEN_ON_PRIMARY) // ink ON a PRIMARY fill
#define Z_COLOR_ACCENT     z_token(Z_TOKEN_ACCENT)     // highlight / active glyph

// Text. NOT all AA, and the comment that used to say so was the reason nobody
// looked — see the measured table below.
#define Z_COLOR_TEXT       z_token(Z_TOKEN_TEXT)       // primary
#define Z_COLOR_TEXT_MUTED z_token(Z_TOKEN_TEXT_MUTED) // secondary / caption
#define Z_COLOR_TEXT_FAINT z_token(Z_TOKEN_TEXT_FAINT) // de-emphasised / disabled

// Z_COLOR_TEXT_INV IS GONE (P54), and how it died is worth keeping.
//
// It was documented as "the ink on a SEMANTIC (coloured) fill" — #f4f4f8, a
// near-white. Every one of its fifteen call sites was something else: a page
// TITLE drawn straight onto Z_COLOR_BG (Sensors, Notepad, Store, Fetch, Pinger,
// Widget, the share sheet), or body ink on a *_DIM panel. Not one of them was
// the thing the name described.
//
// In a dark-only OS that was invisible, because TEXT_INV #f4f4f8 and TEXT
// #f2f2f7 are the same colour to within 2/255. A light appearance is what makes
// them opposites, and every one of those titles would have been white on white —
// the exact failure mode this phase is about, in the theme nobody screenshots.
//
// So: a title on a page is Z_COLOR_TEXT, and ink on a fill is z_on_fill(fill),
// which computes the answer instead of naming it. Nothing needs a third ink.

// THE DRAG DROP TARGET — the one hue in the OS, and the one place the "no brand
// hue" rule argues FOR a colour rather than against it.
//
// This was a raw #4aa3ff in system/launcher/main.c, at two alphas, and it is the
// only hue in the shipped product that was in no table. The rule it appeared to
// break is at the top of this file: the colour in the OS comes from the wallpaper
// and the app icons, never from the chrome. But the argument that rule rests on
// is Von Restorff — an accent only accents if it is RARE — and this mark is the
// rarest thing the OS draws. It exists for the seconds a finger is holding an app
// icon, over a wallpaper, to say "it can go here". Its predecessor was a white
// ghost at 8% and the comment beside it records why that was replaced: on a busy
// wallpaper it was easy to miss, which for a drop target is the whole failure.
//
// So it is KEPT and NAMED rather than deleted, and naming it is what makes it
// theme-able — a hardcoded azure at 19% alpha over a light wallpaper is a smear.
#define Z_COLOR_DROP_TARGET z_token(Z_TOKEN_DROP_TARGET)

// ---------------------------------------------------------------------------
// INCREASE CONTRAST (P51) — the three tokens with two values, and the numbers.
//
// THE SHIPPED PALETTE, MEASURED rather than eyeballed. WCAG 2.1 relative
// luminance over every ink/surface pair this OS actually draws (the ratios are
// computed from the tokens themselves in test_contrast_tokens.c, so this table
// cannot drift from the values above it):
//
//                     BG      SURFACE  SURFACE_2  SURFACE_3
//     TEXT           18.82    15.25     12.49      10.17     all AA
//     TEXT_MUTED      8.18     6.63      5.43       4.42     LAST ONE FAILS AA
//     TEXT_FAINT      4.02     3.25      2.67       2.17     THREE FAIL AA
//
// AA is 4.5:1 for body text and 3:1 for large text. TEXT_FAINT — the disclosure
// chevron on every Settings row, the disabled detail column — is 2.67:1 on a
// card and 2.17:1 on a chip. It does not reach even the LARGE-text threshold on
// two of the four surfaces it is drawn on, and at the smallest text size it is
// small grey type on a grey card: the two accessibility problems compounding.
//
// (P51 also listed two semantic failures here and left them, calling them a
// palette change rather than a preference. That was right about the scope and
// WRONG about the pairs — see the measured note over Z_COLOR_SUCCESS below.
// ON_PRIMARY on SUCCESS is not a combination this OS draws; the one it did draw,
// TEXT_INV on WARN, was 1.99:1, and the green's real failure was in its OTHER
// role, as ink. P52 closed all of it: the semantic set is iOS's dark trio and
// the ink on a fill is computed by z_on_fill.)
//
// WHAT INCREASE CONTRAST DOES. The three tokens that carry the failures get a
// second value, chosen as the LEAST brightening that clears AA on the darkest
// surface each is drawn on — not "as light as possible", because the whole job
// of a muted ink is to be a step below the primary one, and a preference that
// flattens the hierarchy has substituted one unreadable screen for another:
//
//     TEXT_MUTED  #a1a1a8 -> #c7c7cc   (4.42 -> 6.74 on SURFACE_3)
//     TEXT_FAINT  #6c6c70 -> #a8a8b0   (2.17 -> 4.81 on SURFACE_3)
//     BORDER      #38383a -> #6c6c70   (1.40 -> 3.25 on SURFACE; a hairline is
//                                       not text, so its bar is WCAG's 3:1
//                                       non-text minimum)
//
// TEXT (#f2f2f7) and the surfaces themselves do not move. TEXT already passes
// everywhere, and moving a surface would move every other pair with it.
//
// THE SEAM IS THE SAME SHAPE AS z_font_units(). These are macros that already
// expand to a function call (z_rgba is a static inline), so 121 migrated call
// sites keep working unchanged and none of them learns a setting exists. This
// is the runtime z_token_color() the P37 note called Planned, arriving for the
// one reason that needed it first.
//
// P54 GENERALISED IT AND DELETED THE THREE FUNCTIONS. z_ink_muted /
// z_ink_faint / z_hairline were three hand-written two-branch functions for
// three tokens; there are 29 tokens and two appearances now, so the branch
// lives once, in z_token(), over a table with a high-contrast column. The three
// names had no caller outside their own macros.

// On/off for this process, and whether it is on. Applied from the brokered
// ZELTO_KEY_INCREASE_CONTRAST at startup and on change, exactly like the text
// size; returns true when the value moved, so the caller knows a repaint is
// owed. A surface that called z_text_scaling_disable() still honours this — the
// status bar's height is a contract, its contrast is not.
bool z_contrast_apply(bool increase);
bool z_contrast_increased(void);

// --- Semantic (state) — and the ink that goes ON one ----------------------
//
// P52 MEASURED THESE IN BOTH THEIR ROLES, WHICH IS THE THING P51 DID NOT DO.
// P51's table recorded two semantic failures and described them as wanting "the
// fill redrawn". Measured against what the OS actually DRAWS, that was the wrong
// diagnosis twice over.
//
// FIRST: these tokens are mostly INK, not fills. The charging battery glyph in
// the status bar, the battery percentage on the home widget, a "Requesting..."
// line — all `Foreground(Z_COLOR_SUCCESS, ...)`. And in that role the old green
// was the real failure, one nobody had written down:
//
//     SUCCESS #1e7a4a as ink:  BG 3.94  SURFACE 3.19  SURFACE_2 2.61  SURFACE_3 2.13
//
// Under even WCAG's 3:1 NON-TEXT minimum that is two surfaces failed. The colour
// was chosen to be "deep enough for white text at AA" — it was tuned for the
// role it is rarely used in, at the cost of the role it is usually used in.
//
// SECOND: the pair P51 called a failure (ON_PRIMARY on SUCCESS, 3.71) is not a
// pair this OS draws — ON_PRIMARY is documented for a PRIMARY fill. The pair it
// DOES draw, and which the header itself prescribed, is TEXT_INV on a semantic
// fill, and that was far worse than anything P51 listed:
//
//     TEXT_INV on WARN #e0a53a = 1.99:1     (essentially invisible)
//     TEXT_INV on DANGER #d9524f = 3.63:1
//
// The header was telling callers to make an unreadable combination.
//
// THE FIX IS THE iOS DARK SEMANTIC SET, adopted as one sourced decision rather
// than three hand-tuned nudges. iOS's dark-mode systemGreen/Orange/Red are BRIGHT
// — they are designed to be ink on black — and iOS puts DARK text on them when
// they are fills, which is exactly what the measurement says:
//
//     as ink:            BG    SURFACE  SURFACE_2  SURFACE_3     as fill: dark ink
//     SUCCESS #30d158  10.39     8.42      6.89       5.61                  9.78
//     WARN    #ff9f0a  10.22     8.28      6.78       5.52                  9.62
//     DANGER  #ff453a   6.16     4.99      4.09       3.33                  5.81
//
// Every ink use now clears 4.5 (body text) except DANGER on SURFACE_3, which
// clears 3.0; every fill use clears 4.5 with the ink z_on_fill picks.
//
// WHERE THIS DELIBERATELY DIVERGES FROM iOS: Apple ships WHITE on systemRed, and
// that pairing measures 3.11:1 — Apple's own red badge does not clear AA. Zelto
// picks the ink by measurement, so a DANGER fill gets dark ink and looks slightly
// unlike iOS. That is the rule doing its job, not a mistake.
#define Z_COLOR_SUCCESS    z_token(Z_TOKEN_SUCCESS)  // iOS systemGreen
#define Z_COLOR_WARN       z_token(Z_TOKEN_WARN)     // iOS systemOrange
#define Z_COLOR_DANGER     z_token(Z_TOKEN_DANGER)   // iOS systemRed

// THE INK TO DRAW ON A FILL — never a fixed token. Returns whichever of the OS's
// two inks (TEXT_INV, light / ON_PRIMARY, dark) reaches further from `fill` by
// WCAG contrast. One rule, computed, so retinting a fill moves its ink with it
// and the two cannot drift apart the way TEXT_INV and WARN did.
//
// Use it for any SATURATED fill. The *_DIM panel tints below are dark enough that
// light ink is right on all of them (11:1 and up) and they may keep using
// TEXT_INV directly — but going through this costs nothing and cannot be wrong.
ZColor z_on_fill(ZColor fill);

// Semantic — dim panel fills (a state-tinted surface, not a saturated block).
#define Z_COLOR_SUCCESS_DIM z_token(Z_TOKEN_SUCCESS_DIM)
#define Z_COLOR_WARN_DIM    z_token(Z_TOKEN_WARN_DIM)
#define Z_COLOR_DANGER_DIM  z_token(Z_TOKEN_DANGER_DIM)
#define Z_COLOR_ACCENT_DIM  z_token(Z_TOKEN_ACCENT_DIM)

// Modal backdrop behind an overlay card (chooser / consent / recents).
#define Z_COLOR_SCRIM       z_token(Z_TOKEN_SCRIM)

// --- Materials — the translucent tint of a blurred system surface ---------
// The shade, the dock, the keyboard and a modal sheet are not opaque panels: they
// are MATERIALS. The compositor blurs whatever is behind the rectangle a surface
// declares with z_backdrop (a client cannot see through itself); the surface then
// paints ONE of these tints over that blur. The tint is what makes the blur read
// as a surface rather than as a smudge: it lifts contrast for the content on top
// and sets how much of the scene below survives.
//
// Pick by how much the material must SEPARATE from what is behind it:
//   THIN     the most see-through: a floating bar over its own content (the dock,
//            a heads-up banner) — you should still read the wallpaper through it
//   REGULAR  the default panel: the shade, the keyboard, a sheet
//   THICK    a material that must carry small text and controls with no help from
//            the scene below (a modal, the lock screen's plate)
// Without a compositor that implements the blur these degrade gracefully — the
// tint alone still reads as a translucent panel, only without the defocus.
//   SHEET    a full-screen surface that REPLACES what is under it (the app
//            drawer): heavy, so its own content is what you read. Note what a
//            sheet must be laid over — frost it over the WALLPAPER, not over the
//            screen it covered, or the content underneath reads through the
//            content on top (two sets of app icons at once).
#define Z_COLOR_MATERIAL_THIN    z_token(Z_TOKEN_MATERIAL_THIN)     // ~55%
#define Z_COLOR_MATERIAL_REGULAR z_token(Z_TOKEN_MATERIAL_REGULAR)  // ~72%
#define Z_COLOR_MATERIAL_THICK   z_token(Z_TOKEN_MATERIAL_THICK)    // ~86%
#define Z_COLOR_MATERIAL_SHEET   z_token(Z_TOKEN_MATERIAL_SHEET)    // ~90%

// The hairline that edges a material (a 1px inner border catching the "light" at
// its rim). It is what stops a translucent panel dissolving into a busy backdrop.
#define Z_COLOR_MATERIAL_EDGE    z_token(Z_TOKEN_MATERIAL_EDGE)

// --- Corner radii — one geometry for the whole OS -------------------------
// Every rounded surface picks a step here, so the system reads as one object set.
// The renderer draws these as CONTINUOUS corners (a squircle: |x|^4 + |y|^4 = r^4)
// rather than circular arcs — the curvature ramps in instead of starting abruptly,
// which is why an iOS icon looks "rounder" than a same-radius CSS box. See
// sdk/src/render.c (corner_coverage) and the compositor's matching mask.
// P45 MEASURED THIS LADDER, because P44 flagged it as the same shape of number
// as the type scale and the safe areas and then left it alone on the grounds
// that it "reads as hand-authored rather than transcribed" — an assertion about
// intent, which is exactly the move P43 made about the metrics and got wrong.
//
// THE MEASUREMENT SAYS P44 WAS RIGHT, and here is the test that settles it. If
// these were points spent as pixels (the P43/P44 bug) the raw values would be
// the numbers off a spec sheet — round in POINTS — and dividing by 1.85 would
// recover them. It is the other way round: 10/16/22/32 are round and evenly
// stepped (+6, +6, +10) in SCREEN UNITS, and in points they are 5.4, 8.6, 11.9,
// 17.3 — round nowhere. A transcribed table looks the opposite. So this ladder
// was authored directly in screen units and is NOT the transcription bug.
// Z_RADIUS_ICON being a FRACTION is the corroboration: whoever wrote that line
// was thinking about resolution independence on the one radius that needed it.
//
// BUT HAND-AUTHORED IS NOT THE SAME AS RIGHT, and the ladder's real fault is
// not its scale — it is that it is UNDER-RESOLVED. The anchor for judging it is
// the one radius in the OS that is calibrated rather than chosen: an app icon,
// whose corner is 0.2237 x 104 = 23.3 units on the home grid. That anchor is
// worth more than any spec sheet here because an icon sits in the SAME FRAME as
// the widgets and cards being judged, so the comparison is side-by-side on one
// screen rather than against a remembered number. Measured against it:
//
//     CHIP  10 = 0.43 x the icon corner   (a key: about right)
//     CARD  16 = 0.69 x                   (a list row: slightly tight, ~15%)
//     PANEL 22 = 0.95 x                   ← INVERTED, see below
//     SHEET 32 = 1.38 x                   (a pulled sheet: the open question)
//
// PANEL WAS THE ONE THAT MATTERED, and the evidence needs no external number: a
// home-screen widget's corner was 22 units while the app icons sitting directly
// beside it on the same screen have corners of 23.3. The big soft card was
// fractionally SQUARER than the small tiles next to it. Every phone this idiom
// comes from makes the widget visibly rounder than the icon.
//
// The fix is not to retune PANEL, because PANEL was doing FOUR jobs whose
// correct radii differ by more than 2x: a home widget, the Control Center
// slider slab, a "Done" button (which at this size is really a capsule), and
// the consent ALERT. One token cannot be right for all four, and moving it
// would have fixed the widget by breaking the alert. So the widget — the case
// with the in-frame proof — gets its own token, and PANEL keeps the value that
// suits the alert it is now mostly used by.
//
// STILL OPEN, DELIBERATELY NOT CHANGED: SHEET (32) on a full-width pulled
// surface, and CARD (16) at ~15% under. Neither has an in-frame anchor the way
// the widget did, and both would have been changed on a half-remembered spec
// value — which is the failure mode this comment exists to stop repeating.
// P52 REVISITED THE TWO STILL-OPEN STEPS AND CHANGED ONE OF THEM.
//
// CHIP / CARD / PANEL / WIDGET DO NOT MOVE, and that is a decision rather than an
// omission. The dossier's iOS readings for them (an inset-list card at ~10pt = 19
// units, an alert at ~14pt = 26) are marked [unpub] — Apple publishes no control
// corner radii, so those are community measurements, and both deltas are under 4
// units. P45 declined exactly these two on exactly these grounds; moving CARD now
// would additionally move the sheet below (which is derived from it), so one
// eyeballed nudge would quietly become two.
//
// SHEET DID MOVE, and not because a spec sheet said so — because it was the ONLY
// number the concentric rule left free. iOS 26 made concentric corners
// first-class (ConcentricRectangle / .containerConcentric) with one rule: a
// nested radius is the container's LESS the inset between them, so the gap
// between the two curves stays constant all the way round. The share sheet is the
// place this OS actually nests — a sheet holding rows — and P52's spacing scale
// put the sheet's content margin on Apple's 16pt (29 units) like every other
// container in the OS. With the inset at 29 and the rows at CARD, the sheet's
// corner is DETERMINED: 16 + 29 = 45. It is not a chosen value.
//
// The old 32 also left the ladder non-monotonic: the largest surface in the OS
// had a SMALLER corner than a home widget (40). A ladder whose biggest step is
// not its roundest is not a ladder.
#define Z_RADIUS_CHIP     10.0f   // a small control: a QS chip, a key, a badge
#define Z_RADIUS_CARD     16.0f   // a card, a list row, a notification
#define Z_RADIUS_PANEL    22.0f   // an alert / a small raised panel
#define Z_RADIUS_WIDGET   40.0f   // a home-screen widget card — MUST read rounder
                                  // than the app icons beside it (see above)
#define Z_RADIUS_SHEET    45.0f   // a big pulled surface: the shade, a modal sheet.
                                  // = Z_RADIUS_CARD + a 16pt content margin, by
                                  // the concentric rule; asserted, not chosen.

// THE CONCENTRIC RULE, as an expression. A rounded thing drawn INSIDE another
// rounded thing takes the container's radius less the inset between them — never
// a second constant that happens to look right, which is how the share sheet's
// corner drifted 4 units out of true for three phases before P48 caught it.
//
// This is the same standing rule as "a reserve that names its parts must be an
// expression over them", applied to curvature. Use it for the small cases too:
// the 1-unit fill inside a material's edge hairline is a nesting, and writing it
// `Z_RADIUS_NESTED(Z_RADIUS_WIDGET, 1.0f)` says so where `- 1.0f` only said that
// someone subtracted one.
//
// Clamped at zero: a deep inset inside a slightly-rounded container would
// otherwise go negative and the renderer would read that as a very large radius.
#define Z_RADIUS_NESTED(outer, inset) \
    (((outer) - (inset)) > 0.0f ? ((outer) - (inset)) : 0.0f)

// THE CORNER IS A SQUIRCLE AT n = 4, AND STAYS THERE — a written decision, since
// iOS's continuous corner is a hand-tuned bezier nearer n ~ 5 and "match iOS"
// would mean chasing a curve Apple ships no closed form for. Two things make the
// exponent expensive to change and cheap to leave: it is implemented TWICE (the
// SDK's corner_coverage in sdk/src/render.c and the compositor's CORNER_N in
// compositor/src/backdrop.c) and the two MUST agree, or a material's blurred
// backdrop is masked to a different curve than the surface painted over it; and
// the visible gap between n=4 and n=5 is far smaller than the gap between a
// circular arc and either of them, which is the jump Zelto already made.
// An INTEGER, so it can carry a _Static_assert: a float comparison is not an
// integer constant expression and -Wpedantic -Werror rejects it.
#define Z_CORNER_N 4
#define Z_RADIUS_ICON     0.2237f // APP ICONS ONLY: a FRACTION of the icon's width
                                  // (Apple's icon grid: the corner is proportional
                                  // to the tile, so an icon keeps its shape at any
                                  // size — never a fixed px radius).

// --- Elevation — soft drop-shadow depth levels ---------------------------
// A raised surface (a card, the shade panel, an overlay modal, a lifted ghost)
// casts a soft shadow so it reads as floating above what it sits on. The value
// is the shadow's blur radius in logical px; the renderer derives the spread,
// downward offset and peak opacity from it (light from directly above). Pair a
// level with Shadow() — the higher the level, the further off the surface it
// floats. SHADOW is the ink (near-black; alpha is scaled per pixel by the
// distance falloff, so the token's own alpha is the peak under the surface).
//
// WHEN TO CAST ONE AT ALL (P52). iOS builds depth from TONE and HAIRLINES, not
// from drop shadows: a grouped list's card is separated from the page by being
// #1c1c1e on #000000, and it casts nothing. Real shadows are reserved for
// surfaces that are genuinely FLOATING — a sheet, an alert, a menu, a popover, a
// thing over the wallpaper. So the test before writing Shadow() is not "is this
// raised in the visual hierarchy" but "is there something UNDER it that it is
// floating over":
//
//   YES, keep the shadow — a modal sheet or alert (over an app), the keyboard's
//     accent popup and preview callout, the volume HUD, the switcher's cards, an
//     app icon and the dock plate (over the wallpaper), a notification card
//     (over the wallpaper, an app, or a blurred panel — never over a sibling
//     surface), a knob riding a track.
//   NO, use the tone ladder — a card sitting ON a surface. It is already a step
//     up the ramp from what it sits on, and that step IS the separation.
//
// The audit that produced this rule found exactly ONE site on the wrong side of
// it, and the anchor was in the repo rather than in a spec: SETTINGS' OWN
// GROUPED CARDS CAST NO SHADOW — the only Shadow() in that file is the switch
// knob — so the most-used list in the OS already did it this way, and
// app_chrome.h's z_card was the outlier. That is worth more than a style guide,
// because the two idioms were visibly disagreeing inside one product.
//
// No lint enforces this: "floating over something" versus "sitting on a surface"
// is semantic, not syntactic, and a rule a grep cannot check belongs where it is
// read rather than in a test that would only approximate it.
#define Z_ELEV_1   6.0f    // subtle: a knob, a key cap, an app icon
#define Z_ELEV_2   14.0f   // a home widget, the dock plate — over the wallpaper
#define Z_ELEV_3   28.0f   // overlays, modals, the lifted ghost
#define Z_COLOR_SHADOW      z_token(Z_TOKEN_SHADOW)

// --- Press feedback — the touch-down highlight veil ----------------------
// A tappable control (Button, an OnTap tile, a nav mark) paints a soft veil over
// itself while pressed, so touch-down gets an immediate visual response and
// release fades it out. The toolkit scales the peak alpha by the live press
// spring (0 released -> 1 held). See P31 (Feedback.md).
//
// TWO VEILS, AND THE CHOICE IS COMPUTED FROM THE SURFACE — never from the
// appearance, and never named at the call site. Use z_press_veil().
//
// The old comment here read "a light overlay reads as a highlight on both the
// dark surfaces and the azure PRIMARY fill". Two things wrong with that
// sentence: PRIMARY has not been azure for several phases, and the claim was
// false for it either way. white@0x3d over PRIMARY #f2f2f7 measures 1.026:1, so
// every filled Button in the DARK palette has had press feedback that is painted
// and cannot be seen since P31 — see the measured note over z_press_veil in
// sdk/src/theme.c. A light appearance made the same defect unmissable on the
// page itself (1.000:1), which is how it was finally found.
#define Z_COLOR_PRESS      z_token(Z_TOKEN_PRESS)      // over a dark surface
#define Z_COLOR_PRESS_INV  z_token(Z_TOKEN_PRESS_INV)  // over a light one

// The veil to paint over `under`. Same shape and same argument as z_on_fill: a
// rule computed from what is actually there cannot drift from it.
ZColor z_press_veil(ZColor under);

// --- Back-compat aliases (older token names) -----------------------------
#define Z_COLOR_BACKGROUND Z_COLOR_BG

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_GFX_H
