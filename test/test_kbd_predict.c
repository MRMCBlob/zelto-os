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

// BOTH MODELS, IN ONE BINARY, ON PURPOSE. lm_trie.c is what ships and
// lm_bigram.c is what it falls back to — but it is also the thing the dictionary
// has to be measured AGAINST, and "the dictionary is sharper" is only a number if
// something asks the two of them the same question in the same run. z_lm_p() is
// the trie's answer and z_lm_bigram_p() is the letter-pair table's, so section 0
// below can state the difference rather than assert the conclusion.
#include "../system/keyboard/lm_bigram.c"
#include "../system/keyboard/lm_trie.c"
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

static ZKbdKey KEYS[40];
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
// Row 4, at the real proportions: a 1.7-share layer key, a 5.6-share SPACE BAR
// and a 2.0-share return, dividing the same 720-unit strip. The space bar's WIDTH
// is the point of writing this row down — it is seven times a letter cap, so its
// Gaussian is seven times as wide, and the question the space test asks is what
// happens where that overlaps the row above.
enum { SPACE_X = 147, SPACE_W = 403, ROW4_Y = 14 + 3 * ROW_PITCH };
static int SPACE_KEY;
static void add_row4(void) {
    struct { float x, w; char ch; } r[] = {
        {14.0f, 122.0f, 0},                            // "123"
        {(float)SPACE_X, (float)SPACE_W, Z_LM_BOUNDARY},
        {561.0f, 144.0f, 0},                           // return
    };
    for (int i = 0; i < 3; i++) {
        ZKbdKey *k = &KEYS[N_KEYS];
        k->x = r[i].x;
        k->y = (float)ROW4_Y;
        k->w = r[i].w;
        k->h = CAP_H;
        k->ch = r[i].ch;
        if (r[i].ch == Z_LM_BOUNDARY) {
            SPACE_KEY = N_KEYS;
        }
        N_KEYS++;
    }
}
static void build_board(void) {
    N_KEYS = 0;
    add_row("qwertyuiop", 14.0f, 14.0f);
    add_row("asdfghjkl", 54.0f, 14.0f + ROW_PITCH);
    add_row("zxcvbnm", 120.0f, 14.0f + 2 * ROW_PITCH);
    add_row4();
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
// A substitution cost that COUNTS how often it is asked. The number is the whole
// point (section I): the real callback scans the laid-out caps and takes a square
// root, so how many times the edit-distance search asks it is the difference
// between a 79us scan and a 929us one.
static int subst_calls;
static float counting_subst(void *ud, char want, char got) {
    (void)ud;
    subst_calls++;
    // Roughly the shape of the real one — adjacent letters are cheap — so the
    // answers below are the answers the keyboard would get, not a degenerate 1.0.
    return (want > got ? want - got : got - want) <= 1 ? 0.5f : 1.0f;
}

static char classify(float x, float y, const char *prefix, bool lang) {
    int i = z_kbd_classify(KEYS, N_KEYS, x, y, prefix, lang, NULL, 0);
    return i < 0 ? '?' : KEYS[i].ch;
}

int main(void) {
    char msg[240], act[64];
    build_board();

    // --- 0. the model is the one it says it is, and its order is right -------
    // Everything below rests on the model having the right SHAPE (a likely
    // continuation beating an unlikely one by a wide margin); the absolute values
    // do not matter, because the classifier consumes them as clamped odds.
    EXPECT_STR_EQ("trie+bigram", z_lm_name());
    {
        char rep[300];
        z_lm_report(rep, sizeof(rep));
        printf("    lm: %s\n", rep);
        // THE LIST IS A HUMAN-EDITED FILE AND THIS IS ITS LINT. Bands of
        // vocabulary get appended over time and the same word honestly gets
        // written twice; the builder drops the worse rank and counts it. A
        // non-zero count here means bytes are shipping in the image to say
        // something the image already says.
        snprintf(act, sizeof(act), "%d duplicates", n_dups);
        CHECK(n_dups == 0,
              "the shipped word list contains duplicate entries — they cost "
              "image bytes and say nothing, and the rank of the second copy is "
              "silently discarded", act);
        snprintf(act, sizeof(act), "%d words", n_words);
        CHECK(n_words > 1000,
              "the dictionary is far smaller than the list in words_en.h — the "
              "scanner dropped entries (anything that is not plain lowercase a-z "
              "is skipped), so the model is answering from a fraction of what "
              "was shipped", act);
    }
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

    // --- 0b. THE DICTIONARY GETS SHARPER WITH A LONGER PREFIX ----------------
    // The property that separates the two models, stated as the ratio between
    // them on the same question. A letter-pair table reads the LAST LETTER and
    // throws the rest away, so P(x | "e") and P(x | "hel") are literally the same
    // distribution to it — asserted here, because that is the baseline the
    // dictionary is being measured against and a reader should not have to take
    // it on faith.
    CHECK(z_lm_bigram_p("hel", 'l') == z_lm_bigram_p("xxxxxl", 'l'),
          "the letter-pair model gave two different answers for two prefixes "
          "ending in the same letter — it is reading more than the last letter, "
          "so it is not the baseline this section claims it is", "differs");
    {
        float t_l = z_lm_p("hel", 'l'), b_l = z_lm_bigram_p("hel", 'l');
        float t_p = z_lm_p("hel", 'p'), b_p = z_lm_bigram_p("hel", 'p');
        float t_q = z_lm_p("hel", 'q'), b_q = z_lm_bigram_p("hel", 'q');
        printf("    P(.|\"hel\")  trie: l=%.4f p=%.4f q=%.4f\n", t_l, t_p, t_q);
        printf("               bigram: l=%.4f p=%.4f q=%.4f\n", b_l, b_p, b_q);
        // 'p' is the sharpest case in the language for this: "help" is common,
        // "elp" as a letter pair is not, and only a model that knows the WORD can
        // tell those apart.
        snprintf(act, sizeof(act), "trie %.4f vs bigram %.4f", t_p, b_p);
        CHECK(t_p > 4.0f * b_p,
              "after \"hel\" the dictionary is no more confident about 'p' than "
              "the letter-pair table is — 'help' is a common word and 'lp' is a "
              "rare pair, so a model that knew any words at all would separate "
              "them", act);
        snprintf(act, sizeof(act), "trie l/q %.0fx, bigram l/q %.0fx",
                 (double)(t_l / t_q), (double)(b_l / b_q));
        CHECK(t_l / t_q > 4.0f * (b_l / b_q),
              "the dictionary does not separate a live continuation from a dead "
              "one any better than the letter-pair table does; sharpness with a "
              "longer prefix is the whole reason for shipping a word list", act);
    }

    // --- 0c. IS A WORD is a different question from IS A WORD PREFIX ---------
    // The same trie answers both and the space bar depends on the difference:
    // "hel" is a prefix of "hello" and "help" and is not a word, "hell" is both,
    // "hello" is a word. A model that conflated them would grow the space bar in
    // the middle of every word.
    CHECK(!z_lm_is_word("hel"),
          "\"hel\" was reported as a complete word — it is a PREFIX of one, and "
          "the space bar grows on the wrong question if those are the same",
          "is a word");
    CHECK(z_lm_is_word("hello"),
          "\"hello\" was not reported as a complete word", "not a word");
    CHECK(z_lm_is_word("go now"),
          "the model looked at the whole prefix instead of the word being typed "
          "— only the run of letters before the cursor is the current word",
          "not a word");
    CHECK(!z_lm_is_word("zqxj"),
          "a string the dictionary has never seen was reported as a word",
          "is a word");
    {
        float w = z_lm_p("hello", Z_LM_BOUNDARY);
        float f = z_lm_p("hel", Z_LM_BOUNDARY);
        printf("    P(space|\"hello\")=%.3f  P(space|\"hel\")=%.3f\n", w, f);
        snprintf(act, sizeof(act), "complete %.3f vs fragment %.3f", w, f);
        CHECK(w > 8.0f * f,
              "the model is no keener on a space after a complete word than "
              "after a fragment, so the space bar cannot grow — this is the one "
              "adaptive-target behaviour a user notices and the one a letter-pair "
              "table provably could not do", act);
        CHECK(z_lm_bigram_p("hello", Z_LM_BOUNDARY) == 0.0f,
              "the letter-pair model answered a question about the word "
              "boundary; it has no notion of one, and a positive answer here "
              "means the comparison above is not between two different models",
              "nonzero");
    }

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
    //
    // AND IT IS WHERE THE DICTIONARY SHOWS UP AS TERRITORY. P47 measured this at
    // 48 units with the letter-pair table. The number is printed on every run
    // rather than pinned, because it is a MEASUREMENT of two constants and a
    // model and pinning it would turn every tuning change into a test edit — but
    // the direction is asserted: a model that knows the word "hello" must claim
    // at least as much ground after "he" as one that only knows the pair "el",
    // and it must claim MORE after "hel", where the letter-pair table learns
    // nothing new and the dictionary learns a great deal.
    {
        float y = cy('l');
        int flip = PITCH;   // offset from 'l's centre at which 'k' takes over
        for (int d = 0; d <= PITCH; d++) {
            if (classify(cx('l') - (float)d, y, "he", true) != 'l') {
                flip = d;
                break;
            }
        }
        int flip3 = PITCH;
        for (int d = 0; d <= PITCH; d++) {
            if (classify(cx('l') - (float)d, y, "hel", true) != 'l') {
                flip3 = d;
                break;
            }
        }
        printf("    'l'/'k' boundary: %d units after \"he\" (P47's bigram: 48), "
               "%d after \"hel\"\n",
               flip, flip3);
        snprintf(act, sizeof(act), "\"he\" %d, \"hel\" %d", flip, flip3);
        CHECK(flip3 > flip,
              "a LONGER prefix did not buy the model any more territory: 'l' "
              "holds the same ground after \"hel\" as after \"he\". That is the "
              "defining behaviour of a letter-pair table, which reads only the "
              "last letter — so either the dictionary is not being consulted or "
              "the odds clamp is saturated and the extra confidence is thrown "
              "away", act);
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

    // --- F. THE SPACE BAR GROWS WHEN THE WORD IS FINISHED --------------------
    // The behaviour a user notices, and the one a letter-pair table provably
    // cannot produce. It is not a special case anywhere in the code: the space
    // cap carries Z_LM_BOUNDARY as its symbol, the model has an opinion about
    // that symbol, and the same argmax runs.
    //
    // WHERE TO PRESS, and why it is above the bar rather than on it. The space
    // bar's own centre zone is 100 units wide in each direction — pressing the
    // middle of it proves nothing, because the centre rule answers first. The
    // interesting point is the GAP between the space bar and the row above,
    // where its very wide Gaussian (sigma scales with the cap, and this cap is
    // seven letters wide) overlaps a letter's much narrower one. That is where a
    // thumb reaching for space actually lands short.
    // MEASURED AS A REACH, NOT AS ONE POINT. A single press proves whichever of
    // the two answers you wrote it to get; how far the target extends is the
    // thing that changed, so that is what is measured — sweeping up from the
    // bar's own top edge until a letter takes the point back.
    {
        float sx = KEYS[SPACE_KEY].x + KEYS[SPACE_KEY].w * 0.5f;
        float top = KEYS[SPACE_KEY].y;
        int reach_done = 0, reach_part = 0;
        for (int d = 0; d <= ROW_PITCH; d++) {
            if (classify(sx, top - (float)d, "hello", true) == Z_LM_BOUNDARY) {
                reach_done = d;
            } else {
                break;
            }
        }
        for (int d = 0; d <= ROW_PITCH; d++) {
            if (classify(sx, top - (float)d, "hel", true) == Z_LM_BOUNDARY) {
                reach_part = d;
            } else {
                break;
            }
        }
        printf("    space bar reaches %d units above its own top edge after the "
               "complete word \"hello\", %d after the fragment \"hel\"\n",
               reach_done, reach_part);
        snprintf(act, sizeof(act), "complete %d, fragment %d", reach_done,
                 reach_part);
        CHECK(reach_done > reach_part + 8,
              "the space bar's target is the same size whether or not the word "
              "is finished. That is a letter-pair keyboard: only a model that "
              "knows where a word ENDS can grow the space bar, and it is the one "
              "adaptive-target behaviour a user actually notices", act);
        // ...and the growth has to stop somewhere, or the bottom letter row
        // becomes untypeable the moment you finish a word.
        char b_done = classify(cx('v'), cy('v'), "hello", true);
        snprintf(msg, sizeof(msg),
                 "after a complete word, a press in the dead centre of the 'v' "
                 "cap gave '%c' — the space bar has eaten the row above it",
                 b_done == Z_LM_BOUNDARY ? '_' : b_done);
        CHECK_CH('v', b_done, msg);
    }

    // --- G. A WORD THE DICTIONARY HAS NEVER SEEN IS STILL TYPEABLE -----------
    // The failure mode a prefix tree has and a letter-pair table does not: off
    // the dictionary every continuation is probability ZERO, and zero is not
    // "unlikely", it is "untypeable off-centre". Two mechanisms stop that, and
    // both are checked here because either alone would let this pass while the
    // keyboard was unusable for names.
    {
        // 1. The model falls back. "zq" is not a prefix of anything in the list,
        //    so the trie has nothing; the letter-pair table still ranks every
        //    letter, and the answer must come from it.
        float any = 0.0f;
        for (char c = 'a'; c <= 'z'; c++) {
            if (z_lm_p("zq", c) > any) {
                any = z_lm_p("zq", c);
            }
        }
        snprintf(act, sizeof(act), "best letter %.5f", any);
        CHECK(any > 0.01f,
              "off the dictionary the model gives every letter almost no mass — "
              "a prefix tree answers zero outside its word list, and the "
              "letter-pair backoff is what stops that from making every name and "
              "password untypeable", act);
        for (char c = 'a'; c <= 'z'; c++) {
            snprintf(msg, sizeof(msg),
                     "P('%c' | \"zq\") is zero — some letter is unreachable "
                     "off-centre after a prefix the dictionary does not carry",
                     c);
            CHECK(z_lm_p("zq", c) > 0.0f, msg, "0");
        }
        // 2. ...and the geometry still answers. Every point of the row resolves
        //    to something with a prefix nothing in the dictionary continues.
        float y = cy('a');
        int dead = 0, sampled = 0;
        for (float x = KEYS[key_of('a')].x; x <= cx('l') + CAP_W; x += 1.0f) {
            sampled++;
            if (z_kbd_classify(KEYS, N_KEYS, x, y, "zqx", true, NULL, 0) < 0) {
                dead++;
            }
        }
        snprintf(act, sizeof(act), "%d of %d dead", dead, sampled);
        CHECK(dead == 0 && sampled > 500,
              "a point on the keyboard resolved to NO key while typing a string "
              "the dictionary has never seen", act);
    }

    // --- H. whole-word candidates: a DIFFERENT mechanism ---------------------
    // Not the classifier. z_lm_candidates answers "what word was that", from the
    // whole word, after it is finished — the thing autocorrect is built on. Its
    // one hard contract is that an exact dictionary hit outranks every
    // correction, however common the corrected word is.
    {
        ZLmWord w[4];
        int n = z_lm_candidates("teh", NULL, NULL, w, 4);
        snprintf(act, sizeof(act), "%d candidates, first '%s'", n,
                 n > 0 ? w[0].word : "-");
        CHECK(n > 0 && strcmp(w[0].word, "the") == 0,
              "\"teh\" did not propose \"the\" as its best candidate — a "
              "transposition of the two commonest letters in the commonest word "
              "in English is the easiest correction there is", act);

        n = z_lm_candidates("the", NULL, NULL, w, 4);
        CHECK(n > 0 && strcmp(w[0].word, "the") == 0,
              "a word that IS in the dictionary did not come back as its own "
              "best candidate", n > 0 ? w[0].word : "none");
        snprintf(act, sizeof(act), "exact %.3g vs next %.3g", w[0].score,
                 n > 1 ? w[1].score : 0.0f);
        CHECK(n > 1 && w[0].score > 1000.0f * w[1].score,
              "an exact dictionary hit did not outrank its nearest correction by "
              "a wide margin — autocorrect decides whether to fire by comparing "
              "these two numbers, so a narrow gap means a correctly spelled word "
              "can be 'corrected' into a commoner one", act);

        n = z_lm_candidates("zqxvv", NULL, NULL, w, 4);
        snprintf(act, sizeof(act), "%d candidates", n);
        CHECK(n == 0,
              "a string nothing in English is within two edits of still produced "
              "a candidate — autocorrect would fire on it", act);
        // The positive control for that absence, from the same run: the same call
        // on a plausible typo does return something, so `n == 0` above means
        // "nothing was close" and not "the search is broken".
        n = z_lm_candidates("wrold", NULL, NULL, w, 4);
        CHECK(n > 0 && strcmp(w[0].word, "world") == 0,
              "\"wrold\" did not propose \"world\" — and without this the "
              "zero-candidate assertion above proves nothing",
              n > 0 ? w[0].word : "none");
    }

    // --- H2. INFLECTED ENGLISH IS NOT DESTROYED (P49) ------------------------
    // A REGRESSION TEST FOR A SHIPPED BUG, and it is worth stating what the bug
    // was because the fix reads like a feature. words_en.h carries "walk" and not
    // "walked", which P48 recorded as a coverage decision. It was not a coverage
    // problem: measured over 60 hand-checked inflected forms of common words,
    // autocorrect REWROTE 49 of them into a different word — walked -> walk,
    // hands -> and, dogs -> does, reading -> wedding, taking -> thing. It had been
    // doing that since autocorrect shipped, invisibly, because every test and
    // every worked example in that phase used uninflected words.
    //
    // Both directions are asserted, because a rule that made everything a word
    // would pass the first half and break autocorrect entirely.
    {
        static const char *INFLECTED[] = {
            "walked", "asked", "opened", "carried", "stopped", "moved",
            "walking", "making", "running", "reading", "writing",
            "walks", "dogs", "hands", "watches", "carries",
        };
        for (int i = 0; i < (int)(sizeof(INFLECTED) / sizeof(INFLECTED[0]));
             i++) {
            snprintf(msg, sizeof(msg),
                     "\"%s\" is not a word to the model, so autocorrect will "
                     "rewrite it into a shorter one — ordinary inflected English "
                     "is not a typo",
                     INFLECTED[i]);
            CHECK(z_lm_is_word(INFLECTED[i]), msg, "not a word");
        }
        // ...and the other direction. A suffix rule that accepted anything
        // ending in s/ed/ing would silently switch autocorrect off, which no
        // assertion above would notice.
        static const char *TYPOS[] = {"teh", "wrold", "freind", "recieve",
                                      "hleped", "wnats", "zqxvv", "runing"};
        for (int i = 0; i < (int)(sizeof(TYPOS) / sizeof(TYPOS[0])); i++) {
            snprintf(msg, sizeof(msg),
                     "the typo \"%s\" now reads as a WORD — the inflection rule "
                     "may only accept a suffix on a base that is itself in the "
                     "dictionary, or it takes typos out of autocorrect's reach",
                     TYPOS[i]);
            CHECK(!z_lm_is_word(TYPOS[i]), msg, "is a word");
        }
        // The doubling rule, as its own pair: English doubles the final
        // consonant of a single-syllable C-V-C base, so "running" is a word and
        // "runing" is a typo, and only a rule that knows the difference can have
        // both. Without it "runing" strips to "run" and stops being correctable.
        CHECK(z_lm_is_word("running"), "\"running\" is not a word", "not a word");
        CHECK(!z_lm_is_word("runing"),
              "\"runing\" reads as a word — it strips to \"run\", but a "
              "single-syllable C-V-C base doubles its consonant, so this is a "
              "typo and not an inflection", "is a word");
        // ...and the exceptions that keep that rule from over-firing: w/x/y
        // never double, so these must stay words.
        CHECK(z_lm_is_word("fixing"),
              "\"fixing\" was rejected — 'x' never doubles", "not a word");
        CHECK(z_lm_is_word("buying"),
              "\"buying\" was rejected — 'y' never doubles", "not a word");
    }

    // --- I. THE SUBSTITUTION COST IS ASKED 26x26 TIMES, NOT 200,000 ----------
    // P49 item 1. The caller's substitution callback is a fact about the LAYOUT:
    // the keyboard's scans the laid-out caps for two characters and takes a
    // square root, and the DP asked it once per mismatched cell — over 1620 words
    // that is hundreds of thousands of calls for an answer with only 676 possible
    // questions in it. It was 95% of what a scan cost (929us with the callback,
    // 51us without, measured on the real list).
    //
    // Counted rather than timed, because a wall clock in a test is a flake. The
    // bound is the SHAPE of the memo — at most one call per (want, got) pair —
    // and the same call must still return the same answer, which is the second
    // assertion and the reason the first one is not enough on its own.
    {
        ZLmWord w[4];
        subst_calls = 0;
        int n = z_lm_candidates("wrold", counting_subst, NULL, w, 4);
        snprintf(act, sizeof(act), "%d calls", subst_calls);
        CHECK(subst_calls <= 26 * 26,
              "the substitution cost was asked more than 26x26 times for one "
              "candidate search — it is a fact about the layout with 676 possible "
              "answers, and asking it per DP cell is 95% of what a scan costs",
              act);
        CHECK(n > 0 && strcmp(w[0].word, "world") == 0,
              "memoising the substitution cost changed the answer — the memo is "
              "only correct if the callback is a pure function of (want, got), "
              "which is what the interface promises",
              n > 0 ? w[0].word : "none");
        // The positive control for that bound: the callback WAS consulted. A memo
        // that never called it at all would pass the assertion above trivially.
        snprintf(act, sizeof(act), "%d calls", subst_calls);
        CHECK(subst_calls > 0,
              "the substitution callback was never called, so the bound above "
              "proves nothing about memoisation", act);
    }

    // --- J. A LEARNED WORD IS ANSWERED BY THE SAME MODEL ---------------------
    // P49 item 2, and the property the whole design turns on: a learned word
    // merges INTO THE LIST the tree is built from, so it is not a second model
    // consulted alongside the first. The test of that is not "learning works", it
    // is that EVERY entry point changes together — z_lm_is_word, z_lm_is_prefix,
    // z_lm_p and z_lm_candidates. A second model would move one and not the rest.
    {
        const char *LEARNED = "zelto";
        // The negative control comes first and from the same run: before the
        // word is taught, all four say no.
        CHECK(!z_lm_is_word(LEARNED),
              "\"zelto\" was already a word before anything learned it — the "
              "before/after comparison below would prove nothing", "is a word");
        float p_before = z_lm_p("zelt", 'o');
        ZLmWord w[4];
        int n, n_before = z_lm_candidates(LEARNED, NULL, NULL, w, 4);
        unsigned gen_before = z_lm_generation();
        int words_before = z_lm_word_count();

        CHECK(z_lm_learn(LEARNED), "the model refused to learn a plain new word",
              "refused");

        CHECK(z_lm_is_word(LEARNED),
              "a learned word is not a word to z_lm_is_word — autocorrect would "
              "go on 'correcting' it, which is the exact behaviour learning "
              "exists to stop", "not a word");
        CHECK(z_lm_is_prefix("zelt"),
              "a learned word did not make its own prefix live — the classifier "
              "reads z_lm_is_prefix, so the letters would still fight the user "
              "half way through typing it", "not a prefix");
        float p_after = z_lm_p("zelt", 'o');
        snprintf(act, sizeof(act), "%.4f -> %.4f", p_before, p_after);
        CHECK(p_after > 4.0f * p_before,
              "learning a word did not change P(next | prefix) — z_lm_p is what "
              "the CLASSIFIER consumes, so a learned word that does not move it "
              "is a dictionary entry the touch targets have never heard of", act);
        n = z_lm_candidates(LEARNED, NULL, NULL, w, 4);
        CHECK(n > 0 && strcmp(w[0].word, LEARNED) == 0,
              "a learned word is not its own best candidate — the strip and "
              "autocorrect read this list, and an exact hit must win it",
              n > 0 ? w[0].word : "none");
        snprintf(act, sizeof(act), "%d before, %d after", n_before, n);
        CHECK(z_lm_generation() != gen_before,
              "z_lm_generation did not change after a learn — callers memoise "
              "answers from this model (kbd_candidates does) and have nothing "
              "else to invalidate against", "unchanged");
        CHECK(z_lm_word_count() == words_before + 1,
              "the word count did not go up by exactly one", act);

        // Learning is IDEMPOTENT. A word taught twice must not become two
        // entries — the duplicate lint at the top of this file is what would
        // eventually notice, long after the rank of the second copy was silently
        // discarded.
        CHECK(!z_lm_learn(LEARNED),
              "the model learned the same word twice", "learned again");
        CHECK(z_lm_word_count() == words_before + 1,
              "a repeated learn changed the word count", "count moved");

        // WHAT IT REFUSES. Not a policy list — the policy is in the keyboard —
        // but the shape contract the keyboard's policy is written against.
        CHECK(!z_lm_learn("Zelto"), "a capital was accepted into an a-z trie",
              "accepted");
        CHECK(!z_lm_learn("zelto2"), "a digit was accepted", "accepted");
        CHECK(!z_lm_learn("z"), "a one-letter word was accepted", "accepted");
        CHECK(!z_lm_learn(""), "an empty string was accepted", "accepted");

        // A LEARNED WORD MUST NOT OUTRANK THE SHIPPED LIST AT THE SAME EDIT
        // DISTANCE. LM_LEARN_RANK is what stops learning from turning into "the
        // last thing you typed is now the commonest word in English".
        //
        // THE PROBE HAS TO HOLD EVERYTHING BUT THE RANK CONSTANT, which the first
        // attempt at this assertion did not: it learned "teg" and checked that
        // "teh" still corrected to "the" — and it did, at LM_LEARN_RANK 0, because
        // "the" is a TRANSPOSITION of "teh" (cost 0.8) and "teg" a substitution
        // (cost 1.0), so the edit discount decided it and the rank never came into
        // it. The negative test caught that: the break passed. Here both
        // candidates are exactly one edit from the typed string, so rank is the
        // only thing left to decide between them.
        CHECK(z_lm_learn("worla"), "could not learn the test word", "refused");
        n = z_lm_candidates("worl", NULL, NULL, w, 4);
        snprintf(act, sizeof(act), "best '%s'", n > 0 ? w[0].word : "none");
        CHECK(n > 0 && strcmp(w[0].word, "worla") != 0,
              "a word learned ONCE now beats the shipped dictionary at the same "
              "edit distance — a learned word is common FOR YOU, not commoner "
              "than the words everybody uses, and LM_LEARN_RANK is what says so",
              act);

        // ...and FORGET puts it all back. The delete half of the privacy
        // decision: Settings > Keyboard > Clear Learned Words calls this.
        z_lm_forget_all();
        CHECK(!z_lm_is_word(LEARNED),
              "a forgotten word is still a word — Clear Learned Words is the "
              "only way out of the store, and it has to actually work",
              "still a word");
        snprintf(act, sizeof(act), "%d words", z_lm_word_count());
        CHECK(z_lm_word_count() == words_before,
              "forgetting did not restore the shipped word count", act);
        CHECK(z_lm_is_word("hello"),
              "forgetting the learned words took the SHIPPED dictionary with it",
              "hello is gone");
    }

    // --- K. THE CONTRACTION TABLE, LINTED -----------------------------------
    // P49 item 3b. The table is data about English (words_en.h) and its safety
    // rule — only contractions whose bare form is not itself a word — is a human
    // judgement that no test can make. What a test CAN do is catch the mechanical
    // ways the table goes wrong: a duplicate, and an expansion that is not its own
    // bare form with an apostrophe put back.
    {
        for (int i = 0; i < N_CONTRACTIONS_EN; i++) {
            const char *bare = CONTRACTIONS_EN[i].bare;
            const char *full = CONTRACTIONS_EN[i].full;
            CHECK(z_lm_contraction(bare) == full,
                  "a contraction in the table is not reachable through "
                  "z_lm_contraction", bare);
            // Strip the apostrophe from the expansion and it must be the bare
            // form, case-insensitively ("im" -> "I'm" is the one that needs the
            // case slack, and it is the only transformation allowed here).
            char stripped[32];
            int k = 0;
            for (const char *p = full; *p && k < (int)sizeof(stripped) - 1; p++) {
                if (*p == '\'') {
                    continue;
                }
                stripped[k++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
            }
            stripped[k] = '\0';
            snprintf(msg, sizeof(msg),
                     "the expansion of \"%s\" is \"%s\", which is not that word "
                     "with an apostrophe put back — an expansion may only insert "
                     "punctuation, never change a letter",
                     bare, full);
            CHECK(strcmp(stripped, bare) == 0, msg, full);
            int apos = 0;
            for (const char *p = full; *p; p++) {
                apos += (*p == '\'');
            }
            snprintf(msg, sizeof(msg),
                     "\"%s\" -> \"%s\" inserts %d apostrophes; exactly one is "
                     "what a contraction is", bare, full, apos);
            CHECK(apos == 1, msg, full);
            for (int j = i + 1; j < N_CONTRACTIONS_EN; j++) {
                snprintf(msg, sizeof(msg),
                         "\"%s\" appears twice in the contraction table", bare);
                CHECK(strcmp(bare, CONTRACTIONS_EN[j].bare) != 0, msg, bare);
            }
        }
        CHECK(z_lm_contraction("hello") == NULL,
              "an ordinary word came back from the contraction table", "found");
        // The one the rule exists for, stated as a test so that adding it later
        // has to argue with this line: "were" is an English word and must never
        // be rewritten to "we're".
        CHECK(z_lm_contraction("were") == NULL,
              "\"were\" is in the contraction table — it is a real English word "
              "(\"they were here\"), and the rule for what may be in that table "
              "is precisely that the bare form is NOT one", "found");
        CHECK(z_lm_contraction("its") == NULL,
              "\"its\" is in the contraction table — \"its colour\" is not a "
              "typo for \"it's\"", "found");
    }

    return zt_result();
}
