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

#include "contrast.c"

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

// <ink> on <surface> must reach <bar>. The message carries the measured number
// because "expected >= 4.5, got 2.17" is the finding, not a hint towards it.
static void need(const char *ink_name, ZColor ink, const char *surf_name,
                 ZColor surf, double bar, const char *why) {
    double r = ratio(ink, surf);
    if (r + 0.005 < bar) {
        char want[64], got[64], msg[256];
        snprintf(want, sizeof(want), ">= %.1f:1", bar);
        snprintf(got, sizeof(got), "%.2f:1", r);
        snprintf(msg, sizeof(msg), "%s on %s does not reach %s (%s)", ink_name,
                 surf_name, why, "WCAG 2.1 AA");
        zt_fail_(__FILE__, __LINE__, msg, want, got);
    }
}

int main(void) {
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
    for (int i = 0; i < 3; i++) {
        if (lum(moved[i].hi) <= lum(moved[i].lo)) {
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "%s must get LIGHTER under Increase Contrast - it is drawn "
                     "on a dark palette, so a token that did not move (or moved "
                     "down) is a preference that does nothing",
                     moved[i].n);
            zt_fail_(__FILE__, __LINE__, msg, "brighter with the setting on",
                     "the same or darker");
        }
    }

    // --- 3. the hierarchy survives in both modes ---------------------------
    // Primary > secondary > de-emphasised. This is what stops "make it readable"
    // from becoming "make it all the same", which is the way a contrast setting
    // usually goes wrong: three inks that all pass AA and no longer say anything
    // about which text matters.
    for (int mode = 0; mode < 2; mode++) {
        z_contrast_apply(mode != 0);
        double t = lum(Z_COLOR_TEXT), m = lum(Z_COLOR_TEXT_MUTED),
               f = lum(Z_COLOR_TEXT_FAINT);
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "with Increase Contrast %s the primary ink must stay brighter "
                 "than the secondary one",
                 mode ? "ON" : "OFF");
        if (!(t > m)) {
            zt_fail_(__FILE__, __LINE__, msg, "TEXT brighter than TEXT_MUTED",
                     "not brighter");
        }
        snprintf(msg, sizeof(msg),
                 "with Increase Contrast %s the secondary ink must stay "
                 "brighter than the de-emphasised one",
                 mode ? "ON" : "OFF");
        if (!(m > f)) {
            zt_fail_(__FILE__, __LINE__, msg,
                     "TEXT_MUTED brighter than TEXT_FAINT", "not brighter");
        }
    }

    // --- 4. the change detector -------------------------------------------
    // Same contract as z_text_size_apply: the caller repaints on true, and a
    // detector that always says true repaints every surface in the OS on every
    // unrelated setting write.
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
        zt_fail_(__FILE__, __LINE__,
                 "z_on_fill disagrees with Z_COLOR_ON_PRIMARY about what ink "
                 "belongs on a PRIMARY fill - the computed rule and the named "
                 "token must not contradict each other",
                 "ON_PRIMARY", "the other ink");
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
    // and a z_on_fill that returned the LIGHT ink unconditionally would still
    // pass on SUCCESS today. So assert the thing that was actually broken: the
    // light ink on WARN is a failure, and the rule does not pick it.
    double light_on_warn = ratio(Z_COLOR_TEXT_INV, Z_COLOR_WARN);
    if (!(light_on_warn < 4.5)) {
        char got[64];
        snprintf(got, sizeof(got), "%.2f:1", light_on_warn);
        zt_fail_(__FILE__, __LINE__,
                 "positive control: light ink on WARN must still be the failing "
                 "pair this rule exists to avoid - if it now passes, 5a could be "
                 "satisfied by a z_on_fill that ignores its argument",
                 "< 4.5:1", got);
    }
    if (lum(z_on_fill(Z_COLOR_WARN)) >= lum(Z_COLOR_TEXT_INV)) {
        zt_fail_(__FILE__, __LINE__,
                 "z_on_fill picked the LIGHT ink for WARN, which is the 1.99:1 "
                 "pair the header used to prescribe",
                 "the dark ink", "the light ink");
    }

    return zt_result();
}
