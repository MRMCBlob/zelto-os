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
static size_t w_text_len;
static uint16_t w_off[LM_MAX_WORDS];
static uint8_t w_len[LM_MAX_WORDS];
static int n_words;
static int n_dups;      // entries dropped as duplicates — a lint, see below
#define N_BANDS ((int)(sizeof(WORDS_EN) / sizeof(WORDS_EN[0])))

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
static bool built;
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

static const char *word_at(int i) {
    return &w_text[w_off[i]];
}
static float rank_weight(int rank) {
    return 1.0f / ((float)rank + 1.0f + ZIPF_OFFSET);
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
        nodes[me].wterm = rank_weight(order[lo]);
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

static void trie_build(void) {
    if (built) {
        return;
    }
    built = true;   // set FIRST: a failed build must not be retried per press
    double t0 = now_us();

    // Flatten the bands into one buffer, then scan it. Anything that is not a-z
    // is dropped rather than trusted — the list is hand-edited, and one stray
    // capital would otherwise become a node nothing can ever reach.
    size_t total = 0;
    for (int b = 0; b < N_BANDS; b++) {
        total += strlen(WORDS_EN[b]);
    }
    w_text = malloc(total + 1);
    if (!w_text) {
        build_us = now_us() - t0;
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
            n_words++;
        }
        p = q + 1;
    }
    if (n_words == 0) {
        build_us = now_us() - t0;
        return;
    }

    order = malloc((size_t)n_words * sizeof(int));
    if (!order) {
        n_words = 0;
        build_us = now_us() - t0;
        return;
    }
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
static char cache_word[LM_MAX_WORD];
static int32_t cache_node = -2;   // -2 = nothing cached; -1 = a real "off tree"
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

bool z_lm_is_word(const char *prefix) {
    trie_build();
    char w[LM_MAX_WORD];
    if (cur_word(prefix, w) == 0) {
        return false;
    }
    int32_t at = walk(w);
    return at >= 0 && nodes[at].wterm > 0.0f;
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

static float edit_cost(const char *want, int wl, const char *got, int gl,
                       ZLmSubstCost subst, void *ud, float ceiling) {
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
                sc = subst ? subst(ud, want[i - 1], got[j - 1]) : 1.0f;
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
        float d = edit_cost(word_at(i), wl, got, gl, subst, ud, ceiling);
        if (d > ceiling) {
            continue;
        }
        float score = rank_weight(i) * (d <= 0.0f ? EXACT_BONUS
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
             "%s: %d words (%d dropped as duplicates), %d nodes; "
             "list %zuB + index %zuB + trie %zuB = %zuB, built in %.0fus "
             "(a 26-wide child array would make the trie %zuB)",
             z_lm_name(), n_words - n_dups, n_dups, (int)n_nodes, list_bytes,
             index_bytes, trie_bytes, list_bytes + index_bytes + trie_bytes,
             build_us,
             (size_t)n_nodes * (26 * sizeof(int32_t) + 2 * sizeof(float)));
}
