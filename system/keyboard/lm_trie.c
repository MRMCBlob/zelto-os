// The language model, DICTIONARY edition (P48). The seam it implements is
// predict.h; the word list it reads is words_en.h; the letter-pair table it falls
// back to is lm_bigram.c, through lm_backoff.h.
//
// WHAT A PREFIX TREE BUYS THAT A LETTER-PAIR TABLE CANNOT. Three things, and each
// one is a test rather than a claim:
//
//   1. IT GETS SHARPER WITH A LONGER PREFIX. A bigram reads the last letter and
//      throws the rest away, so P(x | "e") and P(x | "hel") are the same
//      distribution to it. Here they are different nodes: at "hel" the surviving
//      words are hello / help / held / helping, so 'l' and 'p' carry almost all
//      the mass and 'q' carries none of it. That is the difference the classifier
//      turns into territory on the strip.
//
//   2. IT KNOWS WHERE A WORD ENDS. Every node carries the weight of the word that
//      ends exactly there (`wterm`) beside the weight of everything that
//      continues (`wsum`), so P(boundary | prefix) is a division and not a guess.
//      That is the adaptive SPACE BAR, which P47 recorded as the one thing its
//      model provably could not do.
//
//   3. IT CAN PROPOSE A WHOLE WORD. z_lm_candidates walks the list, not the tree,
//      because "what did they mean" is a question about entire words and not
//      about the next letter. It is what autocorrect and the suggestion strip are
//      built on, and it is deliberately a SEPARATE ENTRY POINT — see the note over
//      it in predict.h for why conflating the two mechanisms is the trap.
//
// AND THE ONE THING IT LOSES, which is why lm_bigram.c is still compiled in: off
// the dictionary it knows nothing at all. A trie over 1620 words assigns
// probability zero to every continuation of "zq", of a surname, of an
// abbreviation and of the ~99% of English that did not make the list. Zero is not
// "unlikely", it is "untypeable off-centre", so every answer here is MIXED with
// the letter-pair model (LM_MIX) and a walk that falls off the tree returns the
// letter-pair model outright.
//
// THE BUILD IS LAZY AND MEASURED. It runs on the first question asked, which on a
// real boot is the first press, and z_lm_report() prints what it cost. The word
// list ships in the image, so its size is a product fact; see item 1 of P48 and
// the "[keyboard] lm" line in any keyboard log.
#include "lm_backoff.h"
#include "predict.h"
#include "words_en.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The longest word the model will consider, and the longest prefix it will walk.
// The list's longest entry is 11 characters; the slack is for the PREFIX side,
// where somebody typing a long unhyphenated string should fall off the tree
// cheaply rather than walk a bounded loop that pretends to be looking.
#define LM_MAX_WORD 24

// Zipf. Word frequency in English is close to f(rank) ~ 1 / (rank + b) with b
// around 2.7 (the Zipf-Mandelbrot correction, which is what stops the first two
// or three ranks from being absurdly overweighted relative to a corpus). This is
// the ONLY place the word list's order becomes a number.
#define ZIPF_OFFSET 2.7f

// How much of the letter-pair model leaks into an answer the dictionary DID have.
// Not zero, deliberately: the dictionary is confident and finite, and a user
// spelling a word it does not carry has to be able to reach a letter the trie
// gives no mass to at all. 0.15 leaves the dictionary in charge of the decision
// while keeping every letter reachable, which is the same argument Z_KBD_ODDS
// makes one layer up and for the same reason.
#define LM_MIX 0.15f

// --- the word list, as offsets ----------------------------------------------
// The list is .rodata text in newline-separated bands (words_en.h). It is copied
// once into one heap buffer with the newlines turned into NULs, so that a word is
// a plain C string a caller can be handed — z_lm_candidates returns pointers into
// it, and returning a pointer into the middle of a '\n'-separated blob would hand
// out "hello\nhelp\nheld..." as a candidate.
//
// Offsets, not pointers: 1620 `const char *` is 13KB of relocations to describe
// 10KB of text. Two arrays of 3 bytes per word do it, and the rank is the index.
#define LM_MAX_WORDS 4096
static char *w_text;             // the flattened, NUL-separated copy
static size_t w_text_len, w_text_cap;
static uint16_t w_off[LM_MAX_WORDS];
static uint8_t w_len[LM_MAX_WORDS];
static uint16_t w_rank[LM_MAX_WORDS];   // the rank its weight comes from
static int n_words;
static int n_const;     // how many of them came from the shipped list
static int n_dups;      // entries dropped as duplicates — a lint, see below
#define N_BANDS ((int)(sizeof(WORDS_EN) / sizeof(WORDS_EN[0])))

// --- the learned words ------------------------------------------------------
// WHERE A LEARNED WORD MERGES, which is the question to answer before writing any
// of this (P49 item 2). It merges INTO THE LIST, before the tree is built — not
// into a second model consulted alongside the first. A learned word is therefore
// answered by the same z_lm_p, the same z_lm_is_word, the same z_lm_candidates as
// a shipped one, and there is no code path anywhere that can tell them apart or
// let them disagree. The cost of that choice is that learning REBUILDS the tree;
// see tree_build() and the deferral in trie_build().
//
// THE MODEL DOES NOT PERSIST ANYTHING AND MUST NOT. It is linked into a test
// binary with no storage, no permissions and no app; who is allowed to learn a
// word, where the list of them is kept, and what the user can do about it are
// decisions that live where the field's content purpose is visible — the keyboard
// (see kbd_learn in main.c). This side is a pure data structure with a doorbell.
//
// WHAT RANK A LEARNED WORD GETS. Not rank 0: a word the user typed three times is
// not commoner than "the", and a model that thought so would start correcting
// English into somebody's surname. Not the tail either, or learning it would
// change nothing. LEARN_RANK is the weight of a word around the middle of the
// shipped list — common enough to beat the tail it is competing with in a
// correction, far short of the function words at the head.
#define LM_LEARN_RANK 400
#define LM_MAX_LEARNED 512

static bool list_loaded;
static bool tree_dirty = true;
static unsigned lm_gen = 1;
static int n_rebuilds;

// --- the trie ---------------------------------------------------------------
// Children are a CONTIGUOUS RUN, sorted by character, not a 26-wide array. The
// 26-wide array is the obvious shape and it is 481KB for this list against 70KB
// for this one, for a lookup that scans at most 26 bytes of a cache line it has
// already pulled in. Measured both ways; the numbers are in z_lm_report().
typedef struct TNode {
    int32_t first_kid;
    float wsum;    // total weight of every word at or below this node
    float wterm;   // weight of the word that ends exactly HERE (0 if none)
    uint8_t n_kids;
    uint8_t ch;    // the character on the edge INTO this node ('\0' at the root)
} TNode;

static TNode *nodes;
static int32_t n_nodes, cap_nodes;
static double build_us;

static int32_t alloc_nodes(int32_t k) {
    if (n_nodes + k > cap_nodes) {
        int32_t want = cap_nodes ? cap_nodes * 2 : 1024;
        while (want < n_nodes + k) {
            want *= 2;
        }
        TNode *p = realloc(nodes, (size_t)want * sizeof(TNode));
        if (!p) {
            return -1;
        }
        nodes = p;
        cap_nodes = want;
    }
    int32_t base = n_nodes;
    memset(&nodes[base], 0, (size_t)k * sizeof(TNode));
    for (int32_t i = 0; i < k; i++) {
        nodes[base + i].first_kid = -1;
    }
    n_nodes += k;
    return base;
}

// The memoised walk (see walk() below). Declared here because a rebuild has to
// invalidate it, and a rebuild is the first thing in the file.
static char cache_word[LM_MAX_WORD];
static int32_t cache_node = -2;   // -2 = nothing cached; -1 = a real "off tree"

static const char *word_at(int i) {
    return &w_text[w_off[i]];
}
// The weight of entry `i`, from the RANK it carries rather than from where it
// happens to sit in the array — which is the same number for every shipped word
// and is not for a learned one (LM_LEARN_RANK).
static float rank_weight(int rank) {
    return 1.0f / ((float)rank + 1.0f + ZIPF_OFFSET);
}
static float entry_weight(int i) {
    return rank_weight(w_rank[i]);
}

// The build's sort order. Lexicographic, so that within any range sharing a
// prefix the word EQUAL to that prefix sorts first — which is what lets
// build_into() peel the terminal off the front without a search.
static int *order;
static int cmp_word(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcmp(word_at(ia), word_at(ib));
    if (c != 0) {
        return c;
    }
    return ia < ib ? -1 : (ia > ib ? 1 : 0);   // equal words: better rank first
}

static char word_char(int i, int depth) {
    return depth < w_len[i] ? word_at(i)[depth] : '\0';
}

// Fill node `me` from order[lo, hi), every one of which shares the first `depth`
// characters. Children of `me` are allocated as one run BEFORE any of them is
// descended into, which is what keeps first_kid/n_kids meaningful.
static void build_into(int32_t me, int lo, int hi, int depth) {
    float wsum = 0.0f;
    int i = lo;
    if (w_len[order[lo]] == (uint8_t)depth) {
        nodes[me].wterm = entry_weight(order[lo]);
        wsum += nodes[me].wterm;
        i++;
    }
    int starts[27], ends[27], nk = 0;
    while (i < hi && nk < 26) {
        char c = word_char(order[i], depth);
        int j = i;
        while (j < hi && word_char(order[j], depth) == c) {
            j++;
        }
        starts[nk] = i;
        ends[nk] = j;
        nk++;
        i = j;
    }
    if (nk > 0) {
        int32_t base = alloc_nodes(nk);
        if (base < 0) {
            nodes[me].wsum = wsum;
            return;
        }
        nodes[me].first_kid = base;
        nodes[me].n_kids = (uint8_t)nk;
        for (int k = 0; k < nk; k++) {
            nodes[base + k].ch = (uint8_t)word_char(order[starts[k]], depth);
        }
        for (int k = 0; k < nk; k++) {
            build_into(base + k, starts[k], ends[k], depth + 1);
            wsum += nodes[base + k].wsum;
        }
    }
    nodes[me].wsum = wsum;
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

// The shipped list, flattened and scanned. Runs once: it reads .rodata that
// cannot change, and a learned word is appended to what it produced rather than
// making it run again.
static void list_load(void) {
    if (list_loaded) {
        return;
    }
    list_loaded = true;   // set FIRST: a failed load must not be retried per press

    // Flatten the bands into one buffer, then scan it. Anything that is not a-z
    // is dropped rather than trusted — the list is hand-edited, and one stray
    // capital would otherwise become a node nothing can ever reach.
    size_t total = 0;
    for (int b = 0; b < N_BANDS; b++) {
        total += strlen(WORDS_EN[b]);
    }
    // Room for the learned words to be appended IN PLACE. Sized up front rather
    // than grown, because a learned word arrives during a keystroke and the whole
    // point of the offsets above is that no caller ever holds a pointer that a
    // reallocation could move — z_lm_candidates hands out `const char *` into this
    // buffer and predict.h promises they stay valid.
    w_text_cap = total + 1 + LM_MAX_LEARNED * (LM_MAX_WORD + 1);
    w_text = malloc(w_text_cap);
    if (!w_text) {
        return;
    }
    w_text_len = 0;
    for (int b = 0; b < N_BANDS; b++) {
        size_t len = strlen(WORDS_EN[b]);
        memcpy(w_text + w_text_len, WORDS_EN[b], len);
        w_text_len += len;
    }
    w_text[w_text_len] = '\0';

    size_t p = 0;
    while (p < w_text_len && n_words < LM_MAX_WORDS) {
        size_t q = p;
        while (q < w_text_len && w_text[q] != '\n') {
            q++;
        }
        size_t len = q - p;
        w_text[q] = '\0';   // the separator becomes the terminator
        bool ok = len > 0 && len < LM_MAX_WORD && p <= UINT16_MAX;
        for (size_t k = 0; ok && k < len; k++) {
            if (w_text[p + k] < 'a' || w_text[p + k] > 'z') {
                ok = false;
            }
        }
        if (ok) {
            w_off[n_words] = (uint16_t)p;
            w_len[n_words] = (uint8_t)len;
            w_rank[n_words] = (uint16_t)n_words;   // the list's order IS its rank
            n_words++;
        }
        p = q + 1;
    }
    n_const = n_words;
}

// The tree, from whatever is in the list now. Re-runnable, because a learned
// word changes the list and the whole point of merging one INTO the list is that
// there is exactly one tree answering every question afterwards.
static void tree_build(void) {
    double t0 = now_us();
    n_nodes = 0;        // the node array is reused; cap_nodes keeps the memory
    n_dups = 0;
    cache_node = -2;    // the memoised walk is about the OLD tree
    n_rebuilds++;
    if (n_words == 0) {
        build_us = now_us() - t0;
        return;
    }

    int *o = realloc(order, (size_t)n_words * sizeof(int));
    if (!o) {
        build_us = now_us() - t0;
        return;
    }
    order = o;
    for (int i = 0; i < n_words; i++) {
        order[i] = i;
    }
    qsort(order, (size_t)n_words, sizeof(int), cmp_word);

    // DUPLICATES ARE A LINT, NOT A CRASH. The list is a human-edited file with
    // thematic bands in it, so the same word can honestly be written twice; the
    // better rank wins and the count is reported, so that "the list grew and 20%
    // of it is now repeats" shows up as a number instead of as bytes nobody
    // notices. test_kbd_predict asserts it is zero.
    int k = 0;
    for (int i = 0; i < n_words; i++) {
        if (k > 0 && strcmp(word_at(order[i]), word_at(order[k - 1])) == 0) {
            n_dups++;
            continue;
        }
        order[k++] = order[i];
    }
    int n_uniq = k;

    int32_t root = alloc_nodes(1);
    if (root >= 0) {
        build_into(root, 0, n_uniq, 0);
    }
    build_us = now_us() - t0;
}

// The model, ready to answer. Cheap on every call but the first and the ones
// that follow a learned word — which is deliberate, and is what makes learning
// affordable: z_lm_learn only marks the tree DIRTY, so the rebuild lands on the
// next question rather than inside the keystroke that caused it. On the target
// that is the difference between a 55ms hitch between a press and its character,
// and 55ms between two frames.
static void trie_build(void) {
    list_load();
    if (!tree_dirty) {
        return;
    }
    tree_dirty = false;   // set FIRST: a failed build must not be retried per press
    tree_build();
}

// --- walking ----------------------------------------------------------------
static int32_t kid_of(int32_t me, char c) {
    const TNode *n = &nodes[me];
    for (int i = 0; i < n->n_kids; i++) {
        uint8_t kc = nodes[n->first_kid + i].ch;
        if (kc == (uint8_t)c) {
            return n->first_kid + i;
        }
        if (kc > (uint8_t)c) {
            break;   // children are sorted
        }
    }
    return -1;
}

// The WORD being typed: the run of letters immediately before the cursor,
// lowercased. Everything before the last non-letter is somebody else's word and
// the trie has no opinion about it — which is also why a digit or a punctuation
// mark resets the model to the word-initial distribution, exactly as the
// letter-pair table's own backoff does.
static int cur_word(const char *prefix, char *out) {
    out[0] = '\0';
    if (!prefix) {
        return 0;
    }
    size_t n = strlen(prefix);
    size_t i = n;
    while (i > 0 && isalpha((unsigned char)prefix[i - 1])) {
        i--;
    }
    size_t len = n - i;
    if (len >= LM_MAX_WORD) {
        i = n - (LM_MAX_WORD - 1);
        len = LM_MAX_WORD - 1;
    }
    for (size_t k = 0; k < len; k++) {
        out[k] = (char)tolower((unsigned char)prefix[i + k]);
    }
    out[len] = '\0';
    return (int)len;
}

// The node a word lands on, or -1 if it falls off the tree. MEMOISED on the word
// itself, because z_lm_p is asked about all 27 symbols for one press and the walk
// would otherwise be repeated 27 times for an answer that cannot have changed.
static int32_t walk(const char *w) {
    if (cache_node != -2 && strcmp(cache_word, w) == 0) {
        return cache_node;
    }
    int32_t at = n_nodes > 0 ? 0 : -1;
    for (const char *p = w; *p && at >= 0; p++) {
        at = kid_of(at, *p);
    }
    snprintf(cache_word, sizeof(cache_word), "%s", w);
    cache_node = at;
    return at;
}

const char *z_lm_name(void) {
    return "trie+bigram";
}

float z_lm_p(const char *prefix, char next) {
    bool boundary = (next == Z_LM_BOUNDARY);
    if (!boundary && (next < 'a' || next > 'z')) {
        return 0.0f;
    }
    trie_build();
    // The backoff's answer, on the 27-symbol scale the seam promises: the
    // letter-pair table sums to 1 over the ALPHABET, so its letters are scaled
    // down by the mass the boundary now carries.
    float bp = boundary
                   ? Z_LM_BIGRAM_BOUNDARY_P
                   : z_lm_bigram_p(prefix, next) * (1.0f - Z_LM_BIGRAM_BOUNDARY_P);

    char w[LM_MAX_WORD];
    cur_word(prefix, w);
    int32_t at = walk(w);
    if (at < 0 || nodes[at].wsum <= 0.0f) {
        return bp;   // off the dictionary: the letter-pair model is the model
    }
    float tp;
    if (boundary) {
        tp = nodes[at].wterm / nodes[at].wsum;
    } else {
        int32_t kid = kid_of(at, next);
        tp = kid < 0 ? 0.0f : nodes[kid].wsum / nodes[at].wsum;
    }
    return (1.0f - LM_MIX) * tp + LM_MIX * bp;
}

static bool exact_word(const char *w) {
    int32_t at = walk(w);
    return at >= 0 && nodes[at].wterm > 0.0f;
}

// --- regular inflections (P49) ----------------------------------------------
// THIS IS A BUG FIX, NOT A FEATURE. words_en.h records "no inflections" as a
// deliberate omission — "walk" is in the list, "walked" is not unless it earned
// its own rank — and reasoned that a stemmer would multiply the list by four for
// the same coverage. That reasoning was about COVERAGE and it was answering the
// wrong question. P49 measured what the omission actually does, on 60 hand-
// checked inflected forms of common words (meta/kbd-measure.sh, section 2):
//
//     REWRITTEN into another word   49 of 60 (82%)
//     no candidate, left as typed   11 of 60 (18%)
//     already in the list            0
//
// walked -> walk. asked -> ask. hands -> and. dogs -> does. reading -> wedding.
// taking -> thing. Autocorrect was not failing to help with inflected English,
// it was actively destroying it, and it had been doing so since P48 shipped —
// invisible because every test and every example in the phase used uninflected
// words. That is the exact shape of the bug this project keeps finding.
//
// WHY IT IS A RULE AND NOT 4000 MORE WORDS. Four times the list is four times the
// image, four times the trie and four times the hand-editing, to encode something
// English generates mechanically. The cost words_en.h was worried about — "the
// trie answering 'is this a word' in two places" — is avoided by putting the rule
// INSIDE z_lm_is_word rather than beside it: there is still exactly one function
// every caller asks, and it is this one.
//
// WHAT IT DELIBERATELY DOES NOT DO. It never invents a base: every candidate
// below is accepted only if the STRIPPED FORM IS ITSELF IN THE TRIE, so a typo
// cannot become a word by ending in "s". It over-generates in one direction —
// "thes" strips to "the" and is accepted — and P50 CONFIRMS that as closed rather
// than fixing it. The rule that would close it is "function words do not
// inflect", and there is no way to know which words those are without
// part-of-speech data: it would have to be a hand-written list of articles,
// prepositions, conjunctions and pronouns, i.e. a SECOND lexicon sitting beside
// words_en.h with its own way of going stale, to stop autocorrect fixing a
// handful of strings nobody types. The cost of the trade is one non-word
// autocorrect declines to repair; the benefit is 49 real words in 60 it stops
// destroying. (The OTHER stated residual, "stoping", is now closed — see
// needs_doubling.)
// Would this base DOUBLE its final consonant before -ing / -ed? A single-syllable
// consonant-vowel-consonant base always does in English — run/running,
// sit/sitting, get/getting — so "runing" is a typo and not an inflection, and
// accepting it would be the rule taking a word out of autocorrect's reach.
//
// Doubling is really about the STRESSED FINAL SYLLABLE, which nothing here can
// see directly: "open" ends C-V-C and does NOT double (opened, opening) because
// the stress is on the first syllable.
//
// P49 approximated that with "exactly three letters", which is single-syllable by
// construction and therefore exact — and stated the residual honestly: a
// four-letter base was left alone, so "stoping" was read as a word. P50 CLOSES
// that, because the syllable count is available after all: a base with exactly
// ONE VOWEL is one syllable however many consonants surround it. "stop", "shop",
// "plan", "chat", "grab", "trip", "swim" all double; "open", "enter", "offer",
// "visit" have two vowels and do not. The old rule is the new rule's three-letter
// case, so nothing that worked stops working.
//
// THE RESIDUAL MOVES RATHER THAN VANISHING, and it is worth naming precisely: a
// POLYSYLLABLE STRESSED ON ITS LAST SYLLABLE also doubles ("begin" ->
// "beginning", "admit" -> "admitting", "refer" -> "referring"), and two vowels is
// exactly what excludes it. So "begining" is still read as a word. That is a
// handful of verbs against every one-syllable verb in English, and closing it
// needs stress data this model does not have — which is a different thing from
// the four-letter gap, which needed only counting.
//
// 'w', 'x' and 'y' never double (fix/fixing, box/boxing, buy/buying, pay/paying),
// and are treated as vowels in the C-V-C test for exactly that reason. They are
// NOT counted as vowels for the syllable count, where they are not one.
static bool vowelish(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' ||
           c == 'y' || c == 'w' || c == 'x';
}
static bool is_vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}
static bool needs_doubling(const char *w, int len) {
    if (len < 3) {
        return false;
    }
    // The last three letters are consonant-vowel-consonant...
    if (vowelish(w[len - 3]) || !vowelish(w[len - 2]) || vowelish(w[len - 1])) {
        return false;
    }
    // ...and the whole base is one syllable.
    int vowels = 0;
    for (int i = 0; i < len; i++) {
        if (is_vowel(w[i])) {
            vowels++;
        }
    }
    return vowels == 1;
}

static bool inflected_word(const char *w) {
    int n = (int)strlen(w);
    char b[LM_MAX_WORD];
    if (n < 3 || n >= LM_MAX_WORD) {
        return false;
    }
    // Take the first `k` characters of w, optionally appending `add`.
    #define TRY(k, add)                                                        \
        do {                                                                   \
            int k_ = (k);                                                      \
            if (k_ >= 2 && k_ < LM_MAX_WORD - 1) {                             \
                memcpy(b, w, (size_t)k_);                                      \
                b[k_] = (add);                                                 \
                b[k_ + ((add) ? 1 : 0)] = '\0';                                \
                if (exact_word(b)) {                                           \
                    return true;                                               \
                }                                                              \
            }                                                                  \
        } while (0)

    if (w[n - 1] == 's') {
        TRY(n - 1, 0);                                   // walks -> walk
        if (n >= 4 && w[n - 2] == 'e') {
            TRY(n - 2, 0);                               // watches -> watch
            if (w[n - 3] == 'i') {
                TRY(n - 3, 'y');                         // carries -> carry
            }
        }
    }
    if (n >= 4 && w[n - 1] == 'd' && w[n - 2] == 'e') {
        if (!needs_doubling(w, n - 2)) {
            TRY(n - 2, 0);                               // walked -> walk
        }
        TRY(n - 1, 0);                                   // moved  -> move
        if (n >= 5 && w[n - 3] == 'i') {
            TRY(n - 3, 'y');                             // carried -> carry
        }
        if (n >= 6 && w[n - 3] == w[n - 4]) {
            TRY(n - 3, 0);                               // stopped -> stop
        }
    }
    if (n >= 5 && w[n - 1] == 'g' && w[n - 2] == 'n' && w[n - 3] == 'i') {
        if (!needs_doubling(w, n - 3)) {
            TRY(n - 3, 0);                               // walking -> walk
        }
        TRY(n - 3, 'e');                                 // moving  -> move
        if (n >= 6 && w[n - 4] == w[n - 5]) {
            TRY(n - 4, 0);                               // running -> run
        }
    }
    #undef TRY
    return false;
}

bool z_lm_is_word(const char *prefix) {
    trie_build();
    char w[LM_MAX_WORD];
    if (cur_word(prefix, w) == 0) {
        return false;
    }
    return exact_word(w) || inflected_word(w);
}

bool z_lm_is_prefix(const char *prefix) {
    trie_build();
    char w[LM_MAX_WORD];
    if (cur_word(prefix, w) == 0) {
        return false;
    }
    // On the tree at all: the walk did not fall off, so some word continues (or
    // ends at) this string. Length >= 2, the same floor autocorrect uses — every
    // single letter is trivially on the tree and none is worth protecting.
    return (int)strlen(w) >= 2 && walk(w) >= 0;
}

// --- learning ---------------------------------------------------------------
// The doorbell. Everything about WHETHER to ring it — how many times a word has
// to be typed, whether the field was a password, where the list is kept and how
// the user gets rid of it — is the keyboard's decision and lives in main.c. What
// happens here is a data-structure edit and a generation bump.
unsigned z_lm_generation(void) {
    return lm_gen;
}

// The contraction whose bare form is `lower`, or NULL. The table and the rule
// that governs what may be in it are in words_en.h; this is only the lookup, and
// it is on the MODEL side because a contraction is a fact about English — the
// same reason the word list is here and the substitution cost is not.
const char *z_lm_contraction(const char *lower) {
    if (!lower) {
        return NULL;
    }
    for (int i = 0; i < N_CONTRACTIONS_EN; i++) {
        if (strcmp(CONTRACTIONS_EN[i].bare, lower) == 0) {
            return CONTRACTIONS_EN[i].full;
        }
    }
    return NULL;
}

int z_lm_word_count(void) {
    list_load();
    return n_words;
}

bool z_lm_learn(const char *word) {
    if (!word) {
        return false;
    }
    int len = (int)strlen(word);
    if (len < 2 || len >= LM_MAX_WORD) {
        return false;
    }
    for (int i = 0; i < len; i++) {
        if (word[i] < 'a' || word[i] > 'z') {
            return false;   // the trie is a-z; anything else is not a word to it
        }
    }
    trie_build();   // the list has to exist before anything can be appended to it
    if (!w_text || n_words >= LM_MAX_WORDS ||
        n_words - n_const >= LM_MAX_LEARNED) {
        return false;
    }
    if (z_lm_is_word(word)) {
        return false;   // already known — shipped or already learned
    }
    if (w_text_len + (size_t)len + 1 > w_text_cap ||
        w_text_len + (size_t)len > UINT16_MAX) {
        return false;
    }
    memcpy(w_text + w_text_len, word, (size_t)len);
    w_off[n_words] = (uint16_t)w_text_len;
    w_len[n_words] = (uint8_t)len;
    w_rank[n_words] = LM_LEARN_RANK;
    w_text_len += (size_t)len;
    w_text[w_text_len++] = '\0';
    n_words++;
    // DIRTY, NOT REBUILT. The caller is inside a keystroke; the rebuild is 55ms
    // on the target (P48 measured it) and belongs between two frames, not between
    // a press and its character.
    tree_dirty = true;
    lm_gen++;
    return true;
}

int z_lm_learned(const char **out, int max) {
    list_load();
    int n = 0;
    for (int i = n_const; i < n_words && n < max; i++) {
        out[n++] = word_at(i);
    }
    return n;
}

void z_lm_forget_all(void) {
    list_load();
    if (n_words == n_const) {
        return;
    }
    // Truncate the text buffer back to the shipped list as well as the index —
    // the point of a forget is that the bytes are gone, not that they are
    // unreferenced. The shipped words all sit before n_const's offsets, so the
    // end of the last one is where the learned text began.
    size_t keep = 0;
    for (int i = 0; i < n_const; i++) {
        size_t end = (size_t)w_off[i] + w_len[i] + 1;
        if (end > keep) {
            keep = end;
        }
    }
    memset(w_text + keep, 0, w_text_len - keep);
    w_text_len = keep;
    n_words = n_const;
    tree_dirty = true;
    lm_gen++;
}

// --- whole-word candidates --------------------------------------------------
// Damerau-Levenshtein, with the SUBSTITUTION cost supplied by the caller. The
// three edit classes are not equally likely on a touch keyboard and pretending
// they are is what makes a naive autocorrect propose the wrong word: hitting the
// key next door is the commonest typo there is, and it is the one the classifier
// one layer up did NOT catch (a press deep inside the neighbour's centre zone,
// which is inviolable by design). Swapping two letters is the second commonest.
// Dropping or adding one is a third thing and costs full price.
#define EDIT_TRANSPOSE 0.8f

// How much one unit of edit distance costs a candidate, as a multiplier. 0.06
// means a word has to be about sixteen times commoner to be proposed over a
// near-neighbour one full edit away — which is roughly the gap between rank 100
// and rank 1600 in this list, i.e. the whole list is in play but only just.
#define EDIT_DECAY 0.06f

// An exact dictionary hit is not a correction and must never lose to one, however
// common the corrected word is. Stated as an explicit bonus rather than by
// arranging for the arithmetic to work out, because "it happens to win" is the
// kind of property that stops being true when the list changes.
#define EXACT_BONUS 1.0e6f

static float ROW_A[LM_MAX_WORD + 1], ROW_B[LM_MAX_WORD + 1],
    ROW_C[LM_MAX_WORD + 1];

// THE CALLER'S SUBSTITUTION COST, MEMOISED FOR THE LENGTH OF ONE CALL (P49).
//
// This is 95% of what a scan used to cost, measured rather than guessed: the same
// candidate search over the same list runs in 929us with the keyboard's callback
// and 51us with none (meta/kbd-measure.sh, section 1). The callback is not slow by accident — it
// is a fact about the LAYOUT, so it scans the laid-out caps for two characters
// and takes a square root, and the DP asks it once per mismatched cell. Over 1620
// words that is hundreds of thousands of identical questions, because there are
// only 26x26 of them.
//
// Memoised HERE and not in the caller, because the caller cannot know how many
// times the DP will ask: the interface promises "a fact about the layout", and a
// fact does not change between two cells of the same search. Lazy rather than
// precomputed — a 5-letter word touches at most 5 columns of the table, so
// filling all 676 entries eagerly would cost more than the search saves.
#define SUBST_UNKNOWN (-1.0f)
static ZLmSubstCost memo_fn;
static void *memo_ud;
static float subst_memo[26][26];

static void subst_begin(ZLmSubstCost subst, void *ud) {
    memo_fn = subst;
    memo_ud = ud;
    for (int i = 0; i < 26; i++) {
        for (int j = 0; j < 26; j++) {
            subst_memo[i][j] = SUBST_UNKNOWN;
        }
    }
}
static float subst_of(char want, char got) {
    if (!memo_fn) {
        return 1.0f;
    }
    if (want < 'a' || want > 'z' || got < 'a' || got > 'z') {
        return memo_fn(memo_ud, want, got);   // outside the table: ask every time
    }
    float *slot = &subst_memo[want - 'a'][got - 'a'];
    if (*slot == SUBST_UNKNOWN) {
        *slot = memo_fn(memo_ud, want, got);
    }
    return *slot;
}

static float edit_cost(const char *want, int wl, const char *got, int gl,
                       float ceiling) {
    // Two rolling rows plus the one before them (the transposition needs d[i-2]).
    float *prev2 = ROW_A, *prev = ROW_B, *cur = ROW_C;
    for (int j = 0; j <= gl; j++) {
        prev[j] = (float)j;
    }
    for (int i = 1; i <= wl; i++) {
        cur[0] = (float)i;
        float rowmin = cur[0];
        for (int j = 1; j <= gl; j++) {
            float sc = 0.0f;
            if (want[i - 1] != got[j - 1]) {
                sc = subst_of(want[i - 1], got[j - 1]);
            }
            float v = prev[j - 1] + sc;
            if (prev[j] + 1.0f < v) {
                v = prev[j] + 1.0f;
            }
            if (cur[j - 1] + 1.0f < v) {
                v = cur[j - 1] + 1.0f;
            }
            if (i > 1 && j > 1 && want[i - 1] == got[j - 2] &&
                want[i - 2] == got[j - 1] && prev2[j - 2] + EDIT_TRANSPOSE < v) {
                v = prev2[j - 2] + EDIT_TRANSPOSE;
            }
            cur[j] = v;
            if (v < rowmin) {
                rowmin = v;
            }
        }
        if (rowmin > ceiling) {
            return ceiling + 1.0f;   // no completion of this row can come back
        }
        float *t = prev2;
        prev2 = prev;
        prev = cur;
        cur = t;
    }
    return prev[gl];
}

int z_lm_candidates(const char *typed, ZLmSubstCost subst, void *ud,
                    ZLmWord *out, int max) {
    if (!typed || !out || max <= 0) {
        return 0;
    }
    trie_build();
    char got[LM_MAX_WORD];
    int gl = 0;
    for (const char *p = typed; *p && gl < LM_MAX_WORD - 1; p++) {
        if (*p < 'a' || *p > 'z') {
            return 0;   // not a plain lowercase word: not our question
        }
        got[gl++] = *p;
    }
    got[gl] = '\0';
    if (gl == 0) {
        return 0;
    }
    // A three-letter word is one edit from a dozen other three-letter words, so
    // the shorter the input the less licence a correction gets. Without this,
    // "cat" proposes "can", "car", "cut", "eat" and "hat" with nothing to choose
    // between them.
    float ceiling = gl <= 4 ? 1.0f : 2.0f;
    subst_begin(subst, ud);

    int n = 0;
    for (int i = 0; i < n_words; i++) {
        int wl = w_len[i];
        int dl = wl - gl;
        if (dl < 0) {
            dl = -dl;
        }
        if ((float)dl > ceiling) {
            continue;   // length alone rules it out, before any DP runs
        }
        float d = edit_cost(word_at(i), wl, got, gl, ceiling);
        if (d > ceiling) {
            continue;
        }
        float score = entry_weight(i) * (d <= 0.0f ? EXACT_BONUS
                                                 : powf(EDIT_DECAY, d));
        // Insertion sort into the top-`max`.
        if (n < max) {
            out[n].word = word_at(i);
            out[n].score = score;
            n++;
        } else if (score <= out[n - 1].score) {
            continue;
        } else {
            out[n - 1].word = word_at(i);
            out[n - 1].score = score;
        }
        for (int j = n - 1; j > 0 && out[j].score > out[j - 1].score; j--) {
            ZLmWord t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    }
    return n;
}

void z_lm_report(char *buf, size_t n) {
    trie_build();
    size_t list_bytes = w_text_len;
    size_t index_bytes = (size_t)n_words * (sizeof(w_off[0]) + sizeof(w_len[0]));
    size_t trie_bytes = (size_t)n_nodes * sizeof(TNode);
    snprintf(buf, n,
             "%s: %d words (%d shipped + %d learned, %d dropped as duplicates), "
             "%d nodes; list %zuB + index %zuB + trie %zuB = %zuB, built in "
             "%.0fus (build #%d; a 26-wide child array would make the trie %zuB)",
             z_lm_name(), n_words - n_dups, n_const, n_words - n_const, n_dups,
             (int)n_nodes, list_bytes, index_bytes, trie_bytes,
             list_bytes + index_bytes + trie_bytes, build_us, n_rebuilds,
             (size_t)n_nodes * (26 * sizeof(int32_t) + 2 * sizeof(float)));
}
