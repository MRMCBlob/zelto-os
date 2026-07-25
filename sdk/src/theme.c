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
static ZTheme g_theme = Z_THEME_DARK;

bool z_contrast_increased(void) { return g_increase; }

bool z_contrast_apply(bool increase) {
    bool changed = increase != g_increase;
    g_increase = increase;
    return changed;
}

ZTheme z_theme(void) { return g_theme; }

// Clamp rather than reject, for clamp_step's reason in type.c: the value arrives
// as a brokered STRING that any process can write, and a screen drawn at
// appearance 7 is not a smaller failure than a screen drawn at appearance 1.
bool z_theme_apply(ZTheme theme) {
    if (theme != Z_THEME_LIGHT) {
        theme = Z_THEME_DARK;
    }
    bool changed = theme != g_theme;
    g_theme = theme;
    return changed;
}

// --- THE PALETTE -----------------------------------------------------------
// One row per token, two columns. Read DOWN a column to judge whether an
// appearance has a hierarchy; read ACROSS a row to check that a token still
// means the same thing in the other one.
//
// THE DARK COLUMN IS THE SHIPPED APPEARANCE and P54 stage 0 moved it by not one
// byte — that stage was a pure refactor and the claim was checked rather than
// asserted (sixteen catalogue frames re-shot before and after, diffed against a
// third run on the same binary; every moving pixel was a clock).
//
// WHERE THE DARK VALUES CAME FROM is documented beside each token's NAME in
// gfx.h: the surface ladder matches iOS's dark tokens exactly (P52 measured it),
// the semantic trio is iOS's dark systemGreen/Orange/Red, and the inks are
// opaque greys rather than iOS's alpha-carried near-white because opaque is what
// a contrast table can be computed over.
//
// THE LIGHT COLUMN, AND THE ONE DECISION IN IT. iOS light has TWO ladders —
// systemBackground #fff whose fills go DOWN, and systemGroupedBackground
// #f2f2f7 whose cards go UP to #fff — and Zelto has one set of tokens serving
// both roles. Measured against what this OS actually draws:
//
//   - Nearly every app is a GROUPED page: `Background(Z_COLOR_BG, ...)` with
//     SURFACE / SURFACE_2 cards on it. So the iOS-faithful assignment is
//     BG #f2f2f7 with SURFACE #fff — after which SURFACE_2 and SURFACE_3 have
//     nowhere brighter to go and the ladder has to turn around.
//   - THE LADDER'S DIRECTION IS LOAD-BEARING. sdk/src/view.c reads it as one
//     (`active ? SURFACE_3 : SURFACE_2`), and so does every
//     `z_color_lerp(SURFACE_3, PRIMARY, v)` track. A ladder that reverses in the
//     middle makes "one step more prominent" mean two different things.
//   - So the two candidates were MEASURED on the pairs the OS draws rather than
//     argued. Grouped-faithful puts Notepad's editor (SURFACE_2) on its page at
//     1.05:1 — a field that does not read as a field — and does the same to
//     Fetch's and Store's panels. Monotone-from-white separates every pair.
//
// TAKEN: monotone from white, down Apple's own systemGray ramp. THE COST, stated
// rather than discovered: Settings' grouped list is grey cards on a white page,
// which is the inverse of iOS's grouped convention. What is lost is a
// convention; what would have been lost the other way is legibility, and only
// one of those two is measurable.
//
// SURFACE_4 IS THE DECLARED EXCEPTION and it is not step 5 of anything. It is
// never drawn on BG — its only caller is the keyboard, on MATERIAL_REGULAR. In
// light that makes it a WHITE letter cap on a grey frosted plane with SURFACE_3
// modifier caps beside it, which is exactly iOS's light keyboard and is the
// opposite direction from the ramp above. Its bar is against the material it
// sits on, not against SURFACE_3.
#define RGBA(r, g, b, a) {(r), (g), (b), (a)}

typedef struct {
    ZColor dark, light;
} ZTokenRow;

static const ZTokenRow g_palette[Z_TOKEN_COUNT] = {
    //                             dark                            light
    [Z_TOKEN_BG]               = {RGBA(0x00, 0x00, 0x00, 0xff), RGBA(0xff, 0xff, 0xff, 0xff)},
    [Z_TOKEN_SURFACE]          = {RGBA(0x1c, 0x1c, 0x1e, 0xff), RGBA(0xf2, 0xf2, 0xf7, 0xff)},
    [Z_TOKEN_SURFACE_2]        = {RGBA(0x2c, 0x2c, 0x2e, 0xff), RGBA(0xe5, 0xe5, 0xea, 0xff)},
    [Z_TOKEN_SURFACE_3]        = {RGBA(0x3a, 0x3a, 0x3c, 0xff), RGBA(0xd1, 0xd1, 0xd6, 0xff)},
    // Not step 5 — the letter cap, on a material. See the note above.
    [Z_TOKEN_SURFACE_4]        = {RGBA(0x55, 0x55, 0x59, 0xff), RGBA(0xff, 0xff, 0xff, 0xff)},
    // iOS opaqueSeparator. 1.53:1 on SURFACE against dark's 1.45 — the same band
    // the P52 dossier measured iOS's own translucent separator into.
    [Z_TOKEN_BORDER]           = {RGBA(0x38, 0x38, 0x3a, 0xff), RGBA(0xc6, 0xc6, 0xc8, 0xff)},

    // The "lit" fill inverts with the appearance, because "lit" means further
    // from the page in the direction the page is not: near-white on black,
    // near-black on white. ON_PRIMARY follows it, which is what makes the two of
    // them the palette's two POLARITIES in either appearance — and that is what
    // z_on_fill picks between now that TEXT_INV is gone.
    [Z_TOKEN_PRIMARY]          = {RGBA(0xf2, 0xf2, 0xf7, 0xff), RGBA(0x1c, 0x1c, 0x1e, 0xff)},
    [Z_TOKEN_ON_PRIMARY]       = {RGBA(0x0a, 0x0a, 0x0c, 0xff), RGBA(0xf2, 0xf2, 0xf7, 0xff)},
    [Z_TOKEN_ACCENT]           = {RGBA(0xff, 0xff, 0xff, 0xff), RGBA(0x00, 0x00, 0x00, 0xff)},

    // 17.01 : 5.23 : 3.26 on BG, against dark's 18.82 : 8.18 : 4.02. The same
    // hierarchy and the same SHAPE of failure — muted and faint go under AA on
    // the lower surfaces in both appearances, which is what Increase Contrast is
    // for and what the positive control in test_contrast_tokens asserts.
    [Z_TOKEN_TEXT]             = {RGBA(0xf2, 0xf2, 0xf7, 0xff), RGBA(0x1c, 0x1c, 0x1e, 0xff)},
    [Z_TOKEN_TEXT_MUTED]       = {RGBA(0xa1, 0xa1, 0xa8, 0xff), RGBA(0x6c, 0x6c, 0x70, 0xff)},
    [Z_TOKEN_TEXT_FAINT]       = {RGBA(0x6c, 0x6c, 0x70, 0xff), RGBA(0x8e, 0x8e, 0x93, 0xff)},

    // APPLE'S ACCESSIBLE LIGHT VARIANTS, NOT ITS STANDARD ONES, and that is a
    // measurement rather than a preference: standard light systemGreen and
    // systemOrange are 2.22 and 2.20 as ink on white — under even WCAG's 3:1
    // NON-TEXT minimum. They are tuned to be FILLS. Zelto uses these tokens
    // mostly as INK (the charging glyph, a status line — P52 established this),
    // so it takes the variants Apple publishes for exactly that. All three clear
    // 3.0 on all four surfaces, which dark does not quite manage (DANGER on
    // SURFACE_3 is 3.33 there).
    //
    // SUCCESS IS ONE STEP DARKER THAN APPLE'S, and the test found it rather than
    // a reviewer: Apple's accessible green #1c8139 measures 4.43:1 under the ink
    // z_on_fill picks for it, which is 0.07 short of AA in the FILL role. It is
    // the only one of the three that misses, and it misses in the role the other
    // two pass. #177a33 is the least darkening that clears 4.5 with room to
    // survive a surface moving (4.87) — the same margin rule as TEXT_FAINT's
    // high-contrast value below, where the arithmetically-least answer landed on
    // 4.51 and one rounding from failing is not a value, it is a coincidence.
    [Z_TOKEN_SUCCESS]          = {RGBA(0x30, 0xd1, 0x58, 0xff), RGBA(0x17, 0x7a, 0x33, 0xff)},
    [Z_TOKEN_WARN]             = {RGBA(0xff, 0x9f, 0x0a, 0xff), RGBA(0xc9, 0x34, 0x00, 0xff)},
    [Z_TOKEN_DANGER]           = {RGBA(0xff, 0x45, 0x3a, 0xff), RGBA(0xd7, 0x00, 0x15, 0xff)},
    // The panel tints follow the appearance, not the hue: a state-tinted SURFACE
    // is a surface first. Dark ink lands on the light ones at ~14:1.
    [Z_TOKEN_SUCCESS_DIM]      = {RGBA(0x16, 0x3d, 0x2a, 0xff), RGBA(0xd9, 0xf2, 0xe0, 0xff)},
    [Z_TOKEN_WARN_DIM]         = {RGBA(0x3d, 0x30, 0x16, 0xff), RGBA(0xff, 0xe9, 0xcc, 0xff)},
    [Z_TOKEN_DANGER_DIM]       = {RGBA(0x3d, 0x1c, 0x1e, 0xff), RGBA(0xff, 0xdc, 0xd8, 0xff)},
    [Z_TOKEN_ACCENT_DIM]       = {RGBA(0x16, 0x32, 0x4a, 0xff), RGBA(0xd9, 0xe8, 0xff, 0xff)},

    // A SCRIM DARKENS IN BOTH APPEARANCES, and that is not an oversight. It is
    // not a surface tint — it is the modal backdrop, and what it does is take
    // the thing behind it OUT of reach. On a light page a light veil would read
    // as fog rather than as depth, and every phone dims for a modal whichever
    // way round its palette runs.
    [Z_TOKEN_SCRIM]            = {RGBA(0x00, 0x00, 0x00, 0xb0), RGBA(0x00, 0x00, 0x00, 0xb0)},

    // MATERIALS: THE TINT KEEPS ITS STRUCTURE, MIRRORED. Dark goes darker as it
    // thickens (1c -> 14 -> 0e -> 08); light goes lighter (f2 -> f7 -> fb -> ff),
    // at the same alphas. Apple publishes zero numbers for any material (P52), so
    // Zelto owns these — but owning them is not the same as guessing them.
    //
    // THE BAR IS THE WORST-CASE BACKDROP, because a material is a tint over a
    // BLUR of whatever is behind it and its job is to carry its own content. The
    // hardest backdrop is the one that drags the composite towards the ink:
    // WHITE under a dark material, BLACK under a light one. Composited there,
    // with each appearance's own primary ink on top:
    //
    //             dark, over white          light, over black
    //     THIN        3.44                       4.62
    //     REGULAR     6.67                       8.04
    //     THICK      11.80                      11.95
    //     SHEET      14.58                      13.63
    //
    // Recorded rather than fixed: dark THIN over a bright wallpaper is 3.44 for
    // TEXT, under the 4.5 body bar. THIN's only caller is the dock plate, which
    // carries icons and no text, so it clears the 3.0 non-text minimum — but it
    // is why THIN is not the material to put a label on.
    [Z_TOKEN_MATERIAL_THIN]    = {RGBA(0x1c, 0x1c, 0x1e, 0x8c), RGBA(0xf2, 0xf2, 0xf7, 0x8c)},
    [Z_TOKEN_MATERIAL_REGULAR] = {RGBA(0x14, 0x14, 0x16, 0xb8), RGBA(0xf7, 0xf7, 0xfa, 0xb8)},
    [Z_TOKEN_MATERIAL_THICK]   = {RGBA(0x0e, 0x0e, 0x10, 0xdb), RGBA(0xfb, 0xfb, 0xfd, 0xdb)},
    [Z_TOKEN_MATERIAL_SHEET]   = {RGBA(0x08, 0x08, 0x0a, 0xe6), RGBA(0xff, 0xff, 0xff, 0xe6)},

    // THE RIM INVERTS, because a white rim on a white material is not a rim: the
    // shipped white@1f measures 1.008 against a light material — invisible, and
    // invisible is the whole failure mode of this phase. The dark rim measures
    // 1.363 to 1.466 against its own material, so the light one is BLACK at the
    // alpha that lands in that same band (@1f is 1.32, @2b is 1.48). Matched to
    // the rim that already exists rather than picked to taste.
    [Z_TOKEN_MATERIAL_EDGE]    = {RGBA(0xff, 0xff, 0xff, 0x1f), RGBA(0x00, 0x00, 0x00, 0x26)},

    // THE SHADOW IS BLACK IN BOTH, AT A DIFFERENT STRENGTH, and this is the one
    // value in the table with a genuinely arguable answer — so both anchors are
    // written down, not just the choice.
    //
    // The same alpha does not mean the same shadow. Black@0x80 measured as the
    // step it makes on what it falls on:
    //     on SURFACE #1c1c1e   1.13      on a white page   4.00
    //     on SURFACE_2         1.30      over a mid wallpaper #808080   2.63
    // On a light page 0x80 is not a penumbra, it is a grey smudge.
    //
    // The two defensible anchors pull opposite ways: matching the step the dark
    // shadow makes on the RAISED SURFACES it falls on wants ~0x20 (1.33 on
    // white) and nearly vanishes over the wallpaper; matching the step it makes
    // over the WALLPAPER wants ~0x60 and reads as a halo on a white page. 0x33
    // (1.61 on white) sits between them, nearer the wallpaper anchor, because in
    // light the tone ladder does less of the depth work and the shadow does
    // more — the surfaces are 1.12 apart there against dark's 1.23.
    [Z_TOKEN_SHADOW]           = {RGBA(0x00, 0x00, 0x00, 0x80), RGBA(0x00, 0x00, 0x00, 0x33)},

    // THE PRESS VEIL IS NOT A THEME QUESTION, which is why both columns are the
    // same and why there are two of these. See z_press_veil() below: the veil
    // has to oppose THE SURFACE IT IS PAINTED OVER, and the appearance does not
    // determine that — a near-white PRIMARY button exists in the DARK palette.
    [Z_TOKEN_PRESS]            = {RGBA(0xff, 0xff, 0xff, 0x3d), RGBA(0xff, 0xff, 0xff, 0x3d)},
    [Z_TOKEN_PRESS_INV]        = {RGBA(0x00, 0x00, 0x00, 0x33), RGBA(0x00, 0x00, 0x00, 0x33)},

    // Was a raw #4aa3ff in system/launcher/main.c at two alphas — the only hue
    // in the shipped OS that lived in no table. Kept and named rather than
    // deleted; the argument is beside Z_COLOR_DROP_TARGET in gfx.h. The second
    // alpha (the placeholder under the icon in hand, 0x45) is z_fade() of this.
    //
    // THE SAME IN BOTH APPEARANCES, on purpose: it is drawn on the WALLPAPER,
    // and a photograph does not change when the theme does. Legibility over the
    // wallpaper is stage 4's problem for every surface that sits on it, and
    // solving it once there beats guessing a second azure here.
    [Z_TOKEN_DROP_TARGET]      = {RGBA(0x4a, 0xa3, 0xff, 0x30), RGBA(0x4a, 0xa3, 0xff, 0x30)},
};

// --- Increase Contrast: the three tokens with a second value ---------------
// Chosen as the LEAST move that clears AA on the furthest surface each is drawn
// on — not "as far as possible", because the job of a muted ink is to be a step
// below the primary one, and a preference that flattens the hierarchy has
// substituted one unreadable screen for another. The numbers are in gfx.h.
//
// A LIST, not a table: three overrides against 29 tokens. Linear-scanned, which
// at three entries is cheaper than any structure that would avoid the scan.
//
// The light column is the same rule pointed the other way — DARKER rather than
// brighter — with one deviation worth recording: the least darkening that clears
// 4.5 for the faint ink is #5a5a5e at 4.51:1, and a value that clears its bar by
// 0.01 is one rounding away from not clearing it. #55555a (4.87) is the least
// darkening that clears it with room to be true after a surface moves.
static const struct {
    ZToken token;
    ZColor dark, light;
} g_contrast[] = {
    // dark: 4.42 -> 6.74 on SURFACE_3      light: 3.44 -> 6.00
    {Z_TOKEN_TEXT_MUTED, RGBA(0xc7, 0xc7, 0xcc, 0xff), RGBA(0x48, 0x48, 0x4a, 0xff)},
    // dark: 2.17 -> 4.81 on SURFACE_3      light: 2.14 -> 4.87
    {Z_TOKEN_TEXT_FAINT, RGBA(0xa8, 0xa8, 0xb0, 0xff), RGBA(0x55, 0x55, 0x5a, 0xff)},
    // dark: 1.40 -> 3.25 on SURFACE        light: 1.53 -> 3.25
    {Z_TOKEN_BORDER,     RGBA(0x6c, 0x6c, 0x70, 0xff), RGBA(0x86, 0x86, 0x8b, 0xff)},
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
    bool light = g_theme == Z_THEME_LIGHT;
    if (g_increase) {
        for (int i = 0; i < Z_CONTRAST_OVERRIDES; i++) {
            if (g_contrast[i].token == token) {
                return light ? g_contrast[i].light : g_contrast[i].dark;
            }
        }
    }
    return light ? g_palette[token].light : g_palette[token].dark;
}

// --- The ink that goes on a fill -------------------------------------------
// WCAG 2.1 relative luminance, then pick whichever of the palette's two INK
// POLARITIES reaches further. See the measured table over Z_COLOR_SUCCESS in
// <zelto/gfx.h> for why this is a computation rather than a token: the header
// used to prescribe TEXT_INV on any semantic fill, and TEXT_INV on the old WARN
// was 1.99:1.
//
// THE TWO CANDIDATES ARE TEXT AND ON_PRIMARY (P54), and they used to be TEXT_INV
// and ON_PRIMARY. That worked only because in a dark-only OS TEXT_INV and TEXT
// were the same colour to within 2/255. The pair has to be the palette's two
// POLARITIES in whichever appearance is current, and TEXT/ON_PRIMARY is exactly
// that by construction — TEXT is the ink on the page, ON_PRIMARY is the ink on
// the fill that inverts the page. In dark that is #f2f2f7 / #0a0a0c and in light
// it is #1c1c1e / #f2f2f7, so the rule needs no third ink and no theme test.
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
    ZColor page = Z_COLOR_TEXT, lit = Z_COLOR_ON_PRIMARY;
    return fill_contrast(lit, fill) >= fill_contrast(page, fill) ? lit : page;
}

// --- The press veil ---------------------------------------------------------
// THE VEIL WAS NEVER A THEME QUESTION, and finding that out is what P54 stage 2
// was for. Z_COLOR_PRESS is a WHITE veil at 24%, and the obvious reading is that
// it wants a black twin for the light appearance. Measured, the obvious reading
// is wrong in a way that was already shipping:
//
//     white@0x3d over dark BG      #000000    1.933   <- works
//     white@0x3d over dark PRIMARY #f2f2f7    1.026   <- EVERY FILLED BUTTON
//     white@0x3d over a light page #ffffff    1.000   <- exactly nothing
//
// The middle row is the DARK palette. Z_COLOR_PRIMARY is a near-white "lit"
// surface by design, so every Button, every active Control Center chip and every
// lit keyboard shift has had press feedback of 1.026:1 since P31 — a veil that
// is technically painted and cannot be seen. Nobody screenshots a press.
//
// So the polarity comes from the SURFACE, not the appearance. Same shape as
// z_on_fill, same reason: a rule computed from what is actually there cannot
// drift from it, and one rule fixes light mode and the dark filled button at
// once. The strengths differ because black and white are not equally strong over
// a mid tone — 0x3d white on black is 1.93, and 0x33 black on white is 1.61,
// which is as near as a byte gets while keeping the light veil from reading as
// a smear on a pale chip.
ZColor z_press_veil(ZColor under) {
    // Against BLACK rather than against 0.5: the question is which of the two
    // veils moves this surface further, and the surface's own luminance answers
    // it directly. A mid grey takes the light veil, which is the right way for a
    // tie to fall on a palette whose ink is light more often than not.
    return fill_lum(under) > 0.18 ? Z_COLOR_PRESS_INV : Z_COLOR_PRESS;
}
