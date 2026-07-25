// The palette at runtime: the VALUES behind the token names in <zelto/gfx.h>.
//
// WHAT THIS FILE IS. Every Z_COLOR_* macro expands to z_token(Z_TOKEN_*), so the
// whole OS reads its colours through the one function at the bottom of this
// file, and every colour in the system is a runtime lookup rather than a
// compile-time constant. See the long note at the top of gfx.h for why (short
// version: a phone that cannot be light is not a themed phone, it is a dark
// phone) and for what a token MEANS — this file only says what it IS.
//
// THE TABLE IS THE PALETTE. One row per token, and the columns are the
// appearances. That is the form a palette is reviewed in: reading down a column
// tells you whether an appearance has a hierarchy, and reading across a row
// tells you whether a token means the same thing in both. Two appearances
// scattered through a header as alternating comments is how one of them quietly
// goes wrong, because the eye reads the column it came for.
//
// INCREASE CONTRAST IS AN OVERRIDE LIST, NOT A SECOND TABLE. It moves exactly
// three tokens (see the measured note in gfx.h), and a duplicate palette would
// be 29 rows of which 26 must never differ — which is 26 chances to make them
// differ.
//
// P51's z_ink_muted / z_ink_faint / z_hairline were three hand-written
// two-branch functions and they are gone: the branch is in z_token now, and it
// is the same branch for all of them. This file is deliberately the same shape
// as sdk/src/type.c — one process-global, set from the brokered setting at
// startup and on change, read through a function that the call sites already go
// through without knowing it.
//
// NOT GATED ON z_text_scaling_disable(). The status bar, the keyboard and the
// home indicator opt out of Dynamic Type because their HEIGHTS are contracts
// other processes offset by — nothing about a contract says the clock has to be
// hard to read. A colour swap moves no geometry at all, which is exactly why
// this one is safe everywhere the other is not.

#include <math.h>
#include <stdbool.h>

#include <zelto/gfx.h>

// --- state ----------------------------------------------------------------
// PROCESS-GLOBAL, for type.c's reason: a process draws exactly one surface in
// this OS, so one global is one surface's appearance — the granularity the
// setting actually has. Threading a palette through 330 call sites would be the
// mistake the seam exists to avoid.

static bool g_increase;

bool z_contrast_increased(void) { return g_increase; }

bool z_contrast_apply(bool increase) {
    bool changed = increase != g_increase;
    g_increase = increase;
    return changed;
}

// --- the dark palette ------------------------------------------------------
// The shipped appearance, and P54 stage 0 moved it by not one byte — that stage
// was a pure refactor, and the claim was checked rather than asserted: sixteen
// frames of the catalogue re-shot before and after and diffed with
// meta/pngdiff.py (meta/shots-theme-subset.sh), with the status-bar clock the
// only delta.
//
// Where these came from is documented beside each token's NAME in gfx.h; the
// short version is that the surface ladder matches iOS's dark tokens exactly
// (P52 measured it), the semantic trio is iOS's dark systemGreen/Orange/Red, and
// the inks are opaque greys rather than iOS's alpha-carried near-white because
// opaque is what a contrast table can be computed over.
#define RGBA(r, g, b, a) {(r), (g), (b), (a)}

static const ZColor g_dark[Z_TOKEN_COUNT] = {
    [Z_TOKEN_BG]               = RGBA(0x00, 0x00, 0x00, 0xff),
    [Z_TOKEN_SURFACE]          = RGBA(0x1c, 0x1c, 0x1e, 0xff),
    [Z_TOKEN_SURFACE_2]        = RGBA(0x2c, 0x2c, 0x2e, 0xff),
    [Z_TOKEN_SURFACE_3]        = RGBA(0x3a, 0x3a, 0x3c, 0xff),
    [Z_TOKEN_SURFACE_4]        = RGBA(0x55, 0x55, 0x59, 0xff),
    [Z_TOKEN_BORDER]           = RGBA(0x38, 0x38, 0x3a, 0xff),

    [Z_TOKEN_PRIMARY]          = RGBA(0xf2, 0xf2, 0xf7, 0xff),
    [Z_TOKEN_ON_PRIMARY]       = RGBA(0x0a, 0x0a, 0x0c, 0xff),
    [Z_TOKEN_ACCENT]           = RGBA(0xff, 0xff, 0xff, 0xff),

    [Z_TOKEN_TEXT]             = RGBA(0xf2, 0xf2, 0xf7, 0xff),
    [Z_TOKEN_TEXT_MUTED]       = RGBA(0xa1, 0xa1, 0xa8, 0xff),
    [Z_TOKEN_TEXT_FAINT]       = RGBA(0x6c, 0x6c, 0x70, 0xff),
    [Z_TOKEN_TEXT_INV]         = RGBA(0xf4, 0xf4, 0xf8, 0xff),

    [Z_TOKEN_SUCCESS]          = RGBA(0x30, 0xd1, 0x58, 0xff),
    [Z_TOKEN_WARN]             = RGBA(0xff, 0x9f, 0x0a, 0xff),
    [Z_TOKEN_DANGER]           = RGBA(0xff, 0x45, 0x3a, 0xff),
    [Z_TOKEN_SUCCESS_DIM]      = RGBA(0x16, 0x3d, 0x2a, 0xff),
    [Z_TOKEN_WARN_DIM]         = RGBA(0x3d, 0x30, 0x16, 0xff),
    [Z_TOKEN_DANGER_DIM]       = RGBA(0x3d, 0x1c, 0x1e, 0xff),
    [Z_TOKEN_ACCENT_DIM]       = RGBA(0x16, 0x32, 0x4a, 0xff),

    [Z_TOKEN_SCRIM]            = RGBA(0x00, 0x00, 0x00, 0xb0),
    [Z_TOKEN_MATERIAL_THIN]    = RGBA(0x1c, 0x1c, 0x1e, 0x8c),
    [Z_TOKEN_MATERIAL_REGULAR] = RGBA(0x14, 0x14, 0x16, 0xb8),
    [Z_TOKEN_MATERIAL_THICK]   = RGBA(0x0e, 0x0e, 0x10, 0xdb),
    [Z_TOKEN_MATERIAL_SHEET]   = RGBA(0x08, 0x08, 0x0a, 0xe6),
    [Z_TOKEN_MATERIAL_EDGE]    = RGBA(0xff, 0xff, 0xff, 0x1f),
    [Z_TOKEN_SHADOW]           = RGBA(0x00, 0x00, 0x00, 0x80),
    [Z_TOKEN_PRESS]            = RGBA(0xff, 0xff, 0xff, 0x3d),

    // Was a raw #4aa3ff in system/launcher/main.c at two alphas — the only hue
    // in the shipped OS that lived in no table. Kept and named rather than
    // deleted; the argument is beside Z_COLOR_DROP_TARGET in gfx.h. The second
    // alpha (the placeholder under the icon in hand, 0x45) is z_fade() of this.
    [Z_TOKEN_DROP_TARGET]      = RGBA(0x4a, 0xa3, 0xff, 0x30),
};

// --- Increase Contrast: the three tokens with a second value ---------------
// Chosen as the LEAST brightening that clears AA on the darkest surface each is
// drawn on — not "as light as possible", because the job of a muted ink is to be
// a step below the primary one, and a preference that flattens the hierarchy has
// substituted one unreadable screen for another. The numbers are in gfx.h.
//
// A LIST, not a table: three overrides against 29 tokens. Linear-scanned, which
// at three entries is cheaper than any structure that would avoid the scan.
static const struct {
    ZToken token;
    ZColor dark;
} g_contrast[] = {
    {Z_TOKEN_TEXT_MUTED, RGBA(0xc7, 0xc7, 0xcc, 0xff)},  // 4.42 -> 6.74 on SURFACE_3
    {Z_TOKEN_TEXT_FAINT, RGBA(0xa8, 0xa8, 0xb0, 0xff)},  // 2.17 -> 4.81 on SURFACE_3
    {Z_TOKEN_BORDER,     RGBA(0x6c, 0x6c, 0x70, 0xff)},  // 1.40 -> 3.25 on SURFACE
};

#define Z_CONTRAST_OVERRIDES ((int)(sizeof(g_contrast) / sizeof(g_contrast[0])))

ZColor z_token(ZToken token) {
    // MAGENTA, not black and not a clamp. A token index that does not exist is a
    // bug in the caller, and the two quiet answers are both worse: the table's
    // first entry paints a plausible screen, and reading past the table paints
    // whatever follows it in .rodata. This is the only colour in the OS that is
    // not in the palette, which is the point of it.
    if (token < 0 || token >= Z_TOKEN_COUNT) {
        return z_rgba(0xff, 0x00, 0xff, 0xff);
    }
    if (g_increase) {
        for (int i = 0; i < Z_CONTRAST_OVERRIDES; i++) {
            if (g_contrast[i].token == token) {
                return g_contrast[i].dark;
            }
        }
    }
    return g_dark[token];
}

// --- The ink that goes on a fill -------------------------------------------
// WCAG 2.1 relative luminance, then pick whichever of the two inks reaches
// further. See the measured table over Z_COLOR_SUCCESS in <zelto/gfx.h> for why
// this is a computation rather than a token: the header used to prescribe
// TEXT_INV on any semantic fill, and TEXT_INV on the old WARN was 1.99:1.
//
// The sRGB transfer, not a gamma of 2.2 — the same formula test_contrast_tokens
// checks with, so the assertion and the implementation cannot disagree about
// what "contrast" means. (The names are prefixed because that test compiles
// this file into itself and carries its own copy of the formula — two
// independent implementations that must agree.)
static double fill_chan(unsigned v) {
    double c = (double)v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double fill_lum(ZColor c) {
    return 0.2126 * fill_chan(c.r) + 0.7152 * fill_chan(c.g) +
           0.0722 * fill_chan(c.b);
}

static double fill_contrast(ZColor a, ZColor b) {
    double la = fill_lum(a), lb = fill_lum(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

ZColor z_on_fill(ZColor fill) {
    ZColor light = Z_COLOR_TEXT_INV, dark = Z_COLOR_ON_PRIMARY;
    return fill_contrast(dark, fill) >= fill_contrast(light, fill) ? dark
                                                                  : light;
}
