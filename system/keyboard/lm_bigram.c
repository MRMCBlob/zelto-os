// The language model, bigram edition. See the seam's contract in predict.h.
//
// WHAT THIS MODEL CAN AND CANNOT DO, because the difference shows up in the
// keyboard's behaviour and it should not be a mystery which one is running:
// it knows that 'l' often follows 'e' and that 'k' almost never does, so a press
// in the gutter between them after "he" resolves to 'l'. It does NOT know the
// word "hello" — it has no idea a word is being spelled at all. Two consequences
// a dictionary would not have: it cannot grow the SPACE bar when what you have
// typed is a complete word (the one Kocienda-keyboard behaviour most visible to a
// user), and it gets no better with a longer prefix, because it only ever reads
// the last letter.
//
// It is compiled in: 26 unigram frequencies, 26 word-initial frequencies and ~130
// letter pairs, which is under a kilobyte, needs no data file in the image and
// has no licence attached. A prefix tree over a shipped word list is the upgrade,
// and it replaces THIS FILE ONLY — z_lm_name() is what tells you which one
// answered.
//
// The frequencies are the standard English corpus figures (percent of all
// letters / of all bigrams). They are approximate on purpose: the classifier
// consumes them as ODDS against a uniform alphabet and clamps the result to
// Z_KBD_ODDS, so the third significant figure cannot change a decision. What
// matters is the ORDER — that "ll" and "lo" are common and "lk" and "lp" are not.
#include "predict.h"

#include <ctype.h>
#include <string.h>

// Percent of all letters in English text.
static const float UNI[26] = {
    8.20f, 1.50f, 2.80f, 4.30f, 12.70f, 2.20f, 2.00f, 6.10f, 7.00f,
    0.15f, 0.77f, 4.00f, 2.40f, 6.70f, 7.50f, 1.90f, 0.095f, 6.00f,
    6.30f, 9.10f, 2.80f, 0.98f, 2.40f, 0.15f, 2.00f, 0.074f,
};

// Percent of words STARTING with each letter — a different distribution, and the
// one that applies after a space or at an empty field. ('t' begins one word in
// six, mostly "the"; 'e' begins one in thirty-six despite being the commonest
// letter overall.)
static const float INITIAL[26] = {
    11.60f, 4.40f, 5.20f, 3.20f, 2.80f, 4.00f, 1.60f, 7.20f, 5.40f,
    0.50f, 0.50f, 2.70f, 4.20f, 2.40f, 6.30f, 2.50f, 0.20f, 1.70f,
    7.80f, 16.70f, 1.20f, 0.80f, 5.50f, 0.05f, 1.60f, 0.05f,
};

// Percent of all bigrams, for the pairs that carry the mass. Everything absent
// backs off to BACKOFF * UNI[next], which is what makes "lk" small rather than
// impossible.
static const struct {
    const char *pair;
    float pct;
} BI[] = {
    {"th", 3.56f}, {"he", 3.07f}, {"in", 2.43f}, {"er", 2.05f}, {"an", 1.99f},
    {"re", 1.85f}, {"on", 1.76f}, {"at", 1.49f}, {"en", 1.45f}, {"nd", 1.35f},
    {"ti", 1.34f}, {"es", 1.34f}, {"or", 1.28f}, {"te", 1.20f}, {"of", 1.17f},
    {"ed", 1.17f}, {"is", 1.13f}, {"it", 1.12f}, {"al", 1.09f}, {"ar", 1.07f},
    {"st", 1.05f}, {"to", 1.04f}, {"nt", 1.04f}, {"ng", 0.95f}, {"se", 0.93f},
    {"ha", 0.93f}, {"as", 0.87f}, {"ou", 0.87f}, {"io", 0.83f}, {"le", 0.83f},
    {"ve", 0.83f}, {"co", 0.79f}, {"me", 0.79f}, {"de", 0.76f}, {"hi", 0.76f},
    {"ri", 0.73f}, {"ro", 0.73f}, {"ic", 0.70f}, {"ne", 0.69f}, {"ea", 0.69f},
    {"ra", 0.69f}, {"ce", 0.65f}, {"li", 0.62f}, {"ch", 0.60f}, {"ll", 0.58f},
    {"be", 0.58f}, {"ma", 0.57f}, {"si", 0.55f}, {"om", 0.55f}, {"ur", 0.54f},
    {"ca", 0.53f}, {"el", 0.53f}, {"ta", 0.53f}, {"la", 0.53f}, {"ns", 0.51f},
    {"di", 0.50f}, {"fo", 0.49f}, {"ho", 0.46f}, {"pe", 0.44f}, {"ec", 0.44f},
    {"pr", 0.43f}, {"no", 0.42f}, {"ct", 0.42f}, {"us", 0.42f}, {"ac", 0.41f},
    {"ot", 0.41f}, {"il", 0.40f}, {"tr", 0.39f}, {"ly", 0.38f}, {"nc", 0.37f},
    {"et", 0.37f}, {"ut", 0.36f}, {"ss", 0.35f}, {"so", 0.35f}, {"rs", 0.35f},
    {"un", 0.34f}, {"lo", 0.34f}, {"wa", 0.34f}, {"ge", 0.34f}, {"ie", 0.33f},
    {"wh", 0.33f}, {"ee", 0.33f}, {"wi", 0.32f}, {"em", 0.31f}, {"ad", 0.31f},
    {"ol", 0.31f}, {"rt", 0.30f}, {"po", 0.29f}, {"we", 0.29f}, {"na", 0.29f},
    {"ul", 0.29f}, {"ni", 0.28f}, {"ts", 0.28f}, {"mo", 0.27f}, {"ow", 0.27f},
    {"pa", 0.27f}, {"im", 0.27f}, {"mi", 0.26f}, {"ai", 0.26f}, {"sh", 0.26f},
    {"ir", 0.26f}, {"su", 0.25f}, {"id", 0.25f}, {"os", 0.25f}, {"iv", 0.24f},
    {"ia", 0.24f}, {"am", 0.24f}, {"fi", 0.24f}, {"ci", 0.23f}, {"vi", 0.23f},
    {"pl", 0.22f}, {"ig", 0.22f}, {"tu", 0.21f}, {"ev", 0.21f}, {"ld", 0.21f},
    {"ry", 0.21f}, {"mp", 0.21f}, {"fe", 0.21f}, {"bl", 0.21f}, {"ab", 0.21f},
    {"gh", 0.21f}, {"ty", 0.20f}, {"op", 0.20f}, {"wo", 0.20f}, {"sa", 0.20f},
    {"ay", 0.20f}, {"ex", 0.20f}, {"ki", 0.18f}, {"fr", 0.18f}, {"oo", 0.18f},
    {"av", 0.17f}, {"ag", 0.17f}, {"if", 0.17f}, {"ap", 0.16f}, {"gr", 0.16f},
    {"od", 0.16f}, {"bo", 0.16f}, {"sp", 0.16f}, {"rd", 0.16f}, {"do", 0.16f},
    {"uc", 0.15f}, {"bu", 0.15f}, {"ei", 0.15f}, {"ov", 0.15f}, {"by", 0.15f},
    {"rm", 0.15f}, {"ep", 0.14f}, {"tt", 0.14f}, {"oc", 0.14f}, {"fa", 0.14f},
    {"ef", 0.14f}, {"cu", 0.13f}, {"rn", 0.13f}, {"sc", 0.13f}, {"gi", 0.12f},
    {"da", 0.12f}, {"yo", 0.12f}, {"cr", 0.12f}, {"cl", 0.11f}, {"du", 0.11f},
    {"ga", 0.11f}, {"qu", 0.11f}, {"ue", 0.11f}, {"lu", 0.10f}, {"ys", 0.10f},
};
#define N_BI ((int)(sizeof(BI) / sizeof(BI[0])))

// How much of the unigram distribution leaks into a pair the table does not
// carry. Small enough that a listed pair beats an unlisted one by an order of
// magnitude, large enough that no letter is ever impossible.
#define BACKOFF 0.02f

// The table, expanded once. 676 floats is cheaper than scanning 170 strings for
// every one of the ~31 caps on every press.
static float ROW[26][26];
static bool built;

static void build(void) {
    if (built) {
        return;
    }
    for (int i = 0; i < N_BI; i++) {
        int a = BI[i].pair[0] - 'a';
        int b = BI[i].pair[1] - 'a';
        if (a >= 0 && a < 26 && b >= 0 && b < 26) {
            ROW[a][b] = BI[i].pct;
        }
    }
    built = true;
}

// The last LETTER before the cursor, lowercased, or 0 at a word boundary. A
// digit or a punctuation mark is a boundary too: "3x" and ".x" say nothing about
// the next letter that the word-initial distribution does not say better.
static char last_letter(const char *prefix) {
    if (!prefix || !prefix[0]) {
        return 0;
    }
    size_t n = strlen(prefix);
    unsigned char c = (unsigned char)prefix[n - 1];
    if (!isalpha(c)) {
        return 0;
    }
    return (char)tolower(c);
}

const char *z_lm_name(void) {
    return "bigram";
}

float z_lm_p(const char *prefix, char next) {
    if (next < 'a' || next > 'z') {
        return 0.0f;
    }
    build();
    int b = next - 'a';
    char last = last_letter(prefix);
    if (!last) {
        // Word boundary: the word-INITIAL distribution, not the overall one.
        float total = 0.0f;
        for (int i = 0; i < 26; i++) {
            total += INITIAL[i];
        }
        return total > 0.0f ? INITIAL[b] / total : 0.0f;
    }
    int a = last - 'a';
    float total = 0.0f;
    for (int i = 0; i < 26; i++) {
        total += ROW[a][i] + BACKOFF * UNI[i];
    }
    return total > 0.0f ? (ROW[a][b] + BACKOFF * UNI[b]) / total : 0.0f;
}
