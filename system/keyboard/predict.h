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
// BIGRAM NOW, DICTIONARY LATER. Everything above the seam is geometry and does
// not care which is answering; everything below is a table. The difference is
// visible in what the keyboard can do, so the model SAYS which one it is: a
// letter-pair table cannot know the word "hello", only that 'l' often follows
// 'e'. A prefix tree over a shipped word list would drop in here without the hit
// path changing a line — and would additionally be able to grow the space bar
// once the prefix is a complete word, which a bigram table cannot know.
const char *z_lm_name(void);

// P(next | prefix), over the 26 letters (`next` is lowercase a-z). `prefix` is
// the text before the cursor and may be empty. Probabilities sum to ~1 over the
// alphabet, so a caller can compare against a uniform 1/26 without knowing how
// the model is built.
float z_lm_p(const char *prefix, char next);

// --- the classifier ---------------------------------------------------------
// One candidate key: where the LAYOUT put it, and what it yields. `ch` is the
// lowercase character a character cap commits, or 0 for a modifier — the
// language model has no opinion about shift, and a modifier therefore scores on
// geometry alone.
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
