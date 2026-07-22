// The letter-pair model, as the DICTIONARY's backoff (P48).
//
// This is not the seam. The seam is predict.h, it has one implementation, and
// that implementation is lm_trie.c. This header is the private interface between
// the two model files, and it exists because a prefix tree over a finite word
// list has one failure mode a keyboard cannot afford: OFF THE DICTIONARY, every
// continuation is probability zero and the model has nothing to say about a name,
// an abbreviation, a password or any of the several hundred thousand English
// words that did not make the cut.
//
// P47 shipped the letter-pair table as the whole model. It is the right answer to
// exactly that question — it has an opinion about every pair of letters in the
// language and never says "impossible" — so instead of deleting it, the trie
// falls back to it the moment the walk leaves the tree, and mixes a little of it
// in everywhere else so that a confident dictionary can still be overruled by a
// user who is spelling something it has never seen. That is the difference
// between a keyboard that is very good at the 1620 words it knows and one that is
// unusable for everything else.
//
// It is also what test_kbd_predict measures the trie AGAINST: the two are
// compiled into the same test and asked the same questions, which is the only way
// to state "the dictionary is sharper" as a number rather than a belief.
#ifndef ZELTO_KBD_LM_BACKOFF_H
#define ZELTO_KBD_LM_BACKOFF_H

// P(next | prefix) over the 26 LETTERS ONLY — it sums to 1 over the alphabet and
// has no notion of a word boundary, which is the limitation that motivated the
// dictionary. `next` must be lowercase a-z.
float z_lm_bigram_p(const char *prefix, char next);

// How often a word boundary follows an arbitrary letter in English, used as the
// backoff's answer for Z_LM_BOUNDARY. It is 1 / (mean word length + 1): English
// averages about 4.8 letters per word, so a little under one letter in six is
// followed by a space. A single number is the honest amount of detail here — the
// backoff is what runs when the model does not know the word, and a model that
// does not know the word does not know where it ends either.
#define Z_LM_BIGRAM_BOUNDARY_P 0.172f

#endif  // ZELTO_KBD_LM_BACKOFF_H
