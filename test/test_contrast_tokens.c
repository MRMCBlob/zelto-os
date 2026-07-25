// test_contrast_tokens — the palette's contrast ratios, computed from the tokens.
//
// WHAT IS UNDER TEST. gfx.h is the single source of truth for every colour in
// the OS (121 raw hexes were migrated into it), and until P51 it carried the
// comment "Text (on the surface tones — all AA)". That comment was wrong, and it
// was wrong in the direction that matters: Z_COLOR_TEXT_FAINT — the disclosure
// chevron on every Settings row, the disabled detail column — is 2.67:1 on a
// card and 2.17:1 on a chip, where WCAG AA wants 4.5:1 for body text and 3:1
// even for large text. Nobody had measured, so nobody knew.
//
// So this file MEASURES, from the tokens themselves rather than from a table
// somebody typed: the ratios are computed by the same WCAG 2.1 relative-
// luminance formula a checker uses, over the actual macro expansions. Retint a
// token and this test re-derives; it cannot go stale.
//
// THE THREE THINGS IT ASSERTS, and the middle one is the point:
//
//   1. With Increase Contrast ON, every ink/surface pair the OS draws clears its
//      bar — 4.5:1 for text, 3:1 for a hairline (WCAG's non-text minimum).
//   2. With it OFF, at least one pair does NOT. That is not a bug being
//      enshrined, it is the POSITIVE CONTROL for assertion 1: a setting whose
//      "on" state passes and whose "off" state also passes is a setting that
//      does nothing, and both would pass against a z_ink_faint() that ignored
//      its argument entirely.
//   3. The HIERARCHY survives in both modes. The failure mode of a contrast
//      preference is flattening — if TEXT_FAINT is brightened all the way to
//      TEXT, the screen is legible and every distinction on it is gone.
//
// NEGATIVE-TESTED: making z_ink_faint() return the same colour in both branches
// fails (2) naming the pair that no longer differs; returning Z_COLOR_TEXT for
// the high-contrast faint ink passes (1) and fails (3).
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include <zelto/gfx.h>

#include "framework/ztest.h"

#include "theme.c"

// WCAG 2.1 relative luminance. The channel transfer is sRGB's, not a gamma of
// 2.2 — the difference is small in the mid greys and this palette lives there.
static double chan(unsigned v) {
    double c = (double)v / 255.0;
    return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
static double lum(ZColor c) {
    return 0.2126 * chan(c.r) + 0.7152 * chan(c.g) + 0.0722 * chan(c.b);
}
static double ratio(ZColor a, ZColor b) {
    double la = lum(a), lb = lum(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// WHICH APPEARANCE a failure is in. Every assertion below runs over both, and
// "TEXT_MUTED does not reach 4.5" names half a defect if it does not say where.
static const char *g_where = "dark";

// <ink> on <surface> must reach <bar>. The message carries the measured number
// because "expected >= 4.5, got 2.17" is the finding, not a hint towards it.
static void need(const char *ink_name, ZColor ink, const char *surf_name,
                 ZColor surf, double bar, const char *why) {
    double r = ratio(ink, surf);
    if (r + 0.005 < bar) {
        char want[64], got[64], msg[256];
        snprintf(want, sizeof(want), ">= %.1f:1", bar);
        snprintf(got, sizeof(got), "%.2f:1", r);
        snprintf(msg, sizeof(msg), "[%s] %s on %s does not reach %s (%s)",
                 g_where, ink_name, surf_name, why, "WCAG 2.1 AA");
        zt_fail_(__FILE__, __LINE__, msg, want, got);
    }
}

// Everything an appearance has to satisfy. Called once per ZTheme (P54): a
// palette that is only ever measured in the appearance its author was looking at
// is how TEXT_FAINT shipped at 2.17:1 for eleven phases under a header claiming
// "all AA". The light column is newer than the dark one and therefore likelier
// to be wrong, and it is the one nobody screenshots.
static void audit_appearance(void);

int main(void) {
    // --- 0. THE TABLE IS COMPLETE -----------------------------------------
    // The palette is a designated-initialiser table indexed by ZToken (P54), so
    // a token added to the enum and forgotten in the table is not a compile
    // error — it is a silently TRANSPARENT colour, which paints nothing and
    // looks like a layout bug on whatever surface happened to use it. Nothing in
    // this OS is legitimately alpha 0 (the transparent fixed-gap idiom writes
    // z_rgba(0,0,0,0) directly and never goes through a token), so a zero alpha
    // is exactly the missing row.
    z_contrast_apply(false);
    for (int t = 0; t < Z_TOKEN_COUNT; t++) {
        if (z_token((ZToken)t).a == 0) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "token %d has no row in the palette table - a token the "
                     "enum knows and the table does not is a transparent colour, "
                     "not a compile error",
                     t);
            zt_fail_(__FILE__, __LINE__, msg, "a colour", "alpha 0");
        }
    }
    // And out of range is LOUD. The two quiet answers — clamp to the first
    // entry, or read past the table — both paint a plausible screen.
    ZColor oob = z_token((ZToken)Z_TOKEN_COUNT);
    if (!(oob.r == 0xff && oob.g == 0x00 && oob.b == 0xff && oob.a == 0xff)) {
        zt_fail_(__FILE__, __LINE__,
                 "z_token() of an out-of-range token must be the one colour "
                 "that is not in the palette, so a bad index is the loudest "
                 "thing on the screen rather than a plausible one",
                 "magenta", "something paintable");
    }

    // --- THE APPEARANCES ---------------------------------------------------
    // Every assertion from here down runs twice. The loop is the point of the
    // stage: a light palette that is never measured is a light palette that is
    // wrong, and it would be wrong in the theme no screenshot is taken of.
    struct { ZTheme t; const char *name; } themes[] = {
        {Z_THEME_DARK, "dark"}, {Z_THEME_LIGHT, "light"},
    };
    for (int th = 0; th < 2; th++) {
        g_where = themes[th].name;
        z_theme_apply(themes[th].t);
        audit_appearance();
    }

    // The appearance is a real switch, not a table read twice. Without this a
    // g_palette whose light column was a copy of its dark one would satisfy
    // every ratio above by construction — the same shape of hole the Increase
    // Contrast positive control fills.
    z_contrast_apply(false);
    z_theme_apply(Z_THEME_DARK);
    ZColor bg_dark = Z_COLOR_BG, text_dark = Z_COLOR_TEXT;
    z_theme_apply(Z_THEME_LIGHT);
    ZColor bg_light = Z_COLOR_BG, text_light = Z_COLOR_TEXT;
    if (!(lum(bg_light) > lum(bg_dark) && lum(text_light) < lum(text_dark))) {
        zt_fail_(__FILE__, __LINE__,
                 "the light appearance must invert the page and its ink - if "
                 "the two columns of the palette do not differ, every ratio "
                 "measured above was measured twice on the same numbers",
                 "light page with dark ink", "the dark palette again");
    }
    // And z_theme_apply must report a move, for z_text_size_apply's reason: a
    // detector that always says true repaints every surface in the OS on every
    // unrelated setting write, and one that never does leaves the phone in the
    // appearance it booted in.
    z_theme_apply(Z_THEME_LIGHT);
    if (z_theme_apply(Z_THEME_LIGHT)) {
        zt_fail_(__FILE__, __LINE__,
                 "re-applying the same appearance must report no change",
                 "false", "true");
    }
    if (!z_theme_apply(Z_THEME_DARK)) {
        zt_fail_(__FILE__, __LINE__, "changing the appearance must report it",
                 "true", "false");
    }
    if (z_theme() != Z_THEME_DARK) {
        zt_fail_(__FILE__, __LINE__,
                 "z_theme() must report the appearance it was given", "dark",
                 "something else");
    }

    return zt_result();
}

static void audit_appearance(void) {
    // --- 1. Increase Contrast ON: everything clears its bar ----------------
    // The surfaces are the four an ink is drawn on in this OS. SURFACE_4 is not
    // in the list on purpose: it is the keyboard's raised key cap, and the only
    // ink on it is Z_COLOR_TEXT.
    z_contrast_apply(true);
    struct { const char *n; ZColor c; } surfs[] = {
        {"BG", Z_COLOR_BG},
        {"SURFACE", Z_COLOR_SURFACE},
        {"SURFACE_2", Z_COLOR_SURFACE_2},
        {"SURFACE_3", Z_COLOR_SURFACE_3},
    };
    for (int i = 0; i < 4; i++) {
        need("Z_COLOR_TEXT", Z_COLOR_TEXT, surfs[i].n, surfs[i].c, 4.5,
             "the body-text minimum");
        need("Z_COLOR_TEXT_MUTED (Increase Contrast)", Z_COLOR_TEXT_MUTED,
             surfs[i].n, surfs[i].c, 4.5, "the body-text minimum");
        need("Z_COLOR_TEXT_FAINT (Increase Contrast)", Z_COLOR_TEXT_FAINT,
             surfs[i].n, surfs[i].c, 4.5, "the body-text minimum");
    }
    // A hairline is not text. WCAG's bar for a boundary that carries meaning —
    // and the divider between two rows of a settings list is the whole reason
    // the card reads as a list — is 3:1, against the surface it separates.
    need("Z_COLOR_BORDER (Increase Contrast)", Z_COLOR_BORDER, "SURFACE",
         Z_COLOR_SURFACE, 3.0, "the non-text minimum");

    // --- 2. the positive control: OFF, it does not ------------------------
    // Stated as the specific pair rather than "something fails", so a future
    // palette that fixes the faint ink outright makes this line fail LOUDLY and
    // ask whether the setting still has a job, instead of passing on some other
    // token's accident.
    z_contrast_apply(false);
    double faint_card = ratio(Z_COLOR_TEXT_FAINT, Z_COLOR_SURFACE_2);
    if (!(faint_card < 4.5)) {
        char got[64];
        snprintf(got, sizeof(got), "%.2f:1", faint_card);
        zt_fail_(__FILE__, __LINE__,
                 "positive control: with Increase Contrast OFF the shipped "
                 "Z_COLOR_TEXT_FAINT on a card must still be the sub-AA ink "
                 "this setting exists to fix - if it now passes, the preference "
                 "has nothing to do and assertion 1 proves nothing",
                 "< 4.5:1 (the shipped default)", got);
    }
    // And the swap must actually be a swap: the same token, read in both modes,
    // has to differ. Without this a z_ink_faint() that ignored g_increase would
    // satisfy everything above by accident of the shipped value being bright.
    z_contrast_apply(true);
    ZColor faint_hi = Z_COLOR_TEXT_FAINT, muted_hi = Z_COLOR_TEXT_MUTED,
           border_hi = Z_COLOR_BORDER;
    z_contrast_apply(false);
    ZColor faint_lo = Z_COLOR_TEXT_FAINT, muted_lo = Z_COLOR_TEXT_MUTED,
           border_lo = Z_COLOR_BORDER;
    struct { const char *n; ZColor hi, lo; } moved[] = {
        {"Z_COLOR_TEXT_FAINT", faint_hi, faint_lo},
        {"Z_COLOR_TEXT_MUTED", muted_hi, muted_lo},
        {"Z_COLOR_BORDER", border_hi, border_lo},
    };
    // AWAY FROM THE SURFACE, not "lighter" — which is what this said until P54,
    // and it was only ever true because the OS had one appearance. In light the
    // same preference must make these inks DARKER. Contrast against the surface
    // they sit on is the direction-free way to say the thing that was meant.
    z_contrast_apply(false);
    ZColor card = Z_COLOR_SURFACE_2;
    for (int i = 0; i < 3; i++) {
        if (ratio(moved[i].hi, card) <= ratio(moved[i].lo, card)) {
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "[%s] %s must move FURTHER from the surface it is drawn on "
                     "under Increase Contrast - a token that did not move (or "
                     "moved towards it) is a preference that does nothing",
                     g_where, moved[i].n);
            zt_fail_(__FILE__, __LINE__, msg, "more contrast with the setting on",
                     "the same or less");
        }
    }

    // --- 3. the hierarchy survives in both modes ---------------------------
    // Primary > secondary > de-emphasised. This is what stops "make it readable"
    // from becoming "make it all the same", which is the way a contrast setting
    // usually goes wrong: three inks that all pass AA and no longer say anything
    // about which text matters.
    //
    // STATED AS CONTRAST WITH THE PAGE, not as brightness. "Brighter" was the
    // right word for a dark-only OS and is exactly backwards in light; what was
    // always meant is that the primary ink stands off the page furthest.
    for (int mode = 0; mode < 2; mode++) {
        z_contrast_apply(mode != 0);
        ZColor page = Z_COLOR_BG;
        double t = ratio(Z_COLOR_TEXT, page), m = ratio(Z_COLOR_TEXT_MUTED, page),
               f = ratio(Z_COLOR_TEXT_FAINT, page);
        char msg[240];
        snprintf(msg, sizeof(msg),
                 "[%s] with Increase Contrast %s the primary ink must stand off "
                 "the page further than the secondary one",
                 g_where, mode ? "ON" : "OFF");
        if (!(t > m)) {
            zt_fail_(__FILE__, __LINE__, msg, "TEXT above TEXT_MUTED",
                     "not above it");
        }
        snprintf(msg, sizeof(msg),
                 "[%s] with Increase Contrast %s the secondary ink must stand "
                 "off the page further than the de-emphasised one",
                 g_where, mode ? "ON" : "OFF");
        if (!(m > f)) {
            zt_fail_(__FILE__, __LINE__, msg, "TEXT_MUTED above TEXT_FAINT",
                     "not above it");
        }
    }

    // --- 3b. THE SURFACE LADDER IS MONOTONE, AND IT SEPARATES ---------------
    // The direction belongs to the appearance; the ORDER does not. sdk/src/view.c
    // reads the ladder as a direction (`active ? SURFACE_3 : SURFACE_2`) and so
    // does every z_color_lerp(SURFACE_3, PRIMARY, v) track, so a ladder that
    // turns round in the middle makes "one step more prominent" mean two
    // different things on two surfaces. This is the assertion behind the
    // light-column decision recorded in theme.c — it is the reason the light
    // palette descends from white rather than following iOS's grouped ladder up.
    z_contrast_apply(false);
    struct { const char *n; ZColor c; } ladder[] = {
        {"BG", Z_COLOR_BG}, {"SURFACE", Z_COLOR_SURFACE},
        {"SURFACE_2", Z_COLOR_SURFACE_2}, {"SURFACE_3", Z_COLOR_SURFACE_3},
    };
    for (int i = 1; i < 4; i++) {
        double prev = ratio(ladder[i - 1].c, Z_COLOR_BG);
        double here = ratio(ladder[i].c, Z_COLOR_BG);
        if (i > 1 && !(here > prev)) {
            char msg[240];
            snprintf(msg, sizeof(msg),
                     "[%s] the surface ladder is not monotone: %s is not further "
                     "from the page than %s",
                     g_where, ladder[i].n, ladder[i - 1].n);
            zt_fail_(__FILE__, __LINE__, msg, "a step away from the page",
                     "the same or back towards it");
        }
        // And each step has to be VISIBLE as a step. 1.10 is the floor rather
        // than a target: iOS's own #f2f2f7-on-#fff card separation measures
        // 1.116, so a bar above that would fail Apple's grouped list.
        double step = ratio(ladder[i].c, ladder[i - 1].c);
        if (step < 1.10) {
            char msg[240], got[64];
            snprintf(got, sizeof(got), "%.3f:1", step);
            snprintf(msg, sizeof(msg),
                     "[%s] %s does not separate from %s - two surfaces the OS "
                     "draws on top of each other that read as one",
                     g_where, ladder[i].n, ladder[i - 1].n);
            zt_fail_(__FILE__, __LINE__, msg, ">= 1.10:1", got);
        }
    }

    // --- 4. the change detector -------------------------------------------
    // Same contract as z_text_size_apply: the caller repaints on true, and a
    // detector that always says true repaints every surface in the OS on every
    // unrelated setting write. (z_theme_apply's own is in main(), because it is
    // the thing this function is being run twice by.)
    z_contrast_apply(false);
    if (z_contrast_apply(false)) {
        zt_fail_(__FILE__, __LINE__,
                 "re-applying the same contrast setting must report no change",
                 "false", "true");
    }
    if (!z_contrast_apply(true)) {
        zt_fail_(__FILE__, __LINE__,
                 "turning Increase Contrast on must report a change",
                 "true", "false");
    }
    if (!z_contrast_increased()) {
        zt_fail_(__FILE__, __LINE__,
                 "z_contrast_increased() must report the setting it was given",
                 "true", "false");
    }
    z_contrast_apply(false);

    // --- 5. THE SEMANTIC COLOURS, IN BOTH THE ROLES THEY ARE USED IN -------
    //
    // P51 recorded two semantic failures and called them "the fill wants
    // redrawing". Measured against what the OS actually DRAWS, that was wrong
    // twice: these tokens are mostly INK (the charging battery glyph, the home
    // widget's percentage), and the pair the header PRESCRIBED — TEXT_INV on a
    // semantic fill — was worse than either failure P51 listed (1.99:1 on the
    // old WARN). So both roles are asserted here, and neither was before.
    z_contrast_apply(false);
    struct { const char *n; ZColor c; } sem[] = {
        {"Z_COLOR_SUCCESS", Z_COLOR_SUCCESS},
        {"Z_COLOR_WARN", Z_COLOR_WARN},
        {"Z_COLOR_DANGER", Z_COLOR_DANGER},
    };

    // 5a. AS A FILL: whatever z_on_fill picks must clear the body-text bar. This
    // is the assertion that makes the rule trustworthy — a caller that goes
    // through it can never produce an unreadable pair, whatever the fill becomes.
    for (int i = 0; i < 3; i++) {
        char ink[64];
        snprintf(ink, sizeof(ink), "z_on_fill(%s)", sem[i].n);
        need(ink, z_on_fill(sem[i].c), sem[i].n, sem[i].c, 4.5,
             "the body-text minimum on a saturated fill");
    }
    // The same for PRIMARY, which is a fill with its own named ink: the rule
    // must AGREE with the token, or one of the two is wrong.
    if (lum(z_on_fill(Z_COLOR_PRIMARY)) != lum(Z_COLOR_ON_PRIMARY)) {
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "[%s] z_on_fill disagrees with Z_COLOR_ON_PRIMARY about what "
                 "ink belongs on a PRIMARY fill - the computed rule and the "
                 "named token must not contradict each other",
                 g_where);
        zt_fail_(__FILE__, __LINE__, msg, "ON_PRIMARY", "the other ink");
    }

    // 5b. AS INK: a semantic colour is drawn ON the surfaces, and that is the
    // role the old green failed in — 2.61 on a card and 2.13 on a chip, under
    // even WCAG's 3:1 non-text bar. These are glyphs and large numerals, so 3:1
    // is the right bar; the point is that it is now a bar at all.
    struct { const char *n; ZColor c; } on[] = {
        {"BG", Z_COLOR_BG}, {"SURFACE", Z_COLOR_SURFACE},
        {"SURFACE_2", Z_COLOR_SURFACE_2}, {"SURFACE_3", Z_COLOR_SURFACE_3},
    };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            need(sem[i].n, sem[i].c, on[j].n, on[j].c, 3.0,
                 "the non-text minimum for a state glyph");
        }
    }

    // 5c. POSITIVE CONTROL for 5a. Rule 5a can only fail if z_on_fill is wrong,
    // and a z_on_fill that returned one ink unconditionally would still pass on
    // some of the fills. So assert the thing that would actually be broken: the
    // WRONG ink on WARN is a failure, and the rule does not pick it.
    //
    // "The wrong ink" is stated as the OTHER of the two candidates rather than
    // as "the light one", because which of them is light depends on the
    // appearance — in dark the failure was TEXT_INV (now TEXT) on WARN at 1.99;
    // in light it is ON_PRIMARY's opposite number. The rule under test is the
    // same either way: the computed pick must beat the pick it rejected.
    ZColor picked = z_on_fill(Z_COLOR_WARN);
    ZColor other = (picked.r == Z_COLOR_TEXT.r && picked.g == Z_COLOR_TEXT.g &&
                    picked.b == Z_COLOR_TEXT.b)
                       ? Z_COLOR_ON_PRIMARY
                       : Z_COLOR_TEXT;
    double rejected = ratio(other, Z_COLOR_WARN);
    if (!(rejected < 4.5)) {
        char got[64], msg[240];
        snprintf(got, sizeof(got), "%.2f:1", rejected);
        snprintf(msg, sizeof(msg),
                 "[%s] positive control: the ink z_on_fill REJECTED for WARN "
                 "must still be a failing pair - if both candidates pass, 5a "
                 "could be satisfied by a rule that ignores its argument",
                 g_where);
        zt_fail_(__FILE__, __LINE__, msg, "< 4.5:1", got);
    }
    if (ratio(picked, Z_COLOR_WARN) <= rejected) {
        char msg[240];
        snprintf(msg, sizeof(msg),
                 "[%s] z_on_fill picked the WORSE of its two inks for WARN",
                 g_where);
        zt_fail_(__FILE__, __LINE__, msg, "the ink that reaches further",
                 "the other one");
    }
}
