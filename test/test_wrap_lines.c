// test_wrap_lines — the greedy line breaker behind WrapText.
//
// WHY THIS EXISTS. The toolkit had no text wrapping: layout is one intrinsic-
// size pass, so a Text measures to one line and prose wider than its column runs
// off the right edge. P43 papered over it by hand-breaking Settings' captions on
// '\n'; P44 built z_wrap_lines() so the split happens at build time against the
// real font. The one guarantee the whole thing rests on is NO LINE EXCEEDS THE
// COLUMN — a caption that silently leaves the screen is exactly the bug this
// replaces — and a greedy breaker has enough corner cases (a word wider than the
// column, a hard '\n', trailing spaces, running out of line slots) that the
// guarantee is worth pinning without a font in the loop.
//
// The measure function is a stub: every character is one unit wide. That makes
// "fits in max_w" mean "<= max_w characters", so the expected breaks are
// countable by hand, and the algorithm — not a font — is what is under test. It
// is the same seam WrapText uses in production (it passes the real shaper).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"

#include "framework/ztest.h"

// z_wrap_lines lives in layout.c; pull the TU in (as test_hit_test_clip.c does)
// and stub the two measure-time externs it references but this test never hits.
float z_text_measure(ZText *t, const char *s, float size, ZWeight weight,
                     float *ascent, float *descent) {
    (void)t; (void)s; (void)size; (void)weight; (void)ascent; (void)descent;
    return 0.0f;
}
bool z_image_intrinsic(const char *path, int *w, int *h) {
    (void)path; (void)w; (void)h;
    return false;
}
#include "layout.c"

// One unit per byte. z_wrap_lines guarantees measure(s, len) <= max_w for every
// emitted line, so with this stub every line is at most max_w characters.
static float mono(void *ud, const char *s, int len) {
    (void)ud; (void)s;
    return (float)len;
}

// Assert one emitted line equals an expected C string.
static void expect_line(const ZWrapLine *l, const char *want, int idx) {
    int wlen = (int)strlen(want);
    if (l->len != wlen || strncmp(l->s, want, (size_t)wlen) != 0) {
        char got[128];
        int n = l->len < 127 ? l->len : 127;
        memcpy(got, l->s, (size_t)n);
        got[n] = '\0';
        char msg[64];
        snprintf(msg, sizeof(msg), "line %d mismatch", idx);
        zt_fail_(__FILE__, __LINE__, msg, want, got);
    }
}

// No line may be wider than the column. The invariant, checked on every case.
static void expect_within(const ZWrapLine *lines, int n, float max_w) {
    for (int i = 0; i < n; i++) {
        if ((float)lines[i].len > max_w) {
            char msg[64], exp[32], act[32];
            snprintf(msg, sizeof(msg), "line %d overflows column", i);
            snprintf(exp, sizeof(exp), "<= %g", (double)max_w);
            snprintf(act, sizeof(act), "%d", lines[i].len);
            zt_fail_(__FILE__, __LINE__, msg, exp, act);
        }
    }
}

int main(void) {
    ZWrapLine lines[16];

    // --- A. a plain paragraph breaks on word boundaries ---------------------
    // "the quick brown fox" at width 10: "the quick"(9) fits, +" brown" would be
    // 15, so break; "brown fox"(9) fits.
    {
        int n = z_wrap_lines("the quick brown fox", 10.0f, mono, NULL, lines, 16);
        ASSERT_EQ_INT(2, n);
        expect_line(&lines[0], "the quick", 0);
        expect_line(&lines[1], "brown fox", 1);
        expect_within(lines, n, 10.0f);
    }

    // --- B. a word WIDER than the column is broken mid-word ------------------
    // The guarantee that matters: "supercalifragilistic" (20) at width 8 must
    // NOT emit a 20-wide line. It is cut into 8-char pieces.
    {
        int n = z_wrap_lines("supercalifragilistic", 8.0f, mono, NULL, lines, 16);
        EXPECT_TRUE(n >= 3);
        expect_line(&lines[0], "supercal", 0);
        expect_line(&lines[1], "ifragili", 1);
        expect_line(&lines[2], "stic", 2);
        expect_within(lines, n, 8.0f);
    }

    // --- C. a hard '\n' always breaks, even with room to spare --------------
    {
        int n = z_wrap_lines("a\nb", 100.0f, mono, NULL, lines, 16);
        ASSERT_EQ_INT(2, n);
        expect_line(&lines[0], "a", 0);
        expect_line(&lines[1], "b", 1);
    }

    // --- D. a blank line is kept as a paragraph gap -------------------------
    {
        int n = z_wrap_lines("one\n\ntwo", 100.0f, mono, NULL, lines, 16);
        ASSERT_EQ_INT(3, n);
        expect_line(&lines[0], "one", 0);
        expect_line(&lines[1], "", 1);       // the empty line survives
        expect_line(&lines[2], "two", 2);
    }

    // --- E. trailing spaces are trimmed off an emitted line -----------------
    // "aa   bb" at width 4: "aa"(2) fits, "aa   bb" would be 7 -> break after
    // "aa", and the run of spaces before "bb" is consumed, not carried down.
    {
        int n = z_wrap_lines("aa   bb", 4.0f, mono, NULL, lines, 16);
        ASSERT_EQ_INT(2, n);
        expect_line(&lines[0], "aa", 0);     // no trailing spaces
        expect_line(&lines[1], "bb", 1);     // no leading indent
    }

    // --- F. max_lines is a hard cap (a screenful, then stop) ----------------
    {
        int n = z_wrap_lines("a b c d e f", 1.0f, mono, NULL, lines, 3);
        ASSERT_EQ_INT(3, n);                 // never writes past the buffer
        expect_within(lines, n, 1.0f);
    }

    // --- G. the empty string and a bad column are handled, not crashed ------
    {
        ASSERT_EQ_INT(0, z_wrap_lines("", 10.0f, mono, NULL, lines, 16));
        ASSERT_EQ_INT(0, z_wrap_lines("x", 10.0f, mono, NULL, lines, 0));
        // No measure function: everything is one line (the degrade path when the
        // font failed to open).
        int n = z_wrap_lines("a b c", 10.0f, NULL, NULL, lines, 16);
        ASSERT_EQ_INT(1, n);
        expect_line(&lines[0], "a b c", 0);
    }

    return zt_result();
}
