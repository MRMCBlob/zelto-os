// C API: Graphics (<zelto/gfx.h>)
//
// Low-level types shared by the UI toolkit and (eventually) custom-draw views.
// The MVP exposes colours and rectangles; the canvas/path/GPU-surface API
// documented in docs/api-reference/c/gfx.md is Planned and lands in a later phase.
// Conventions: docs/api-reference/conventions.md.
#ifndef ZELTO_GFX_H
#define ZELTO_GFX_H

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
// This header is the single source of truth: every system surface and app draws
// from these tokens (no ad-hoc hexes), so retinting the OS is a matter of editing
// the values below. A themed, runtime z_token_color() (and a light palette) is
// Planned — see the P37 notes.
//
// SURFACES climb from BG (the deepest, root — true black, which on an OLED phone
// is the panel switched off) up through SURFACE_3 (the highest: an input, a key);
// BORDER is the hairline between them. Ink is DELIBERATELY not pure white: #f2f2f7
// on #000 is still far past AA and is markedly easier to sit in front of for an
// hour than 21:1 white-on-black.
//
// The interactive fill (PRIMARY) is near-white with dark ink on it (ON_PRIMARY) —
// an active toggle reads as "lit", the way a Control Center chip does, without
// introducing a hue. SUCCESS/WARN/DANGER stay coloured because their whole job is
// to be exceptional, each with a *_DIM panel fill for a state-tinted surface.

// Base surfaces (deepest -> highest elevation).
#define Z_COLOR_BG         z_rgba(0x00, 0x00, 0x00, 0xff)  // root background (true black)
#define Z_COLOR_SURFACE    z_rgba(0x1c, 0x1c, 0x1e, 0xff)  // raised panel
#define Z_COLOR_SURFACE_2  z_rgba(0x2c, 0x2c, 0x2e, 0xff)  // card / row
#define Z_COLOR_SURFACE_3  z_rgba(0x3a, 0x3a, 0x3c, 0xff)  // input / key / chip
// One step lighter again, for a control that must stand OFF a surface which is
// itself already raised — the character caps on the keyboard's dark material,
// where SURFACE_3 sits too close to the field of keys to read as a key.
#define Z_COLOR_SURFACE_4  z_rgba(0x55, 0x55, 0x59, 0xff)  // key cap on a panel
#define Z_COLOR_BORDER     z_rgba(0x38, 0x38, 0x3a, 0xff)  // hairline / divider

// The interactive fill. Not a hue — a light, "lit" surface. Text and glyphs on it
// use ON_PRIMARY (dark), NOT TEXT_INV.
#define Z_COLOR_PRIMARY    z_rgba(0xf2, 0xf2, 0xf7, 0xff)  // filled action / active
#define Z_COLOR_ON_PRIMARY z_rgba(0x0a, 0x0a, 0x0c, 0xff)  // ink ON a PRIMARY fill
#define Z_COLOR_ACCENT     z_rgba(0xff, 0xff, 0xff, 0xff)  // highlight / active glyph

// Text (on the surface tones — all AA).
#define Z_COLOR_TEXT       z_rgba(0xf2, 0xf2, 0xf7, 0xff)  // primary (soft white)
#define Z_COLOR_TEXT_MUTED z_rgba(0xa1, 0xa1, 0xa8, 0xff)  // secondary / caption
#define Z_COLOR_TEXT_FAINT z_rgba(0x6c, 0x6c, 0x70, 0xff)  // de-emphasised / disabled (AA-large)
#define Z_COLOR_TEXT_INV   z_rgba(0xf4, 0xf4, 0xf8, 0xff)  // on a SEMANTIC (coloured) fill

// Semantic (state).
#define Z_COLOR_SUCCESS    z_rgba(0x1e, 0x7a, 0x4a, 0xff)  // deep enough for white text at AA
#define Z_COLOR_WARN       z_rgba(0xe0, 0xa5, 0x3a, 0xff)
#define Z_COLOR_DANGER     z_rgba(0xd9, 0x52, 0x4f, 0xff)

// Semantic — dim panel fills (a state-tinted surface, not a saturated block).
#define Z_COLOR_SUCCESS_DIM z_rgba(0x16, 0x3d, 0x2a, 0xff)
#define Z_COLOR_WARN_DIM    z_rgba(0x3d, 0x30, 0x16, 0xff)
#define Z_COLOR_DANGER_DIM  z_rgba(0x3d, 0x1c, 0x1e, 0xff)
#define Z_COLOR_ACCENT_DIM  z_rgba(0x16, 0x32, 0x4a, 0xff)

// Modal backdrop behind an overlay card (chooser / consent / recents).
#define Z_COLOR_SCRIM       z_scrim(0xb0)

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
#define Z_COLOR_MATERIAL_THIN    z_rgba(0x1c, 0x1c, 0x1e, 0x8c)   // ~55%
#define Z_COLOR_MATERIAL_REGULAR z_rgba(0x14, 0x14, 0x16, 0xb8)   // ~72%
#define Z_COLOR_MATERIAL_THICK   z_rgba(0x0e, 0x0e, 0x10, 0xdb)   // ~86%
#define Z_COLOR_MATERIAL_SHEET   z_rgba(0x08, 0x08, 0x0a, 0xe6)   // ~90%

// The hairline that edges a material (a 1px inner border catching the "light" at
// its rim). It is what stops a translucent panel dissolving into a busy backdrop.
#define Z_COLOR_MATERIAL_EDGE    z_rgba(0xff, 0xff, 0xff, 0x1f)

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
#define Z_RADIUS_CHIP     10.0f   // a small control: a QS chip, a key, a badge
#define Z_RADIUS_CARD     16.0f   // a card, a list row, a notification
#define Z_RADIUS_PANEL    22.0f   // an alert / a small raised panel
#define Z_RADIUS_WIDGET   40.0f   // a home-screen widget card — MUST read rounder
                                  // than the app icons beside it (see above)
#define Z_RADIUS_SHEET    32.0f   // a big pulled surface: the shade, a modal sheet
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
#define Z_ELEV_1   6.0f    // subtle: chips, small raised rows
#define Z_ELEV_2   14.0f   // cards, home widgets
#define Z_ELEV_3   28.0f   // overlays, modals, the lifted ghost
#define Z_COLOR_SHADOW      z_scrim(0x80)

// --- Press feedback — the touch-down highlight veil ----------------------
// A tappable control (Button, an OnTap tile, a nav mark) paints this soft light
// veil over itself while pressed, so touch-down gets an immediate visual response
// and release fades it out. One token, whose peak alpha the toolkit scales by the
// live press spring (0 released -> 1 held); a light overlay reads as a highlight
// on both the dark surfaces and the azure PRIMARY fill. See P31 (Feedback.md).
#define Z_COLOR_PRESS      z_rgba(0xff, 0xff, 0xff, 0x3d)  // peak ~24% white veil

// --- Back-compat aliases (older token names) -----------------------------
#define Z_COLOR_BACKGROUND Z_COLOR_BG

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_GFX_H
