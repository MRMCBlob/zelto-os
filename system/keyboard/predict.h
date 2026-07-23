// Adaptive touch targets for the on-screen keyboard (P47).
//
// WHY THIS EXISTS. P46 measured the key caps for the first time: 31.5-32.1pt
// wide by 41.6pt tall, with an 11-unit gutter between them that belongs to no
// key. The gutter is the small half of the problem and 44pt is the wrong
// argument — a ten-key row on a 390pt phone cannot make a cap wider than 38.9pt
// with no gutter at all, and iOS's own caps are ~32pt. The big half is that a
// cap that resolves presses by RECTANGLE is the same size all the time, and a
// thumb is not that accurate. Apple's keyboard has never worked that way: the
// art is fixed and the invisible targets move on every keystroke, so after "hel"
// the region that yields 'l' is much larger than the region that yields 'k'
// beside it, without a pixel changing. That single mechanism is what makes a
// 32pt cap typeable.
//
// WHY IT IS NOT LITERAL RECTANGLES. "Grow the target" is the description, not
// the implementation: grown rectangles overlap, leave gaps and tie, and the code
// becomes an arbitration scheme. The implementation is a CLASSIFIER — for each
// candidate key,
//
//     score(key) = P(touch | key) * P(key | prefix)
//
// and the press means whichever key scores highest. P(touch | key) is a Gaussian
// on the distance from the key's centre, and the centre comes from the frame the
// layout already computed (never a number written down here). P(key | prefix) is
// the language model, behind the seam below. This is mathematically the same
// thing as resizing every target at once; it cannot leave a gap, it cannot
// produce an overlap, and it makes the dead gutter disappear as a side effect
// because every point on the strip is nearest SOMETHING.
//
// THE TWO THINGS IT MUST NOT DO:
//   - A press in the middle of a cap must give that cap's letter, always. If a
//     confident prefix could override the centre, the keyboard could not type a
//     password, a name, or any word the model has never seen. Enforced by
//     Z_KBD_CENTRE below, before any scoring happens.
//   - It must be off for password fields. The caller passes language=false when
//     the relayed content purpose is PASSWORD (see z_im_purpose).
#ifndef ZELTO_KBD_PREDICT_H
#define ZELTO_KBD_PREDICT_H

#include <stdbool.h>
#include <stddef.h>

// --- the language model seam ------------------------------------------------
// IT IS THE DICTIONARY NOW (P48). P47 wrote this seam as "bigram now, dictionary
// later, and put it behind an interface so the second does not touch the hit
// path". That happened: `lm_trie.c` is a prefix tree over the shipped word list
// (`words_en.h`) backed off to the letter-pair table (`lm_bigram.c`), and NOT ONE
// LINE above this comment changed to make it so. The model still says which one
// it is — z_lm_name() is "trie+bigram" — because the difference is visible in
// what the keyboard can do and a reader of a failing log should not have to
// guess which model produced it.
//
// WHAT THE SECOND MODEL CHANGES, and each one is a test:
//   - IT GETS SHARPER WITH A LONGER PREFIX. A bigram reads the last letter and
//     nothing else, so "e" and "hel" are the same question to it. The trie knows
//     that "hel" continues into "hello"/"help" and not into "helq".
//   - IT KNOWS WHERE A WORD ENDS, so the SPACE BAR is part of the same argmax
//     (see the boundary symbol below). A letter-pair table cannot answer that at
//     all, which P47 recorded as its known limitation.
//   - IT CAN PROPOSE A WHOLE WORD (z_lm_candidates), which is what autocorrect
//     and the suggestion strip are built from. That is a DIFFERENT MECHANISM from
//     the classifier below — see the note over z_lm_candidates.
const char *z_lm_name(void);

// The word boundary, as a symbol the model has an opinion about. `next` may be
// any lowercase letter OR this, and the two cases are the same question: "given
// what has been typed, how likely is this next?" The space bar therefore competes
// in the same argmax as the letters instead of being a modifier with no vote,
// which is the whole of the adaptive space bar.
#define Z_LM_BOUNDARY ' '

// How many symbols the model distributes over: 26 letters plus the boundary.
// This is the uniform baseline a caller compares against ("the model has no
// opinion" = 1/Z_LM_SYMBOLS), and it is 27 rather than 26 because the boundary
// takes real mass — after a complete word most of it.
#define Z_LM_SYMBOLS 27

// P(next | prefix). `next` is a lowercase letter or Z_LM_BOUNDARY; `prefix` is
// the text before the cursor and may be empty. Probabilities sum to ~1 over the
// 27 symbols, so a caller can compare against a uniform 1/Z_LM_SYMBOLS without
// knowing how the model is built.
float z_lm_p(const char *prefix, char next);

// Is the word being typed a COMPLETE WORD? Not the same question as "is it a
// word prefix", and the same trie answers both: "hel" is a prefix of "hello" and
// is not a word, "hell" is both, "hello" is a word whose only continuations are
// rare. The suggestion strip needs the first and the space bar needs the second.
bool z_lm_is_word(const char *prefix);

// Is the word being typed a LIVE PREFIX of some dictionary word — is the user
// part-way through spelling something the model knows? "hel" is (hello, help),
// "teh" is not. Autocorrect uses this to hold its fire: a string that is the
// start of a real word is not a typo to be replaced, even when it is not yet a
// word itself, or "hel" would "correct" to the commoner "he".
bool z_lm_is_prefix(const char *prefix);

// --- whole-word correction (a DIFFERENT mechanism) ---------------------------
// CONFLATING THESE TWO IS THE TRAP, so they are separated here by an interface
// and not only by a comment. The classifier below corrects a press BEFORE it
// commits, from the prefix alone; it is invisible, it cannot be wrong in a way
// the user can point at, and it needs no undo. What follows corrects text that is
// ALREADY IN THE FIELD, after a word boundary, from the whole word; it is
// visible, it IS sometimes wrong, and it is the single most complained-about
// behaviour on every phone. It does not ship without the suggestion strip and the
// backspace revert (see kbd_autocorrect in main.c).
typedef struct ZLmWord {
    const char *word;   // points into the model's own storage; valid until exit
    float score;        // higher is better; the ordering is the only contract
} ZLmWord;

// The cost of having meant `want` and pressed `got`, as a multiplier on a plain
// substitution. Supplied by the CALLER because it is a fact about the LAYOUT and
// not about English: 'k' for 'l' is a near-miss on a QWERTY grid and a wild one
// on a Dvorak grid, and the model must not contain a copy of the keyboard. Return
// 1.0 for "no idea" — that is what makes the callback optional.
typedef float (*ZLmSubstCost)(void *ud, char want, char got);

// Up to `max` dictionary words that `typed` might have been, best first. Returns
// how many were written. `typed` is lowercase a-z; anything else returns 0.
//
// An exact dictionary hit is returned as the first candidate with a score far
// above any correction, so a caller can tell "this is already a word" from "this
// is one edit from three words" by looking at the same list.
int z_lm_candidates(const char *typed, ZLmSubstCost subst, void *ud,
                    ZLmWord *out, int max);

// --- learning (P49) ----------------------------------------------------------
// THE KEYBOARD KNOWS 1620 WORDS AND NOT ONE OF THEM IS YOURS. That is the whole
// of what this adds, and the reason it is four small functions rather than a
// subsystem: a learned word MERGES INTO THE LIST the tree is built from, so from
// the moment it is learned it is answered by the same z_lm_p, the same
// z_lm_is_word and the same z_lm_candidates as a shipped word. There is no second
// model to disagree with the first, and no code above this line can tell which
// kind of word it just got an answer about.
//
// THIS SIDE MAKES NO POLICY. Who may teach it a word, how many times a string has
// to be typed before it counts, whether the field was a password, where the list
// is kept between boots and how a user deletes it are all decisions that need to
// see the field's content purpose and the OS's storage — so they live in the
// keyboard (kbd_learn / dict_load / dict_save in main.c, and the privacy note
// above them). What is here is a data structure and a doorbell.
//
// Learn `word` (lowercase a-z, 2..23 characters). Returns true if it was added —
// false if it is malformed, already known, or the store is full. It does NOT
// rebuild the tree; the rebuild is deferred to the next question so that the
// keystroke that taught the word does not pay 55ms for it on the target.
bool z_lm_learn(const char *word);

// Everything that has been learned, for the two callers that must be able to see
// it: the code that PERSISTS the list, and the code that shows the user what is
// in it. A store the user cannot read is a log; this is what stops it being one.
// Writes up to `max` pointers into the model's own storage and returns how many.
int z_lm_learned(const char **out, int max);

// Forget all of it, and blank the bytes rather than just the index. The delete
// half of the same decision — see Settings > Keyboard > Learned words.
void z_lm_forget_all(void);

// Bumped by every learn and every forget. A caller that MEMOISES an answer from
// this model (kbd_candidates in main.c does) needs to know the model underneath
// its cache has changed, and "the word is the same" is no longer enough.
unsigned z_lm_generation(void);

// The apostrophe form of a bare contraction ("dont" -> "don't"), or NULL. A trie
// over a-z cannot hold an apostrophe, so the list carries the bare forms and this
// is how they get their punctuation back. On this side of the seam because it is
// a fact about ENGLISH; whether to apply it, and how visibly, is the keyboard's
// call (see kbd_autocorrect).
const char *z_lm_contraction(const char *lower);

// How many words the model is answering from, shipped plus learned. The cost of
// a candidate scan is linear in this, so a caller reporting what a scan cost has
// to be able to say what it scanned.
int z_lm_word_count(void);

// What the model cost to build, for the log: entries, trie nodes, bytes of
// source list, bytes of built trie, and microseconds spent building it. Reported
// rather than estimated — the word list ships in the image, so its size is a
// product fact and not a detail. Builds the model if it is not built yet.
void z_lm_report(char *buf, size_t n);

// --- the classifier ---------------------------------------------------------
// One candidate key: where the LAYOUT put it, and what it yields. `ch` is the
// lowercase character a character cap commits, Z_LM_BOUNDARY for the SPACE BAR,
// or 0 for a modifier — the language model has no opinion about shift, and a
// modifier therefore scores on geometry alone.
//
// SPACE IS NOT A MODIFIER (P48). It was one, with lm_factor 1.0, for as long as
// there was no model that could know a word had ended. Now that there is, the
// space bar's target grows when what you have typed is a complete word and
// shrinks when it is a fragment — which is the one adaptive-target behaviour a
// user actually notices, and the one a bigram provably could not do.
typedef struct ZKbdKey {
    float x, y, w, h;
    char ch;
} ZKbdKey;

// Tuning, named so the test can state which one it is pinning.
//
// SIGMA is the Gaussian's width as a fraction of the CAP's own size, so it
// scales with the layout instead of being a number in units. 0.35 * width puts
// roughly one sigma at the cap's edge... 0.35 is not arbitrary: it is the value
// at which a confident bigram claims the dead gutter and the OUTER part of the
// neighbouring cap, and never reaches the neighbour's centre zone. Wider and the
// language model decides everything; narrower and it decides nothing, which is
// the rectangle keyboard again.
//
// CENTRE is the inviolable zone, as a fraction of the cap's HALF-extent: a press
// within 0.5 * (w/2) of a cap's centre is that cap, full stop.
//
// ODDS bounds how far the language model may move a decision, as a ratio against
// a uniform alphabet. Without it, a letter the model has literally never seen
// after this prefix scores zero and becomes untypeable off-centre.
//
// THE DICTIONARY DID NOT CHANGE ANY OF THESE THREE, and that is worth saying
// plainly because the obvious expectation was that it would. A sharper model
// pushes harder at the same clamp; it does not raise the clamp. The centre-zone
// guarantee is an inequality over SIGMA, ODDS and the key pitch alone — no term
// in it comes from the model — so a dictionary cannot break it, and
// test_kbd_predict still lints it unchanged. If somebody later widens SIGMA or
// raises ODDS *because* the dictionary is confident, that lint is what will say
// the centre rule has stopped being belt-and-braces.
#define Z_KBD_SIGMA 0.35f
#define Z_KBD_CENTRE 0.5f
#define Z_KBD_ODDS 8.0f

// Which key a press at (px, py) meant. Returns an index into `keys`, or -1 when
// there are none. `prefix` is the text before the cursor (may be NULL).
//
// `language` false = geometry only: the nearest key wins and nothing else has a
// vote. That is the password path AND the negative control (ZELTO_KBD_PREDICT=0),
// and it is deliberately NOT the same as "the old behaviour" — the old behaviour
// was rectangles, which resolve the gutter to nothing at all. Geometry-only still
// answers everywhere; what it drops is the model.
//
// `why` (may be NULL) receives a short human-readable trace for the log: which
// key won geometrically, which won overall, and why.
int z_kbd_classify(const ZKbdKey *keys, int n, float px, float py,
                   const char *prefix, bool language, char *why, size_t why_n);

// The geometrically nearest key alone, ignoring the model. Exposed because the
// audit needs it to answer "does any point on this strip hit nothing?" without
// the answer depending on what has been typed.
int z_kbd_nearest(const ZKbdKey *keys, int n, float px, float py);

#endif // ZELTO_KBD_PREDICT_H
