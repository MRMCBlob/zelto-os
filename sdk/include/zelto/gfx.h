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

// --- Design tokens — the Zelto design system -----------------------------
// One deep slate base with a single azure accent and three semantic colours.
// This header is the single source of truth: every system surface and app
// draws from these tokens (no ad-hoc hexes), so retinting the OS is a matter
// of editing the values below. A themed, runtime z_token_color() is Planned.
//
// SURFACES climb from BG (the deepest, root) up through SURFACE_3 (the
// highest, e.g. an input or a key); BORDER is the hairline between them.
// TEXT/TEXT_MUTED/TEXT_INV are tuned to hit WCAG AA on those tones. Interactive
// elements share PRIMARY (a white-legible azure) with ACCENT as its brighter
// highlight; SUCCESS/WARN/DANGER carry state, each with a *_DIM panel fill for
// a state-tinted surface (a status card) rather than a saturated block.

// Base surfaces (deepest -> highest elevation).
#define Z_COLOR_BG         z_rgba(0x0b, 0x0f, 0x14, 0xff)  // root background
#define Z_COLOR_SURFACE    z_rgba(0x14, 0x1a, 0x21, 0xff)  // raised panel
#define Z_COLOR_SURFACE_2  z_rgba(0x1e, 0x26, 0x30, 0xff)  // card / row
#define Z_COLOR_SURFACE_3  z_rgba(0x2a, 0x33, 0x40, 0xff)  // input / key / chip
#define Z_COLOR_BORDER     z_rgba(0x3a, 0x45, 0x52, 0xff)  // hairline / divider

// Accent (a single azure hue; PRIMARY is deep enough for white text at AA).
#define Z_COLOR_PRIMARY    z_rgba(0x16, 0x68, 0xc9, 0xff)  // filled action / active
#define Z_COLOR_ACCENT     z_rgba(0x4a, 0xa3, 0xff, 0xff)  // highlight / link / glyph

// Text (on the surface tones — all AA).
#define Z_COLOR_TEXT       z_rgba(0xe6, 0xec, 0xf2, 0xff)  // primary
#define Z_COLOR_TEXT_MUTED z_rgba(0x93, 0xa1, 0xb0, 0xff)  // secondary / caption
#define Z_COLOR_TEXT_FAINT z_rgba(0x6b, 0x74, 0x7d, 0xff)  // de-emphasised / disabled (AA-large)
#define Z_COLOR_TEXT_INV   z_rgba(0xf4, 0xf7, 0xfb, 0xff)  // on PRIMARY / semantic fills

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
