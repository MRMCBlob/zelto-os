// test_dynamic_type — the Dynamic Type ladder, at the seam.
//
// WHAT IS UNDER TEST. z_font_units() is the only place a ZFont step becomes a
// number the shaper sees (sdk/src/type.c), and everything the user's text size
// does, it does here: 19 surfaces pass compile-time steps to Font() and none of
// them will ever be told a setting exists. So the ladder's RULES are worth
// pinning without a font, a compositor or a boot in the loop — the rendering
// half is test_text_size_sim.sh's job.
//
// THE RULES, and each is a decision written down in <zelto/ui.h> rather than a
// consequence of the code:
//
//   1. The default step changes nothing. An unset device draws exactly what it
//      drew before this existed, which is what makes shipping the seam safe.
//   2. Each size step is a constant POINT OFFSET applied to every step alike —
//      NOT a multiplier. This is the one that is easy to get wrong and hard to
//      see: x1.35 everywhere gives a 54pt Large Title while Caption stays small,
//      and the additive rule is what makes the large steps move less in RATIO
//      while every step gains the same absolute room.
//   3. The small end floors at Caption2's own size rather than passing under it.
//   4. Z_FONT_DISPLAY (the lock clock) does not scale at all.
//   5. Out-of-range steps CLAMP. The value comes from a brokered string any
//      process can write, and a corrupt store must give a legible screen.
//   6. z_text_scaling_disable() pins the process to the default, whatever the
//      setting says — the status bar, the keyboard and the home indicator.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

#include "internal.h"

#include "framework/ztest.h"

// EXPECT_NEAR stringifies its arguments, which for a ladder of enum arithmetic
// says nothing a reader can act on ("EXPECT_NEAR((float)Z_FONT_BODY + want,
// z_font_units(Z_FONT_BODY))"). These carry the sentence and put the measured
// number in the actual column — the same trick as test_kbd_predict.c.
#define NEAR_MSG(want, got, msg)                                               \
    do {                                                                       \
        double w_ = (double)(want), g_ = (double)(got);                        \
        if (!(fabs(w_ - g_) <= 0.01)) {                                        \
            char wb_[48], gb_[48];                                             \
            snprintf(wb_, sizeof(wb_), "%g", w_);                              \
            snprintf(gb_, sizeof(gb_), "%g", g_);                              \
            zt_fail_(__FILE__, __LINE__, (msg), wb_, gb_);                     \
        }                                                                      \
    } while (0)

#define INT_MSG(want, got, msg)                                                \
    do {                                                                       \
        long long w_ = (long long)(want), g_ = (long long)(got);               \
        if (w_ != g_) {                                                        \
            char wb_[32], gb_[32];                                             \
            snprintf(wb_, sizeof(wb_), "%lld", w_);                            \
            snprintf(gb_, sizeof(gb_), "%lld", g_);                            \
            zt_fail_(__FILE__, __LINE__, (msg), wb_, gb_);                     \
        }                                                                      \
    } while (0)

// type.c is pure arithmetic over its own file-statics plus z_line_height (which
// only z_row_h calls). Pull the TU in and stub the one extern, exactly as
// test_wrap_lines.c pulls in layout.c. The stub is a VARIABLE, so section 8 can
// drive z_row_h from both sides of its floor without a font.
static float g_line_height = 40.0f;
float z_line_height(ZApp *app, ZFont size) {
    (void)app;
    (void)size;
    return g_line_height;
}
#include "type.c"

static const int g_expect_pt[Z_TEXT_SIZE_STEPS] = Z_TEXT_SIZE_OFFSETS;

int main(void) {
    // --- 1. the default step is the identity ------------------------------
    // Checked FIRST and across the whole ramp, because it is the claim that
    // says this phase cannot have moved anything on a device nobody configured.
    z_text_size_apply(Z_TEXT_SIZE_DEFAULT, false);
    NEAR_MSG((float)Z_FONT_CAPTION2, z_font_units(Z_FONT_CAPTION2),
             "Caption2 at the default text size must be its own value");
    NEAR_MSG((float)Z_FONT_BODY, z_font_units(Z_FONT_BODY),
             "Body at the default text size must be its own value");
    NEAR_MSG((float)Z_FONT_LARGE_TITLE, z_font_units(Z_FONT_LARGE_TITLE),
             "Large Title at the default text size must be its own value");
    INT_MSG(Z_TEXT_SIZE_DEFAULT, z_text_size(),
            "z_text_size() reports the step it was given");

    // --- 2. every step is the SAME point offset ---------------------------
    // The additive rule, stated as the test: at any size, Body and Large Title
    // have gained the identical number of units. If somebody reimplements this
    // as a multiplier the two deltas diverge immediately (x1.35 gives Body +11
    // and Large Title +26), and this is the assertion that says so.
    for (int step = 0; step < Z_TEXT_SIZE_STEPS; step++) {
        z_text_size_apply(step, false);
        float want = (float)Z_PT(g_expect_pt[step]);
        char msg[200];

        snprintf(msg, sizeof(msg),
                 "Body at step %d must gain exactly Z_PT(%d) = %.0f units",
                 step, g_expect_pt[step], (double)want);
        NEAR_MSG((float)Z_FONT_BODY + want, z_font_units(Z_FONT_BODY), msg);

        snprintf(msg, sizeof(msg),
                 "Large Title at step %d must gain the SAME Z_PT(%d) as Body - "
                 "an additive ladder, not a ratio",
                 step, g_expect_pt[step]);
        NEAR_MSG((float)Z_FONT_LARGE_TITLE + want,
                 z_font_units(Z_FONT_LARGE_TITLE), msg);
    }

    // The consequence, spelled out so the intent survives a refactor: the RATIO
    // at the largest size is bigger for a small step than for a large one. That
    // is the whole reason the rule is additive, and it is not visible in the
    // per-step equalities above.
    z_text_size_apply(Z_TEXT_SIZE_STEPS - 1, false);
    float r_small = z_font_units(Z_FONT_FOOTNOTE) / (float)Z_FONT_FOOTNOTE;
    float r_large = z_font_units(Z_FONT_LARGE_TITLE) / (float)Z_FONT_LARGE_TITLE;
    if (!(r_small > r_large + 0.10f)) {
        char act[64];
        snprintf(act, sizeof(act), "Footnote x%.3f vs Large Title x%.3f",
                 (double)r_small, (double)r_large);
        zt_fail_(__FILE__, __LINE__,
                 "at the largest text size Footnote must grow proportionally "
                 "MORE than Large Title - small steps move more, which is the "
                 "whole point of an additive ladder",
                 "a clearly larger ratio", act);
    }

    // --- 3. the floor -----------------------------------------------------
    // At the smallest step Caption2 would land 6 units under itself. It stops.
    z_text_size_apply(0, false);
    NEAR_MSG((float)Z_FONT_CAPTION2, z_font_units(Z_FONT_CAPTION2),
             "Caption2 must floor at its own size, not shrink below it");
    // Positive control for the floor: at the SAME step a larger step is still
    // above the floor and must therefore have moved. Without this, a floor that
    // accidentally applied to every step would pass the line above.
    NEAR_MSG((float)Z_FONT_BODY + (float)Z_PT(g_expect_pt[0]),
             z_font_units(Z_FONT_BODY),
             "positive control: Body at the smallest step is above the floor "
             "and must actually shrink");

    // --- 4. the display step is exempt ------------------------------------
    for (int step = 0; step < Z_TEXT_SIZE_STEPS; step++) {
        z_text_size_apply(step, false);
        char msg[140];
        snprintf(msg, sizeof(msg),
                 "Z_FONT_DISPLAY (the lock clock) must not scale - step %d",
                 step);
        NEAR_MSG((float)Z_FONT_DISPLAY, z_font_units(Z_FONT_DISPLAY), msg);
    }

    // --- 5. out of range clamps -------------------------------------------
    z_text_size_apply(-400, false);
    INT_MSG(0, z_text_size(), "a negative step clamps to the smallest");
    z_text_size_apply(9999, false);
    INT_MSG(Z_TEXT_SIZE_STEPS - 1, z_text_size(),
            "an over-large step clamps to the largest");
    NEAR_MSG((float)Z_FONT_BODY +
                 (float)Z_PT(g_expect_pt[Z_TEXT_SIZE_STEPS - 1]),
             z_font_units(Z_FONT_BODY),
             "a clamped step still renders at the largest size, not at a "
             "garbage one");

    // --- 6. the change detector -------------------------------------------
    // The caller uses this to decide whether it owes a FULL REPAINT, and a
    // detector that always says "yes" repaints every surface in the OS on every
    // unrelated setting write.
    z_text_size_apply(2, false);
    INT_MSG(0, z_text_size_apply(2, false) ? 1 : 0,
            "re-applying the same (step, bold) reports no change");
    INT_MSG(1, z_text_size_apply(2, true) ? 1 : 0,
            "flipping Bold Text alone reports a change");
    INT_MSG(1, z_text_bold() ? 1 : 0, "Bold Text is on after that");
    INT_MSG(1, z_text_size_apply(4, true) ? 1 : 0,
            "changing the step alone reports a change");

    // --- 7. the opt-out pins the process to the default --------------------
    // The status bar, the keyboard and the home indicator. Asserted at a step
    // that is NOT the default and with Bold on, so a stub that happened to
    // return the default value could not pass by luck.
    z_text_size_apply(Z_TEXT_SIZE_STEPS - 1, true);
    NEAR_MSG((float)Z_FONT_BODY +
                 (float)Z_PT(g_expect_pt[Z_TEXT_SIZE_STEPS - 1]),
             z_font_units(Z_FONT_BODY),
             "positive control: scaling is ON immediately before the opt-out");
    z_text_scaling_disable();
    NEAR_MSG((float)Z_FONT_BODY, z_font_units(Z_FONT_BODY),
             "after z_text_scaling_disable() Body is the shipped size however "
             "large the setting is");
    INT_MSG(Z_TEXT_SIZE_DEFAULT, z_text_size(),
            "an opted-out process reports the default step");
    INT_MSG(0, z_text_bold() ? 1 : 0,
            "an opted-out process is not bolded either");

    // --- 8. z_row_h: the floor wins until the type outgrows it -------------
    // 44pt (81 units) is a TOUCH TARGET, and a row must never be shorter than
    // one however small the text gets; once a line plus its breathing room is
    // taller than that, the row is the line's. The line heights below are the
    // ones the sim measures for Body at the default and largest text sizes.
    g_line_height = 40.0f;
    NEAR_MSG((float)Z_ROW_H, z_row_h(NULL),
             "at the default text size the touch-target floor still wins, so "
             "deriving the row height cannot have moved any existing screen");
    g_line_height = 10.0f;
    NEAR_MSG((float)Z_ROW_H, z_row_h(NULL),
             "a tiny line height cannot shrink a row under the touch target");
    g_line_height = 53.0f;
    NEAR_MSG(53.0f + 2.0f * (float)Z_ROW_VPAD, z_row_h(NULL),
             "once the line outgrows the floor the row is the line plus its "
             "padding, not the 44pt spec number");

    return zt_result();
}
