// test_kbd_predict — the adaptive-target classifier, as arithmetic.
//
// WHAT THIS IS FOR, and what it deliberately is NOT. The sim test
// (test_keyboard_predict_sim) proves the keyboard types "hello" while missing
// two of the keys, end to end, on the real layout. That test is the claim. This
// one is the tuning: it feeds the classifier a synthetic row whose geometry is an
// INPUT rather than an assertion, and pins the properties the constants in
// predict.h are chosen to produce. Splitting them matters because the sim test
// can only fail one way — "it typed the wrong thing" — and cannot say whether the
// Gaussian is too wide, the odds clamp too loose or the centre zone too small.
//
// The geometry here is written down, which everywhere else in this project is a
// bug. It is not one here: this file is not asking where a key IS, it is asking
// what the scorer does with a key at a given place. The one thing it must not do
// is claim the real keyboard has these numbers — so it builds a row from a pitch
// and a width the way a layout would, and the sim test owns the real frames.
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "framework/ztest.h"

#include "../system/keyboard/lm_bigram.c"
#include "../system/keyboard/predict.c"

// EXPECT_TRUE stringifies its condition, which for the checks below says nothing
// a reader can act on ("EXPECT_TRUE(flip > CAP_W / 2 + 4)"). These carry the
// sentence instead, and put the measured number in the actual column.
#define CHECK(cond, msg, act)                                                  \
    do {                                                                       \
        if (!(cond)) {                                                         \
            zt_fail_(__FILE__, __LINE__, (msg), "true", (act));                \
        }                                                                      \
    } while (0)
#define CHECK_CH(want, got, msg)                                               \
    do {                                                                       \
        char w_[2] = {(char)(want), 0}, g_[2] = {(char)(got), 0};              \
        if (w_[0] != g_[0]) {                                                  \
            zt_fail_(__FILE__, __LINE__, (msg), w_, g_);                       \
        }                                                                      \
    } while (0)

// A three-row QWERTY at the proportions the real one has at 720x1440 (measured
// by the P46 cap audit: 59-unit caps on a 70-unit pitch in row 1, 58 on 69 in
// row 2 which is inset by half a key, 88 between row tops).
enum { CAP_W = 58, CAP_H = 77, PITCH = 69, ROW_PITCH = 88 };

static ZKbdKey KEYS[32];
static int N_KEYS;

static void add_row(const char *chars, float x0, float y) {
    for (const char *p = chars; *p; p++) {
        ZKbdKey *k = &KEYS[N_KEYS++];
        k->x = x0 + (float)(p - chars) * PITCH;
        k->y = y;
        k->w = CAP_W;
        k->h = CAP_H;
        k->ch = *p;
    }
}
static void build_board(void) {
    N_KEYS = 0;
    add_row("qwertyuiop", 14.0f, 14.0f);
    add_row("asdfghjkl", 54.0f, 14.0f + ROW_PITCH);
    add_row("zxcvbnm", 120.0f, 14.0f + 2 * ROW_PITCH);
}
static int key_of(char ch) {
    for (int i = 0; i < N_KEYS; i++) {
        if (KEYS[i].ch == ch) {
            return i;
        }
    }
    return -1;
}
static float cx(char ch) {
    int i = key_of(ch);
    return KEYS[i].x + KEYS[i].w * 0.5f;
}
static float cy(char ch) {
    int i = key_of(ch);
    return KEYS[i].y + KEYS[i].h * 0.5f;
}
static char classify(float x, float y, const char *prefix, bool lang) {
    int i = z_kbd_classify(KEYS, N_KEYS, x, y, prefix, lang, NULL, 0);
    return i < 0 ? '?' : KEYS[i].ch;
}

int main(void) {
    char msg[240], act[64];
    build_board();

    // --- 0. the model is the one it says it is, and its order is right -------
    // Everything below rests on the table having the right SHAPE (a listed pair
    // beating an unlisted one by a wide margin); the absolute values do not
    // matter, because the classifier consumes them as clamped odds.
    EXPECT_STR_EQ("bigram", z_lm_name());
    CHECK(z_lm_p("he", 'l') > 8.0f * z_lm_p("he", 'k'),
          "'el' is a common English bigram and 'ek' is not, so P(l|..e) must be "
          "far above P(k|..e) — this is the fact that corrects the first miss",
          "not 8x");
    CHECK(z_lm_p("hel", 'l') > 8.0f * z_lm_p("hel", 'k'),
          "'ll' is common and 'lk' is not", "not 8x");
    CHECK(z_lm_p("hell", 'o') > 8.0f * z_lm_p("hell", 'p'),
          "'lo' is common and 'lp' is not — this corrects the second miss",
          "not 8x");
    // A word boundary uses a DIFFERENT distribution, and this is how you can
    // tell: 't' begins one English word in six but is not the commonest letter
    // overall, so this comparison inverts if the backoff uses the wrong table.
    CHECK(z_lm_p("", 't') > z_lm_p("", 'e'),
          "at a word boundary the model must back off to word-INITIAL "
          "frequencies, where t beats e; the overall letter frequencies would "
          "put e first", "e >= t");

    // --- A. a dead-centre press is that key, always --------------------------
    // The property that the keyboard's ability to type a password, a name, or
    // any word the model has never seen depends on. Asserted against an
    // ADVERSARIAL prefix: 'q' after "hel" is about as unlikely as English gets,
    // and 'w' — which "lw" does not license either — sits right beside it.
    for (int i = 0; i < N_KEYS; i++) {
        char why[32];
        int got = z_kbd_classify(KEYS, N_KEYS, KEYS[i].x + KEYS[i].w * 0.5f,
                                 KEYS[i].y + KEYS[i].h * 0.5f, "hel", true,
                                 why, sizeof(why));
        snprintf(msg, sizeof(msg),
                 "a press in the dead centre of '%c' gave '%c' — the language "
                 "model overrode a deliberate, accurate press",
                 KEYS[i].ch, got < 0 ? '?' : KEYS[got].ch);
        CHECK_CH(KEYS[i].ch, got < 0 ? '?' : KEYS[got].ch, msg);
        // ...and that it was the RULE that answered, not the scores agreeing by
        // luck. Without this the loop cannot tell a working guarantee from a
        // guarantee that is never reached.
        snprintf(msg, sizeof(msg),
                 "the centre-zone rule did not decide a dead-centre press on "
                 "'%c' (the classifier answered '%s') — the guarantee is not on "
                 "the path it is supposed to be on",
                 KEYS[i].ch, why);
        CHECK(strcmp(why, "centre") == 0, msg, why);
    }
    // WHY THERE IS NO "the model would otherwise have won" CONTROL HERE, which
    // is what this comment exists to record: it would not have. At the shipped
    // constants the centre rule is REDUNDANT, and that is provable rather than
    // lucky. The best a neighbour one pitch away can score is its Gaussian times
    // the odds ceiling; the worst the key under the finger can score is 1.0 times
    // the odds floor. Below, the first is two thirds of an order of magnitude
    // under the second, so no prefix in any language can take a centre press.
    //
    // The rule stays because that inequality is a property of two constants
    // somebody will one day want to change — widen SIGMA to make the keyboard
    // more forgiving, raise ODDS to trust the model more — and the guarantee
    // must not quietly depend on them. This assertion is the lint that says so:
    // if it ever fails, the rule has stopped being belt-and-braces and started
    // being the only thing holding the keyboard's ability to type a password.
    {
        float w = (float)CAP_W, s = Z_KBD_SIGMA * w;
        float p = (float)PITCH;
        float best_neighbour = expf(-(p * p) / (2.0f * s * s)) * Z_KBD_ODDS;
        float worst_centre = 1.0f / Z_KBD_ODDS;
        snprintf(act, sizeof(act), "neighbour %.4f vs centre %.4f",
                 best_neighbour, worst_centre);
        CHECK(best_neighbour < worst_centre,
              "SIGMA and ODDS no longer guarantee a dead-centre press on their "
              "own: a maximally-likely neighbour one pitch away now outscores a "
              "maximally-unlikely key under the finger, so the centre-zone rule "
              "has become load-bearing rather than belt-and-braces", act);
    }

    // --- B. every point resolves to some key — the dead gutter is gone --------
    // Swept across the whole home row INCLUDING the gutters, which under plain
    // rectangles are the ~15% of each row that hits nothing at all (the number
    // the P46 cap audit prints every run).
    {
        float y = cy('a');
        int dead = 0, sampled = 0;
        for (float x = KEYS[key_of('a')].x; x <= cx('l') + CAP_W; x += 1.0f) {
            sampled++;
            if (z_kbd_classify(KEYS, N_KEYS, x, y, "", false, NULL, 0) < 0) {
                dead++;
            }
        }
        snprintf(act, sizeof(act), "%d sampled", sampled);
        CHECK(sampled > 500, "the sweep actually covered the row", act);
        snprintf(act, sizeof(act), "%d of %d dead", dead, sampled);
        CHECK(dead == 0,
              "a point on the keyboard resolved to NO key — the classifier is "
              "what makes every point mean something, which is what removes the "
              "dead gutter", act);
    }

    // --- C. the model claims the gutter and the neighbour's outer half -------
    // Where the boundary between two keys sits after a confident prefix. Stated
    // as a range rather than a number: it has to be past the gutter (else the
    // model is doing nothing at all) and short of the neighbour's centre zone
    // (else assertion A is only saved by a special case). Z_KBD_SIGMA is tuned
    // to exactly this, and this is the assertion that says so.
    {
        float y = cy('l');
        int flip = PITCH;   // offset from 'l's centre at which 'k' takes over
        for (int d = 0; d <= PITCH; d++) {
            if (classify(cx('l') - (float)d, y, "he", true) != 'l') {
                flip = d;
                break;
            }
        }
        snprintf(act, sizeof(act), "flips at %d", flip);
        snprintf(msg, sizeof(msg),
                 "after \"he\", 'l' holds territory only out to %d units from "
                 "its centre; its own painted cap already reaches %d, so the "
                 "model is not claiming even the gutter",
                 flip, CAP_W / 2);
        CHECK(flip > CAP_W / 2 + 4, msg, act);
        float into_k = (float)(PITCH - flip);   // distance from 'k's centre
        snprintf(act, sizeof(act), "%.0f units from k's centre", into_k);
        snprintf(msg, sizeof(msg),
                 "after \"he\", 'l' wins to within %.0f units of 'k's OWN "
                 "centre, and the inviolable centre zone is %.0f — the model is "
                 "eating the middle of the neighbouring key",
                 into_k, Z_KBD_CENTRE * CAP_W * 0.5f);
        CHECK(into_k > Z_KBD_CENTRE * CAP_W * 0.5f, msg, act);
    }

    // --- D. the same press, with and without the model -----------------------
    // The discriminator, in miniature: one point, two answers. 45 units left of
    // 'l's centre is 24 units from 'k's centre — well inside the painted 'k' cap
    // and well outside its centre zone.
    CHECK_CH('k', classify(cx('l') - 45.0f, cy('l'), "he", false),
             "with the model OFF, a press inside the 'k' cap must be 'k' — "
             "geometry and nothing else");
    CHECK_CH('l', classify(cx('l') - 45.0f, cy('l'), "he", true),
             "with the model ON, that same press after \"he\" must be 'l'");
    CHECK_CH('p', classify(cx('o') + 45.0f, cy('o'), "hell", false),
             "with the model OFF, a press inside the 'p' cap must be 'p'");
    CHECK_CH('o', classify(cx('o') + 45.0f, cy('o'), "hell", true),
             "with the model ON, that same press after \"hell\" must be 'o'");

    // --- E. a modifier is not a letter ---------------------------------------
    // A key with no character (shift, delete) gets no language vote, so a
    // confident prefix cannot pull a press off it. Appended at the end of the
    // home row, where an aggressive 'l' would otherwise reach.
    {
        ZKbdKey *mod = &KEYS[N_KEYS++];
        mod->x = cx('l') + PITCH - CAP_W * 0.5f;
        mod->y = KEYS[key_of('l')].y;
        mod->w = 95.0f;
        mod->h = CAP_H;
        mod->ch = 0;
        int got = z_kbd_classify(KEYS, N_KEYS, mod->x + mod->w * 0.5f,
                                 mod->y + mod->h * 0.5f, "hel", true, NULL, 0);
        EXPECT_EQ_INT(N_KEYS - 1, got);
        N_KEYS--;
    }

    return zt_result();
}
