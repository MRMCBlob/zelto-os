// meta/kbd-measure — the keyboard's language model, measured (P49).
//
// WHY THIS IS A PROGRAM AND NOT A TEST. A test pins a property and fails when it
// moves. These are NUMBERS: what a candidate scan costs, what the missing
// inflections cost in real corrections, whether typos still get fixed. They are
// the evidence behind decisions written down in words_en.h and lm_trie.c, and a
// reader who wants to check one should be able to re-run it rather than take the
// comment's word for it. Build and run:
//
//     bash meta/kbd-measure.sh
//
// The three sections and what each one settled:
//
//   1. COST. z_lm_candidates over the shipped list, with and without a
//      substitution callback of the shape the keyboard supplies. The gap between
//      them (929us vs 51us) is what put the memo in lm_trie.c, and the "after"
//      column is what makes the strip affordable to compute per keystroke.
//
//      THIS IS THE HOST NUMBER, and the host is a desktop CPU. The same scan on
//      the target measures 3403us — 37x — which is why the keyboard reports its
//      own cost there too (zelto.kbdcaps=1, the "[keyboard] cost suggest" line in
//      a KBDTAP run). Run both; a decision from one of them is half a decision.
//
//   2. INFLECTIONS. words_en.h ships bases and not inflections and called that a
//      coverage decision. It was not: before the suffix rule, 49 of 60 real
//      inflected forms were REWRITTEN into a different word by autocorrect.
//
//   3. THE CONTROL FOR 2. A suffix rule that accepted anything ending in "s"
//      would score 60/60 on section 2 and switch autocorrect off entirely, so the
//      typo list is run in the same program and reported beside it.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../system/keyboard/lm_bigram.c"
#include "../system/keyboard/lm_trie.c"

// --- 1. cost -----------------------------------------------------------------
// A stand-in for the laid-out CapSet at the real keyboard's proportions (the
// same row test_kbd_predict builds), so the substitution callback does the same
// linear scan and square root the keyboard's does. Timing against a synthetic
// constant would measure a different function.
enum { CAP_W = 58, CAP_H = 77, PITCH = 69, ROW_PITCH = 88 };
static struct { float x, y, w, h; char ch; } CAPS[40];
static int N_CAPS;
static void add_row(const char *chars, float x0, float y) {
    for (const char *p = chars; *p; p++) {
        CAPS[N_CAPS].x = x0 + (float)(p - chars) * PITCH;
        CAPS[N_CAPS].y = y;
        CAPS[N_CAPS].w = CAP_W;
        CAPS[N_CAPS].h = CAP_H;
        CAPS[N_CAPS].ch = *p;
        N_CAPS++;
    }
}
static float subst_cost(void *ud, char want, char got) {
    (void)ud;
    float wx = 0, wy = 0, gx = 0, gy = 0, cw = 0;
    int wf = 0, gf = 0;
    for (int i = 0; i < N_CAPS; i++) {
        if (CAPS[i].ch == want) {
            wx = CAPS[i].x + CAPS[i].w * 0.5f;
            wy = CAPS[i].y + CAPS[i].h * 0.5f;
            cw = CAPS[i].w;
            wf = 1;
        }
        if (CAPS[i].ch == got) {
            gx = CAPS[i].x + CAPS[i].w * 0.5f;
            gy = CAPS[i].y + CAPS[i].h * 0.5f;
            gf = 1;
        }
    }
    if (!wf || !gf || cw <= 0.0f) {
        return 1.0f;
    }
    float dx = wx - gx, dy = wy - gy;
    return sqrtf(dx * dx + dy * dy) < 1.6f * cw ? 0.5f : 1.0f;
}
static double us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void section_cost(int reps) {
    add_row("qwertyuiop", 14.0f, 14.0f);
    add_row("asdfghjkl", 54.0f, 14.0f + ROW_PITCH);
    add_row("zxcvbnm", 120.0f, 14.0f + 2 * ROW_PITCH);
    // A growing prefix of a real word (what a hold rebuilds against), two typos,
    // and a long non-word — the worst case for the length gate.
    static const char *W[] = {"h",   "he",    "hel",      "hell",    "hello",
                              "teh", "wrold", "keyboard", "internationaly"};
    int nw = (int)(sizeof(W) / sizeof(W[0]));
    ZLmWord cand[3];
    double worst = 0.0, worst_plain = 0.0;
    const char *worst_w = "";
    printf("1. CANDIDATE SCAN COST (%d words, %d reps each)\n", z_lm_word_count(),
           reps);
    printf("   %-16s %12s %12s\n", "typed", "with subst", "no subst");
    for (int i = 0; i < nw; i++) {
        double t0 = us();
        for (int r = 0; r < reps; r++) {
            z_lm_candidates(W[i], subst_cost, NULL, cand, 3);
        }
        double a = (us() - t0) / reps;
        t0 = us();
        for (int r = 0; r < reps; r++) {
            z_lm_candidates(W[i], NULL, NULL, cand, 3);
        }
        double b = (us() - t0) / reps;
        if (a > worst) {
            worst = a;
            worst_plain = b;
            worst_w = W[i];
        }
        printf("   %-16s %9.1f us %9.1f us\n", W[i], a, b);
    }
    printf("   worst \"%s\": %.0fus with the layout's substitution cost, %.0fus "
           "without it\n",
           worst_w, worst, worst_plain);
    printf("   a 40Hz build would pay %.2f%% of a frame if it ran this every "
           "build; kbd_candidates memoises on the word, so it runs once per "
           "keystroke\n",
           worst / 25000.0 * 100.0);
}

// --- 2/3. inflections, and the control ---------------------------------------
static const char *INFLECTED[] = {
    "walked",  "asked",    "called",  "looked",  "worked",  "played",
    "wanted",  "needed",   "helped",  "opened",  "started", "watched",
    "turned",  "moved",    "lived",   "showed",  "waited",  "carried",
    "tried",   "stopped",  "walking", "asking",  "calling", "looking",
    "working", "playing",  "waiting", "helping", "opening", "starting",
    "watching","turning",  "moving",  "living",  "showing", "reading",
    "writing", "running",  "making",  "taking",  "walks",   "asks",
    "calls",   "looks",    "works",   "plays",   "books",   "rooms",
    "hands",   "words",    "names",   "cars",    "dogs",    "trees",
    "birds",   "chairs",   "tables",  "letters", "windows", "gardens",
};
static const char *TYPOS[] = {
    "teh",     "wrold",    "recieve", "freind",  "thsi",     "hte",
    "becuase", "wroking",  "hleped",  "dogss",   "alwasy",   "tommorow",
    "seperate","wnats",    "lookd",   "runing",
};

static void section_words(void) {
    int n = (int)(sizeof(INFLECTED) / sizeof(INFLECTED[0]));
    int ok = 0, wrong = 0;
    printf("\n2. INFLECTED ENGLISH (%d hand-checked real forms)\n", n);
    for (int i = 0; i < n; i++) {
        if (z_lm_is_word(INFLECTED[i])) {
            ok++;
            continue;
        }
        ZLmWord c[1];
        if (z_lm_candidates(INFLECTED[i], NULL, NULL, c, 1) > 0) {
            wrong++;
            printf("   REWRITTEN  %-10s -> %s\n", INFLECTED[i], c[0].word);
        } else {
            printf("   not a word, no candidate: %s\n", INFLECTED[i]);
        }
    }
    printf("   read as words %d/%d, REWRITTEN into another word %d/%d\n", ok, n,
           wrong, n);

    n = (int)(sizeof(TYPOS) / sizeof(TYPOS[0]));
    int fixed = 0;
    printf("\n3. THE CONTROL: real typos must still be corrected (%d)\n", n);
    for (int i = 0; i < n; i++) {
        if (z_lm_is_word(TYPOS[i])) {
            printf("   NOT CORRECTED, reads as a word: %s\n", TYPOS[i]);
            continue;
        }
        if (z_lm_is_prefix(TYPOS[i])) {
            printf("   NOT CORRECTED, live prefix:     %s\n", TYPOS[i]);
            continue;
        }
        ZLmWord c[1];
        if (z_lm_candidates(TYPOS[i], NULL, NULL, c, 1) > 0) {
            fixed++;
            printf("   %-10s -> %s\n", TYPOS[i], c[0].word);
        } else {
            printf("   no candidate close enough:      %s\n", TYPOS[i]);
        }
    }
    printf("   %d/%d still corrected\n", fixed, n);
}

int main(int argc, char **argv) {
    char rep[320];
    z_lm_report(rep, sizeof(rep));
    printf("model: %s\n\n", rep);
    section_cost(argc > 1 ? atoi(argv[1]) : 200);
    section_words();
    return 0;
}
