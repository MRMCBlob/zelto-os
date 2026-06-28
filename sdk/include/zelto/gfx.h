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

// --- Design tokens (a small slice of the design language) ----------------
// Resolved to concrete colours here; a themed z_token_color() is Planned.
#define Z_COLOR_BACKGROUND z_rgba(0x0a, 0x84, 0x8c, 0xff)  // Zelto teal
#define Z_COLOR_SURFACE    z_rgba(0xff, 0xff, 0xff, 0xff)
#define Z_COLOR_SURFACE_2  z_rgba(0xe9, 0xee, 0xf0, 0xff)
#define Z_COLOR_PRIMARY    z_rgba(0xfa, 0x66, 0x26, 0xff)  // Zelto amber
#define Z_COLOR_ACCENT     z_rgba(0x2e, 0x9b, 0xff, 0xff)
#define Z_COLOR_TEXT       z_rgba(0x10, 0x20, 0x24, 0xff)
#define Z_COLOR_TEXT_INV   z_rgba(0xff, 0xff, 0xff, 0xff)

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_GFX_H
