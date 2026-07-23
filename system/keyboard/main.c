// Zelto System UI — on-screen keyboard (zelto-keyboard, P21).
//
// A bottom-anchored TOP-layer layer-shell app, always running like the bars, but
// only *shown* while a text field is focused. It is the system's single
// input-method-v2 client (via z_im_bind): the compositor raises the "show"
// callback when any app's text field takes focus (the text-input/input-method
// handshake) and "hide" when it blurs. While shown it renders a tap-driven QWERTY
// grid; each key calls z_im_commit_text (insert) or z_im_backspace (delete), and
// the character lands in whatever field owns focus — the app never knows a
// keyboard exists.
//
// SHOW/HIDE. The surface is a fixed KBD_H strip at the bottom. Hidden, it slides
// itself off the bottom edge (a vertical Offset driven by an animated value,
// recomputed each frame — the drawer's trick), reserves NO exclusive zone and
// catches NO input (falls through to the app / nav bar). Shown, it reserves KBD_H
// (the compositor shrinks the app so the focused field stays visible above the
// keyboard — a real exclusive zone, no per-app work), catches input, and slides
// up. There is no "hide" key: the keyboard leaves when the field blurs.
//
// LAYERING. TOP layer (like the status/nav bars), created after the nav bar so it
// composites above it (a phone keyboard sits over the nav). The lock screen and
// the shade are OVERLAY, so they always composite ABOVE the keyboard: a locked
// screen shows its own passcode keypad, never the app's keyboard (and text-input
// focus is dropped under the lock anyway). See docs/platform/soft-keyboard.md.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#include "common/safe_areas.h"
#include "common/settings_defaults.h"
#include "predict.h"

// How close two shift presses have to be to mean CAPS LOCK. The same window
// every double-tap on a phone uses; long enough that a deliberate second press
// lands inside it, short enough that "shift, think, shift" does not.
#define KBD_DOUBLE_TAP_S 0.35

// How long a finger has to stay on a key before the press means "hold" — the
// accent popup on a letter, the repeating delete on backspace. Deliberately the
// same number for both, because a user learns ONE duration for "press and wait"
// and a key that needed a different one would feel broken rather than different.
#define KBD_HOLD_S 0.5

// The repeating delete, as a cadence rather than a count. Time, not repetitions,
// because a repeat that accelerated every N deletions would run at a different
// speed on a field with two characters left in it than on a full one.
#define KBD_REPEAT_S 0.15         // the first, comfortable cadence
#define KBD_REPEAT_FAST_S 0.06    // after KBD_REPEAT_ACCEL_S of holding
#define KBD_REPEAT_ACCEL_S 1.2
#define KBD_REPEAT_WORD_S 2.0     // after this, whole words at a time
#define KBD_REPEAT_WORD_GAP_S 0.25

// --- the learned dictionary, as constants (P49) -----------------------------
// The policy numbers. The mechanism is z_lm_learn (predict.h); everything about
// WHEN to ring that doorbell is here, and the reasoning is over kbd_learn().
//
// How many times a string the dictionary does not carry has to survive a word
// boundary uncorrected before it is a word of yours rather than a typo. A word
// typed ONCE is a typo. Three is the smallest number a slip cannot reach by
// accident — you would have to make the same misspelling three times — and is
// reached inside a single message for a word you actually use.
#define KBD_LEARN_SEEN 3
// How many candidate words are counted at once. Not a cache of what you typed:
// it is a fixed table of at most this many strings, and the least-seen entry is
// overwritten when a new candidate arrives. Small on purpose.
#define KBD_SEEN_SLOTS 32
#define KBD_LEARN_WORD_CAP 24
// The file the learned words live in, in the keyboard's own private directory
// under /var/zelto (P11). One word per line, exactly the shape of words_en.h,
// because a store the user cannot read is a log.
#define KBD_DICT_FILE "learned-words.txt"
// How many learned words are persisted. The model's own cap is the same order;
// this one is what bounds the FILE, so the store cannot grow without limit no
// matter how long the phone is used.
#define KBD_LEARN_MAX 512
// The brokered keys the learned dictionary is visible and deletable through are
// in common/settings_defaults.h, because Settings reads both of them too and a
// key two surfaces share must not be spelled in two places.

// Shift is three states, not a bool (P47). A one-shot shift and a caps lock are
// different keys wearing the same cap, and a phone tells them apart by how you
// press it: once for the next letter, twice quickly to lock.
typedef enum ShiftState {
    SHIFT_OFF,
    SHIFT_ONCE,   // the next letter, then back off
    SHIFT_LOCK,   // until pressed again
} ShiftState;

typedef struct KbdState {
    bool inited;
    bool visible;      // a text field is focused (input method active)
    ShiftState shift;  // what the SHIFT KEY is set to
    double last_shift_s;   // for the double-tap window
    // AUTO-CAPITALISATION is not a state of the shift key, it is a fact about the
    // text: at a sentence start the next letter is upper case whether or not
    // anyone pressed anything. Kept separately so that pressing shift can turn it
    // OFF (auto_off) rather than toggling it to doubly-on, and so that it is
    // recomputed — not remembered — every time the field's contents change.
    bool auto_shift;
    bool auto_off;         // the user overrode the auto-capital for this position
    char last_ctx[Z_TEXTFIELD_CAP];   // the context auto_shift was computed from
    // ...and whether last_ctx has ever been WRITTEN. Not redundant: the first
    // context an empty field reports is "", which compares equal to a zeroed
    // last_ctx, so a plain strcmp says "nothing changed" at exactly the moment
    // the answer matters most — the first letter of an empty field is the
    // commonest sentence start there is.
    bool ctx_seen;
    bool symbols;      // symbols/numbers layer instead of letters
    ZAnimated *anim;   // 0 = parked off the bottom, 1 = fully up
    // P45 KBD harness: type a string by itself, one character per tick, once a
    // field really is focused. `typed` is how far through ZELTO_KBD_TYPE we are.
    int typed;
    bool typing;
    // P46: the same idea one layer out — press the KEY CAPS instead of calling
    // the commit function. `tapped` is how far through ZELTO_KBD_TAP we are.
    int tapped;
    bool tapping;
    bool audited;

    // --- the live press (P47) ------------------------------------------------
    // A tap handler hears about a press once, when it is over. A keyboard has
    // three things to do while the finger is still down: show what it is about to
    // type, notice the press has become a HOLD, and follow the finger onto an
    // accent. All of that is driven from z_press_hook, and all of it lives here
    // because body() must be able to draw it without asking anything.
    bool down;
    float px, py;             // where the finger is now
    double down_s;            // when it landed
    bool hold_fired;          // the hold did something; the release is spent
    char hold_name[16];       // the cap it went down on
    // What the CLASSIFIER says the press means, refreshed on every move. This is
    // what the callout shows — see the note over kbd_callout for why it is that
    // and not the cap under the finger.
    bool preview;
    char preview_ch[8];
    char shown_ch[8];         // the last one logged, so the log is one per change
    float preview_x, preview_y, preview_w, preview_h;
    // The accent popup: open, whose, and the frame of the cap it belongs to.
    bool accents;
    char accent_base;
    float base_x, base_y, base_w, base_h;
    // The repeating delete.
    bool repeating;
    int repeats;
    double next_repeat_s;
    bool word_mode;
    // The harness's in-flight hold: where it went down, and what it slides onto.
    float hold_px, hold_py;
    char slide_to[16];

    // --- the suggestion strip and autocorrect (P48) --------------------------
    // The three slots, refreshed from the current word every build. slot[1] is
    // always the literal text the user typed; slot[0] and slot[2] are the two
    // words the dictionary thinks they meant, or empty. `slot_auto` marks which
    // slot autocorrect would apply on the next boundary, so the strip can show it
    // the way iOS bolds the pending correction.
    char slot[3][Z_TEXTFIELD_CAP];
    int slot_auto;                    // index of the pending autocorrection, or -1
    float slot_x[3], slot_w[3];       // laid-out frames, for the coordinate-free tap
    // REVERT. After autocorrect fires, backspace restores what was typed. The
    // corrected text is in the field (surrounding text can confirm it is still
    // there); the ORIGINAL is not, so the keyboard holds it here — for exactly one
    // key. Any key other than that first backspace clears it, the same one-edit
    // lifetime the double-space period has.
    bool revert_armed;
    char revert_from[Z_TEXTFIELD_CAP];   // what autocorrect put in the field
    char revert_to[Z_TEXTFIELD_CAP];     // what the user actually typed
    // ...and WHICH BOUNDARY followed it. P48 could hard-code a space here because
    // the space bar was the only boundary there was; now "teh." and a typo ended
    // with RETURN correct too, and a revert that went looking for "<word> " would
    // silently decline on both of them.
    char revert_bnd[8];

    // --- the learned dictionary (P49) ----------------------------------------
    // The counters that decide whether a word the shipped dictionary does not
    // carry is a typo or a word of yours. See the privacy note over kbd_learn():
    // these live in RAM and are NEVER written to disk, because a file of strings
    // somebody typed once is exactly the log a learned dictionary must not
    // become. Only a word that reaches the threshold is persisted.
    struct {
        char word[KBD_LEARN_WORD_CAP];
        int seen;
    } seen[KBD_SEEN_SLOTS];
    bool dict_loaded;
    int learned_n;          // how many words are in the model, for the log/UI
    int64_t clear_epoch;    // the sys.kbd_forget_learned we have already honoured
} KbdState;

// Commit the next character of ZELTO_KBD_TYPE, then re-arm until the string is
// done. See the note over the arming code in on_show().
static void type_tick(ZApp *app, void *ud);
// Press the next cap named by ZELTO_KBD_TAP. See the note over tap_tick().
static void tap_tick(ZApp *app, void *ud);
// The end of a held press (a "NAME~ms" token). See the note over hold_release().
static void hold_release(ZApp *app, void *ud);
// Tapping a suggestion-strip slot. See the note over on_suggest().
static void on_suggest(ZApp *app, void *state, void *data);
// Recompute the three strip slots and the pending autocorrection from the word
// being typed. See kbd_suggest().
static void kbd_suggest(ZApp *app, KbdState *s);
// The run of letters immediately before the cursor — the word being typed, in the
// case it was typed in. Returns its length. See the implementation.
static int kbd_cur_word(ZApp *app, char *out, size_t n);
// Would autocorrect change `word` on a boundary, and to what? Writes the
// case-matched replacement to `out` and returns true when it fires. The single
// source the strip and EVERY boundary consult, so they cannot disagree — which is
// the whole of P49 item 3: in P48 the space bar was the only boundary there was.
static bool kbd_autocorrect(ZApp *app, const char *word, char *out, size_t n);
// Forget the pending revert. Called by every key that is not the backspace that
// consumes it, because a revert is offered for exactly one keystroke.
static void kbd_clear_revert(KbdState *s);
// A word has ended and `commit` is the text that ended it. THE one boundary —
// see the note over kbd_boundary().
static void kbd_boundary(ZApp *app, KbdState *s, const char *commit);
// Count a word that survived a boundary uncorrected, and learn it once it has
// survived enough of them. See the privacy note over kbd_learn().
static void kbd_learn_seen(ZApp *app, KbdState *s, const char *word);
// Teach the model a word now, persist it, and republish the count.
static bool kbd_learn(ZApp *app, KbdState *s, const char *word);
// Read / write / delete the learned-word file. See dict_load().
static void dict_load(KbdState *s);
static void dict_save(void);
// Settings cleared the learned words. See on_setting().
static void on_setting(ZApp *app, const char *key, const char *value, void *ud);

// --- input-method show/hide (driven by the compositor) ---------------------
static void on_show(ZApp *app, void *ud) {
    KbdState *s = ud;
    s->visible = true;
    z_animated_spring(s->anim, 1.0f);
    // THE COORDINATE-FREE TYPING HOOK (P45). ZELTO_KBD_TYPE=<text> commits that
    // text through input-method-v2 exactly as a tapped key cap does, one
    // character at a time.
    //
    // It is armed HERE, off the real show handshake, not at startup: the
    // compositor raises the input method only when an app has actually focused a
    // text field, so committing before that would send the string into nothing.
    // That also makes the hook a stronger test than tapping key caps ever was —
    // it drives the genuine text-input-v3 <-> input-method-v2 relay, end to end,
    // with no key-cap coordinates to go stale. (Hardware keys cannot do this at
    // all: the SDK's kb_key handles Escape/Backspace and Return/space and never
    // inserts text, so a field is only ever typed into through this path.)
    //
    // AND IT IS NOT SUBSUMED BY ZELTO_KBD_TAP (P46), which was the open question.
    // KBD_TAP presses caps, so everything it produces has been through the
    // layout, the hit walk and — since P47 — the CLASSIFIER. That is the point of
    // it and it is also why it cannot replace this: when a test asks for exactly
    // the characters "hello" and gets "heklp", KBD_TAP cannot tell you whether the
    // relay is broken or the classifier is, because it exercises both. KBD_TYPE
    // exercises exactly one of them. It is the isolating half of a pair, kept
    // deliberately, and the QEMU KBD harness uses it for that reason: a typed note
    // surviving a reboot is a claim about persistence, and nothing about geometry
    // should be able to fail it.
    const char *want = getenv("ZELTO_KBD_TYPE");
    if (want && want[0] && !s->typing) {
        s->typing = true;
        z_after(app, 400, type_tick, s);
    }
    // THE KEY-CAP HOOK (P46). ZELTO_KBD_TAP="a,SHIFT,b" presses the caps
    // themselves. Armed off the same handshake and for the same reason; what it
    // adds over ZELTO_KBD_TYPE is everything BETWEEN a finger and the commit —
    // see tap_tick().
    const char *taps = getenv("ZELTO_KBD_TAP");
    if (!taps || !taps[0]) {
        taps = getenv("ZELTO_KBD_CAPS");   // the audit runs without a sequence
    }
    if (taps && taps[0] && !s->tapping) {
        s->tapping = true;
        // 50ms, not 400 (P47). The old delay was long enough for the show spring
        // to have finished, which meant the slide-settled gate in tap_tick was
        // never reached — P46 removed the gate and no test failed. Arming almost
        // immediately makes the gate the thing that waits, so it runs on every
        // boot of every keyboard test instead of being a guard nobody exercises.
        z_after(app, 50, tap_tick, s);
    }
    z_invalidate(app);
}

static void type_tick(ZApp *app, void *ud) {
    KbdState *s = ud;
    const char *want = getenv("ZELTO_KBD_TYPE");
    if (!want || s->typed >= (int)strlen(want)) {
        return;
    }
    char one[2] = {want[s->typed], '\0'};
    s->typed++;
    z_im_commit_text(app, one);
    fprintf(stderr, "[keyboard] committed '%s' (%d/%d)\n", one, s->typed,
            (int)strlen(want));
    fflush(stderr);
    if (s->typed < (int)strlen(want)) {
        z_after(app, 300, type_tick, s);
    }
}
static void on_hide(ZApp *app, void *ud) {
    KbdState *s = ud;
    s->visible = false;
    s->shift = SHIFT_OFF;
    s->auto_shift = false;
    s->auto_off = false;
    s->last_ctx[0] = '\0';
    s->ctx_seen = false;
    s->symbols = false;
    z_animated_spring(s->anim, 0.0f);
    z_invalidate(app);
}

// --- what the field says ----------------------------------------------------
// THE CONTEXT IS THE FIELD'S, NOT OURS. Everything below — the prediction prefix,
// the double-space period, auto-capitalisation — is a rule about the text BEFORE
// THE CURSOR, and the keyboard cannot see the field. It reads text-input-v3's
// surrounding text, relayed by the compositor since P21 into an SDK stub that
// dropped it until P47. An echo of the keyboard's own keystrokes would have been
// the obvious alternative and would be wrong the moment anything else touched the
// field: a paste, a caret move, an app clearing the buffer.
//
// The one thing it costs is LATENCY. The app re-declares its surrounding text on
// the build after a commit, so two presses closer together than a frame round
// trip see the same context. That is a real limitation of doing it this way and
// the reason it is done this way anyway: the alternative is a second copy of the
// text that is right until it is not.
static const char *kbd_prefix(ZApp *app) {
    int cursor = 0;
    const char *s = z_im_surrounding(app, &cursor);
    static char buf[Z_TEXTFIELD_CAP];
    int n = cursor;
    if (n < 0) { n = 0; }
    if (n > (int)sizeof(buf) - 1) { n = (int)sizeof(buf) - 1; }
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return buf;
}

// Is the language machinery allowed to run at all? See predict.h; a password is
// exactly the string these rules get wrong.
static bool predict_on(ZApp *app) {
    const char *e = getenv("ZELTO_KBD_PREDICT");
    if (e && e[0] == '0') {
        return false;   // the negative control: geometry, no model
    }
    return z_im_purpose(app) != Z_IM_PURPOSE_PASSWORD;
}

// Does the text before the cursor START A SENTENCE? Empty field, or a terminator
// followed by space(s), or a fresh line. Only asked when the FIELD declared that
// it wants sentence case (content hint AUTO_CAPITALIZATION) — a note does, a
// username does not, and nothing the keyboard can see tells them apart.
static bool sentence_start(const char *p) {
    int n = (int)strlen(p);
    if (n == 0) {
        return true;
    }
    if (p[n - 1] == '\n') {
        return true;
    }
    int i = n - 1;
    while (i >= 0 && p[i] == ' ') {
        i--;
    }
    if (i == n - 1) {
        return false;   // mid-word: no space between us and the last letter
    }
    return i >= 0 && (p[i] == '.' || p[i] == '?' || p[i] == '!');
}

// The one question every character key asks: upper or lower?
static bool kbd_upper(const KbdState *s) {
    if (s->shift == SHIFT_LOCK) {
        return true;
    }
    if (s->shift == SHIFT_ONCE) {
        return true;
    }
    return s->auto_shift && !s->auto_off;
}

// DOES THIS CHARACTER END A WORD? (P49)
//
// The rule is not a list of punctuation, it is the SAME rule kbd_cur_word already
// applies from the other side: a word is the run of letters before the cursor, so
// anything that is not a letter is where the word ended. Writing it as a
// predicate over that fact rather than as a table of ".,?!" is what stops the two
// from drifting apart — a symbol added to the layer becomes a boundary
// automatically, and a boundary that kbd_cur_word would not stop at cannot exist.
//
// The two exceptions, each of which is a decision:
//   - A DIGIT does not end a word. kbd_cur_word does stop at one, so "a2" is two
//     tokens to the model either way; what a digit tells us that a full stop does
//     not is that this is not prose ("h2", "b12", a part number), and correcting
//     the letters in front of one is how autocorrect earns its reputation.
//   - THE APOSTROPHE does not end a word. It is the one punctuation mark that
//     lives INSIDE English words, and it is the mark the expansion table below
//     puts back — a keyboard that treated it as a boundary would correct "dont"
//     the moment you tried to type "don't" by hand.
static bool boundary_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return false;
    }
    if (c >= '0' && c <= '9') {
        return false;
    }
    return c != '\'';
}

// --- key handlers -----------------------------------------------------------
// A character key: commit the (shift-cased) byte, then spend the one-shot shift.
// A character that ENDS A WORD goes through kbd_boundary instead of straight to
// the field — which is the whole of P49 item 3: "teh." used to commit
// uncorrected because only the space bar knew what a boundary was.
static void on_char(ZApp *app, void *state, void *data) {
    KbdState *s = state;
    int cp = (int)(intptr_t)data;
    char buf[2] = {(char)cp, '\0'};
    if (kbd_upper(s) && cp >= 'a' && cp <= 'z') {
        buf[0] = (char)(cp - 32);
    }
    if (boundary_char(buf[0])) {
        kbd_boundary(app, s, buf);
    } else {
        kbd_clear_revert(s);   // any letter ends the one-key window a revert lives in
        z_im_commit_text(app, buf);
        fprintf(stderr, "[keyboard] commit '%s'\n", buf);
        fflush(stderr);
    }
    if (s->shift == SHIFT_ONCE) {
        s->shift = SHIFT_OFF;   // spent. A LOCK is not.
        z_invalidate(app);
    }
}
static void on_shift(ZApp *app, void *state) {
    KbdState *s = state;
    kbd_clear_revert(s);
    double now = z_now_seconds();
    bool dbl = (now - s->last_shift_s) < KBD_DOUBLE_TAP_S;
    s->last_shift_s = now;
    if (s->shift == SHIFT_LOCK) {
        s->shift = SHIFT_OFF;      // a locked shift unlocks on the next press
        s->auto_off = true;
    } else if (dbl) {
        s->shift = SHIFT_LOCK;     // two presses inside the window
        s->auto_off = false;
    } else if (kbd_upper(s)) {
        // It was on — possibly because auto-capitalisation put it on, which is
        // why this is not a plain toggle. Turning it off has to say so, or the
        // next build recomputes auto_shift and turns it back on.
        s->shift = SHIFT_OFF;
        s->auto_off = true;
    } else {
        s->shift = SHIFT_ONCE;
        s->auto_off = false;
    }
    fprintf(stderr, "[keyboard] shift %s%s\n",
            s->shift == SHIFT_LOCK ? "LOCK"
                                   : (s->shift == SHIFT_ONCE ? "once" : "off"),
            s->auto_off ? " (auto overridden)" : "");
    fflush(stderr);
    z_invalidate(app);
}
static void on_symbols(ZApp *app, void *state) {
    KbdState *s = state;
    kbd_clear_revert(s);
    s->symbols = !s->symbols;
    s->shift = SHIFT_OFF;
    z_invalidate(app);
}
// A WORD BOUNDARY, WHICHEVER KEY SPELLED IT (P49 item 3).
//
// P48 put autocorrect on the SPACE BAR, and a space is not a word boundary — it
// is one of four spellings of one. "teh." "teh!" "teh?" and a typo finished with
// RETURN all committed uncorrected, and the strip that had been showing the
// pending correction just vanished as the word scrolled out from under the
// cursor. So every key that ends a word arrives HERE with the text it is about
// to commit, and this function owns what happens in what order.
//
// THE ORDER MATTERS, and it is the reason this is one function and not a helper
// three handlers call:
//   1. THE DOUBLE-SPACE PERIOD FIRST, because it REWRITES the text a correction
//      would have read. Two spaces in a row become ". " — the rule every phone
//      has had for fifteen years, and the reason nobody reaches for the symbols
//      layer to end a sentence. It is a rule about the TEXT, answered from the
//      field's surrounding text: if what is there is a word followed by one
//      space, this second space replaces that space with a full stop. A double
//      space ends a word that has ALREADY been spaced, so there is no word under
//      the cursor to correct and this path never reaches step 2.
//   2. THEN AUTOCORRECT, on the finished word, from the whole of it. The
//      classifier corrected this word's PRESSES as they were typed, from the
//      prefix; this is a different mechanism with a different failure mode — it
//      is visible and it is sometimes wrong, so it does not happen without the
//      strip that shows it and the backspace that undoes it.
//   3. THEN LEARNING, on the word that came through uncorrected, because a word
//      autocorrect just replaced is not evidence of anything.
//
// Off in a password field for the same reason prediction is: a password may end
// in a space, and a keyboard that turned it into a full stop would be unfixable
// from the app's side.
static void kbd_boundary(ZApp *app, KbdState *s, const char *commit) {
    const char *p = kbd_prefix(app);
    int n = (int)strlen(p);
    bool period = commit[0] == ' ' && commit[1] == '\0' && predict_on(app) &&
                  n >= 2 && p[n - 1] == ' ' &&
                  ((p[n - 2] >= 'a' && p[n - 2] <= 'z') ||
                   (p[n - 2] >= 'A' && p[n - 2] <= 'Z') ||
                   (p[n - 2] >= '0' && p[n - 2] <= '9'));
    if (period) {
        kbd_clear_revert(s);
        z_im_backspace(app);
        z_im_commit_text(app, ". ");
        fprintf(stderr, "[keyboard] double-space period after '%s'\n", p);
        fflush(stderr);
        return;
    }
    char word[Z_TEXTFIELD_CAP], corrected[Z_TEXTFIELD_CAP];
    kbd_clear_revert(s);
    int wl = kbd_cur_word(app, word, sizeof(word));
    if (wl > 0 && kbd_autocorrect(app, word, corrected, sizeof(corrected))) {
        // Replace the typed letters with the correction as ONE delete + commit,
        // so the field sees a single edit — then the boundary. The undo is armed
        // on the corrected word, which is what surrounding text will confirm is
        // still there when the next backspace asks to take it back.
        z_im_delete(app, wl);
        z_im_commit_text(app, corrected);
        z_im_commit_text(app, commit);
        s->revert_armed = true;
        snprintf(s->revert_from, sizeof(s->revert_from), "%s", corrected);
        snprintf(s->revert_to, sizeof(s->revert_to), "%s", word);
        snprintf(s->revert_bnd, sizeof(s->revert_bnd), "%s", commit);
        fprintf(stderr,
                "[keyboard] autocorrect '%s' -> '%s' at boundary '%s' "
                "(backspace reverts)\n",
                word, corrected, commit[0] == '\n' ? "\\n" : commit);
        fflush(stderr);
        return;
    }
    z_im_commit_text(app, commit);
    fprintf(stderr, "[keyboard] commit '%s' (boundary)\n",
            commit[0] == '\n' ? "\\n" : commit);
    fflush(stderr);
    if (wl > 0) {
        kbd_learn_seen(app, s, word);
    }
}
static void on_space(ZApp *app, void *state) {
    kbd_boundary(app, state, " ");
}
// BACKSPACE, and the AUTOCORRECT REVERT.
//
// A backspace immediately after an autocorrection puts back what you typed. That
// is a claim about state the keyboard does not obviously have — the word you
// typed is GONE from the field, autocorrect replaced it — so the answer, worked
// out the same way the double-space period's was: the field holds the CORRECTED
// word (and surrounding text can confirm it is still there, untouched by a paste
// or a caret move), and the keyboard holds the ORIGINAL, for exactly one
// keystroke. If anything has disturbed the tail the correction left, this is an
// ordinary backspace instead — never a surprise edit somewhere the cursor no
// longer is.
static void on_backspace(ZApp *app, void *state) {
    KbdState *s = state;
    if (s->revert_armed) {
        s->revert_armed = false;
        const char *p = kbd_prefix(app);
        int n = (int)strlen(p);
        int fl = (int)strlen(s->revert_from);
        int bl = (int)strlen(s->revert_bnd);
        // The correction committed "<from><boundary>" — the word and whichever
        // character ended it, which is a space, a full stop or a newline. Only
        // revert if exactly that is still before the cursor.
        if (bl > 0 && n >= fl + bl &&
            strncmp(p + n - bl, s->revert_bnd, (size_t)bl) == 0 &&
            strncmp(p + n - fl - bl, s->revert_from, (size_t)fl) == 0) {
            z_im_delete(app, fl + bl);
            z_im_commit_text(app, s->revert_to);
            fprintf(stderr, "[keyboard] autocorrect reverted '%s' -> '%s'\n",
                    s->revert_from, s->revert_to);
            fflush(stderr);
            // A REVERT IS THE STRONGEST SIGNAL THERE IS, so it does not wait for
            // the counter (P49). Repetition is a guess that a string was not a
            // typo; a revert is a person saying "no, I meant this" about a word
            // the dictionary already offered its opinion on. Learning it here is
            // also what stops the same correction happening again on the next
            // sentence, which is the behaviour that makes a phone keyboard feel
            // like it is arguing with you.
            kbd_learn(app, s, s->revert_to);
            return;
        }
        fprintf(stderr,
                "[keyboard] revert declined: '%s%s' is no longer under the "
                "cursor (prefix '%s')\n",
                s->revert_from,
                s->revert_bnd[0] == '\n' ? "\\n" : s->revert_bnd, p);
    }
    z_im_backspace(app);
    fprintf(stderr, "[keyboard] backspace\n");
    fflush(stderr);
}

// --- accents ----------------------------------------------------------------
// The alternates a letter offers when you hold it. A phone keyboard has no room
// for a dead-key layer and no room for a second alphabet, so the accents live
// UNDER the letters they belong to and appear only while a finger is on one.
//
// The strings are static and their POINTERS are the identity the popup's caps
// carry as tap data — which is what lets the probe resolve "the é key" without a
// coordinate, exactly as a character cap is resolved by its codepoint.
static const struct AccentRow {
    char base;
    const char *alt[8];
    int n;
} ACCENTS[] = {
    {'a', {"à", "á", "â", "ä", "ã", "å"}, 6},
    {'c', {"ç"}, 1},
    {'e', {"è", "é", "ê", "ë"}, 4},
    {'i', {"ì", "í", "î", "ï"}, 4},
    {'n', {"ñ"}, 1},
    {'o', {"ò", "ó", "ô", "ö", "õ"}, 5},
    {'s', {"ß"}, 1},
    {'u', {"ù", "ú", "û", "ü"}, 4},
    {'y', {"ÿ"}, 1},
};
#define N_ACCENTS ((int)(sizeof(ACCENTS) / sizeof(ACCENTS[0])))

static const struct AccentRow *accents_for(char base) {
    for (int i = 0; i < N_ACCENTS; i++) {
        if (ACCENTS[i].base == base) {
            return &ACCENTS[i];
        }
    }
    return NULL;
}

// The upper case of a Latin-1 accent (P48). The alternates are stored lower case
// and this is the ONE place the case is applied — at commit and in the popup —
// because the accent's IDENTITY the popup and the slide resolve by is its stored
// pointer, which must not change when shift is held. Almost all of them are U+00Ex
// -> U+00Cx, i.e. the low byte of the two-byte UTF-8 form drops by 0x20; ÿ and ß
// are the two that do not follow the rule and are handled by name. Writes `out`
// (>= 4 bytes) and returns it.
//
// WHY THIS EXISTS NOW. P47 shipped lower-case only and wrote down why holding
// shift then 'e' giving a lower-case è "reads as a bug" — and it does. The letters
// that most need the capital are the ones a name STARTS with (À, Ö), and a name is
// exactly what a dictionary cannot help with, so the fix belongs in the table and
// not in autocorrect. It is a rule, not a second table: 26 entries would be 26
// more things to keep in sync with the lower-case row.
static const char *accent_upper(const char *acc, char *out) {
    unsigned char c0 = (unsigned char)acc[0], c1 = (unsigned char)acc[1];
    if (c0 == 0xC3 && c1 == 0xBF) {   // ÿ -> Ÿ (U+0178), not in the C3 block
        out[0] = (char)0xC5; out[1] = (char)0xB8; out[2] = '\0';
        return out;
    }
    if (c0 == 0xC3 && c1 == 0x9F) {   // ß has no single upper case: leave it
        out[0] = (char)0xC3; out[1] = (char)0x9F; out[2] = '\0';
        return out;
    }
    if (c0 == 0xC3 && c1 >= 0xA0 && c1 <= 0xBE) {
        out[0] = (char)0xC3; out[1] = (char)(c1 - 0x20); out[2] = '\0';
        return out;
    }
    snprintf(out, 4, "%s", acc);   // not an accent we case: pass through
    return out;
}

// Committing an accent closes the popup, and spends a one-shot shift the way any
// other letter would. Upper-cased when the keyboard is shifted, so "hold shift,
// hold e" gives É — the case a name needs and a dictionary cannot supply.
static void on_accent(ZApp *app, void *state, void *data) {
    KbdState *s = state;
    kbd_clear_revert(s);
    const char *acc = data;
    char up[4];
    if (kbd_upper(s)) {
        acc = accent_upper(acc, up);
    }
    z_im_commit_text(app, acc);
    fprintf(stderr, "[keyboard] commit '%s' (accent of '%c')\n", acc,
            s->accent_base);
    fflush(stderr);
    s->accents = false;
    if (s->shift == SHIFT_ONCE) {
        s->shift = SHIFT_OFF;
    }
    z_invalidate(app);
}
// RETURN ends a word too, which P48's on_space-only autocorrect did not know: a
// typo at the end of a line was the one typo a phone keyboard never fixed.
static void on_enter(ZApp *app, void *state) {
    kbd_boundary(app, state, "\n");
}

// --- the key caps, driven as key caps (P46) ---------------------------------
//
// WHAT THIS COVERS THAT ZELTO_KBD_TYPE DOES NOT. The P45 hook calls
// z_im_commit_text() from inside this process, which proves the text-input-v3 <->
// input-method-v2 relay and the persistence behind it, and proves nothing
// whatsoever about the keyboard as a SURFACE. Everything between a finger and
// that call was untested: whether a cap is laid out where the grid says, whether
// pressing it reaches that cap rather than a neighbour or the material behind it,
// whether the mark on a modifier describes what the modifier does, and whether
// the caps are big enough to hit.
//
// WHY THIS IS NOT THE COORDINATE TAPPING THAT ROTTED FIVE HARNESSES. Those
// harnesses carried the numbers: a screenshot was measured, "the 'a' key is at
// (96, 1180)" went into a shell script, the layout moved, and the tap kept
// landing — on something else, silently. Here the test names a CHARACTER and the
// layout answers where it is (z_probe_tap, sdk/src/app.c), so the only way for
// this to drift is for the code that builds the row to change, which is the thing
// under test. There is not a single coordinate in the harness or in this file.
//
// A name is one character ('a', '5', '?') or one of these words.
static struct {
    const char *name;
    ZAction act;
} KEY_WORDS[] = {
    {"SHIFT", on_shift},
    {"BKSP", on_backspace},
    {"SPACE", on_space},
    {"ENTER", on_enter},
    {"SYM", on_symbols},
};
#define N_KEY_WORDS ((int)(sizeof(KEY_WORDS) / sizeof(KEY_WORDS[0])))

// Name -> the handler the cap carries. A character key is an OnTapData node
// keyed by the LOWERCASE codepoint (char_key binds `c`, not the shifted glyph),
// which is exactly why tapping 'a' with shift latched must be asked for as 'a'.
static bool key_handler(const char *name, ZTapAction *on_data, void **data,
                        ZAction *on_plain) {
    *on_data = NULL;
    *data = NULL;
    *on_plain = NULL;
    for (int i = 0; i < N_KEY_WORDS; i++) {
        if (strcmp(name, KEY_WORDS[i].name) == 0) {
            *on_plain = KEY_WORDS[i].act;
            return true;
        }
    }
    // A suggestion-strip slot, named SUG0/SUG1/SUG2, so a coordinate-free tap can
    // reach it (the strip is new UI and its slots carry a word, not a key name).
    if (name[0] == 'S' && name[1] == 'U' && name[2] == 'G' && name[3] >= '0' &&
        name[3] <= '2' && !name[4]) {
        *on_data = on_suggest;
        *data = (void *)(intptr_t)(name[3] - '0');
        return true;
    }
    if (name[0] && !name[1]) {
        *on_data = on_char;
        *data = (void *)(intptr_t)name[0];
        return true;
    }
    return false;
}

// The reverse, for the audit: what is this cap called?
static void key_name(ZTapAction on_data, void *data, ZAction on_plain, char *buf,
                     size_t n) {
    for (int i = 0; i < N_KEY_WORDS; i++) {
        if (on_plain == KEY_WORDS[i].act) {
            snprintf(buf, n, "%s", KEY_WORDS[i].name);
            return;
        }
    }
    if (on_data == on_char) {
        snprintf(buf, n, "%c", (char)(intptr_t)data);
        return;
    }
    if (on_data == on_suggest) {
        snprintf(buf, n, "SUG%d", (int)(intptr_t)data);
        return;
    }
    snprintf(buf, n, "?");
}

// --- the laid-out grid, as data ---------------------------------------------
// Every cap the layout produced: its frame, its name, the handler it carries and
// the character it yields. Read straight off the tree with z_probe_taps, so it is
// whatever the last build actually laid out — there is no second description of
// the keyboard anywhere in this file to drift from the first.
//
// Both the touch-target audit and the press classifier work from this, which is
// the point: the thing that decides what a press meant and the thing that reports
// how big the targets are cannot disagree about where the keys are.
#define AUDIT_MAX 64
typedef struct CapSet {
    int n;
    char name[AUDIT_MAX][16];
    ZKbdKey key[AUDIT_MAX];
    ZTapAction on_data[AUDIT_MAX];
    void *data[AUDIT_MAX];
    ZAction on_plain[AUDIT_MAX];
} CapSet;

static float in_pt(float units) {
    return units * (float)Z_TYPE_DEN / (float)Z_TYPE_NUM;
}

static void collect_cap(void *ud, ZTapAction on_data, void *data,
                        ZAction on_plain, float x, float y, float w, float h) {
    CapSet *a = ud;
    if (a->n >= AUDIT_MAX) {
        return;
    }
    int i = a->n++;
    key_name(on_data, data, on_plain, a->name[i], sizeof(a->name[i]));
    a->key[i].x = x;
    a->key[i].y = y;
    a->key[i].w = w;
    a->key[i].h = h;
    // The SYMBOL a cap yields, or 0 for a modifier. Only a-z and the word
    // boundary: a digit or a bracket on the symbols layer must get no vote rather
    // than a made-up one, because the model is English and knows nothing about
    // either.
    //
    // THE SPACE BAR IS A SYMBOL, NOT A MODIFIER (P48), and this one line is where
    // its target starts moving: everything downstream — the Gaussian, the odds
    // clamp, the argmax — already treats whatever has a `ch` as something the
    // model may have an opinion about. See predict.h.
    char c = (on_data == on_char) ? (char)(intptr_t)data : 0;
    if (on_plain == on_space) {
        c = Z_LM_BOUNDARY;
    }
    a->key[i].ch = (c >= 'a' && c <= 'z') || c == Z_LM_BOUNDARY ? c : 0;
    a->on_data[i] = on_data;
    a->data[i] = data;
    a->on_plain[i] = on_plain;
}

static void collect_caps(ZApp *app, CapSet *a) {
    a->n = 0;
    z_probe_taps(app, collect_cap, a);
}

// --- the press classifier ----------------------------------------------------
// See system/keyboard/predict.h for why a press is classified rather than
// hit-tested, and what the two rules are that it must never break. The prefix and
// the on/off decision are kbd_prefix / predict_on, next to the key handlers that
// share them.
//
// Which cap a press at (x, y) meant, without pressing it. Split out from the
// resolver so the audit can ask the SAME question the finger asks.
static int kbd_pick(ZApp *app, const CapSet *a, float x, float y, char *why,
                    size_t why_n) {
    return z_kbd_classify(a->key, a->n, x, y, kbd_prefix(app), predict_on(app),
                          why, why_n);
}

// While the accent popup is open, ONLY its cells are candidates. The letters
// underneath are still laid out and still tappable, and a release that resolved
// to one of them would type the letter you were trying to put a mark on.
static void keep_accents_only(CapSet *a) {
    int k = 0;
    for (int i = 0; i < a->n; i++) {
        if (a->on_data[i] != on_accent) {
            continue;
        }
        if (k != i) {
            snprintf(a->name[k], sizeof(a->name[k]), "%s", a->name[i]);
            a->key[k] = a->key[i];
            a->on_data[k] = a->on_data[i];
            a->data[k] = a->data[i];
            a->on_plain[k] = a->on_plain[i];
        }
        k++;
    }
    a->n = k;
}

// The tap resolver (z_tap_resolver): this surface answers "what did that press
// mean?" itself. Installed once, in kbd_body.
static bool kbd_resolve(ZApp *app, void *state, float x, float y) {
    KbdState *s = state;
    if (!s->visible) {
        return false;   // parked off the bottom: nothing here to press
    }
    CapSet a;
    collect_caps(app, &a);
    if (a.n == 0) {
        return false;
    }
    // A release that ends an ACCENT gesture. The popup is above the key, so the
    // finger has slid off the cap it started on — which is the point, and which
    // is why this cannot be an ordinary tap: the press and the release are on two
    // different controls and only the second one decides.
    //
    // Releasing anywhere that is NOT over the popup commits NOTHING. That is the
    // deliberate choice: a hold you did not follow through has to be cancellable,
    // and the alternative (commit the nearest accent, or fall back to the base
    // letter) means an accidental hold silently changes what you typed.
    if (s->accents) {
        keep_accents_only(&a);
        s->accents = false;
        int i = a.n > 0 ? z_kbd_nearest(a.key, a.n, x, y) : -1;
        bool inside = i >= 0 && x >= a.key[i].x && x <= a.key[i].x + a.key[i].w &&
                      y >= a.key[i].y && y <= a.key[i].y + a.key[i].h;
        if (inside) {
            z_probe_tap(app, a.on_data[i], a.data[i], a.on_plain[i]);
        } else {
            fprintf(stderr,
                    "[keyboard] accent popup dismissed at %.0f,%.0f (nothing "
                    "committed)\n",
                    x, y);
            fflush(stderr);
        }
        z_invalidate(app);
        return true;
    }
    // A release that ends a hold which already did something — the repeating
    // delete. The keys it deleted are what the gesture meant; emitting a tap on
    // top of them would delete one more.
    if (s->hold_fired) {
        s->hold_fired = false;
        return true;
    }
    char why[32];
    int i = kbd_pick(app, &a, x, y, why, sizeof(why));
    if (i < 0) {
        return false;
    }
    // What the RECTANGLES would have said, logged beside what was chosen. This is
    // the line that makes the difference visible: on a press in the gutter the
    // geometric answer is "nothing at all", and on a corrected miss it is the
    // neighbour.
    ZProbeHit g = z_probe_at(app, x, y);
    char gname[16] = "-";
    if (g.found) {
        key_name(g.on_data, g.data, g.on_plain, gname, sizeof(gname));
    }
    // Dispatch through the probe rather than calling the handler directly: it
    // hit-tests the CHOSEN cap's own centre and runs whatever the walk finds
    // there, so a cap that something is painted over still misbehaves visibly
    // instead of being typed anyway. hit/ran carry that claim into the log.
    ZProbeTap r = z_probe_tap(app, a.on_data[i], a.data[i], a.on_plain[i]);
    fprintf(stderr,
            "[keyboard] press x=%.0f y=%.0f geom='%s' chose='%s' why=%s "
            "prefix='%s' lm=%s hit=%s ran=%s\n",
            x, y, gname, a.name[i], why, kbd_prefix(app),
            predict_on(app) ? z_lm_name() : "off",
            r.hit_same ? "same" : "DIFFERENT", r.ran ? "yes" : "no");
    fflush(stderr);
    return true;
}

// --- the suggestion strip and autocorrect (P48) -----------------------------
// TWO MECHANISMS, ONE SOURCE OF TRUTH. The strip and the space bar must never
// disagree about what the correction is — a strip that offered "the" while the
// space bar committed "then" would be worse than no strip at all — so both go
// through kbd_autocorrect below, and the strip's pending-correction highlight IS
// the space bar's decision, drawn.
static void kbd_clear_revert(KbdState *s) {
    s->revert_armed = false;
}

// The word being typed: the letters right before the cursor, in their own case
// (so a capital survives the round trip through a correction). Stops at the first
// non-letter, because that is where the current word begins.
static int kbd_cur_word(ZApp *app, char *out, size_t n) {
    const char *p = kbd_prefix(app);
    int len = (int)strlen(p);
    int i = len;
    while (i > 0 && ((p[i - 1] >= 'a' && p[i - 1] <= 'z') ||
                     (p[i - 1] >= 'A' && p[i - 1] <= 'Z'))) {
        i--;
    }
    int wl = len - i;
    if (wl > (int)n - 1) {
        wl = (int)n - 1;
    }
    memcpy(out, p + i, (size_t)wl);
    out[wl] = '\0';
    return wl;
}

// The substitution cost the dictionary's edit distance uses, as a fact about THIS
// keyboard and not about English (predict.h explains why the model must not carry
// a copy of the layout). A near-neighbour on the grid is the commonest typo and
// the one the classifier could not catch — a deliberate press deep inside the
// wrong cap's centre zone — so it costs half of a substitution to a key across
// the board. `ud` is the laid-out CapSet.
static float kbd_subst_cost(void *ud, char want, char got) {
    const CapSet *a = ud;
    float wx = 0, wy = 0, gx = 0, gy = 0, cw = 0;
    bool wf = false, gf = false;
    for (int i = 0; i < a->n; i++) {
        if (a->key[i].ch == want) {
            wx = a->key[i].x + a->key[i].w * 0.5f;
            wy = a->key[i].y + a->key[i].h * 0.5f;
            cw = a->key[i].w;
            wf = true;
        }
        if (a->key[i].ch == got) {
            gx = a->key[i].x + a->key[i].w * 0.5f;
            gy = a->key[i].y + a->key[i].h * 0.5f;
            gf = true;
        }
    }
    if (!wf || !gf || cw <= 0.0f) {
        return 1.0f;   // one of them is not on this layer: no opinion
    }
    float dx = wx - gx, dy = wy - gy;
    float dist = sqrtf(dx * dx + dy * dy);
    return dist < 1.6f * cw ? 0.5f : 1.0f;
}

// THE CANDIDATE SEARCH, MEMOISED ON THE WORD (P49 item 1).
//
// WHY THIS EXISTS. kbd_suggest() runs on EVERY BUILD, and during a hold the body
// rebuilds at 40Hz; kbd_autocorrect() then asked the same question about the same
// word a second time in the same build. P48 measured the classifier — the thing
// it was asked to measure — and shipped this in the same phase with no number on
// it at all.
//
// THE NUMBERS, host and TARGET, because a cost measured only on a desktop CPU is
// the gap this project keeps falling into:
//   - HOST (meta/kbd-measure.sh, section 1): 929us for one scan before the model
//     memoised the caller's substitution cost, 79us after. A build paid it TWICE,
//     so 7.4% of a 40Hz frame, on the machine with the fast CPU.
//   - TARGET (zelto.kbdcaps=1, the "[keyboard] cost suggest" line in a KBDTAP
//     run): 3403us for the same scan on the same list. 37x the host, which is
//     13.6% of a 40Hz frame for ONE call and ~27% for the two a build made.
// Neither number is a crash and neither shows up as anything looking broken,
// which is precisely the shape of bug this project keeps finding.
//
// The memo is keyed on the word because that is the whole of what the answer
// depends on — the word cannot change between two builds unless the word changes,
// and if the word has not changed neither has its candidate list. The second key
// is the model's GENERATION: a learned word changes what the answer should be
// without changing the question, and it also invalidates the `word` pointers,
// which point into the model's own storage.
static struct {
    char word[Z_TEXTFIELD_CAP];
    ZLmWord cand[3];
    int n;
    unsigned gen;
    bool valid;
} g_cand;
static int kbd_hits, kbd_misses;   // reported by the caps audit

static int kbd_candidates(ZApp *app, const char *lower, ZLmWord *out) {
    if (g_cand.valid && g_cand.gen == z_lm_generation() &&
        strcmp(g_cand.word, lower) == 0) {
        kbd_hits++;
        for (int i = 0; i < g_cand.n; i++) {
            out[i] = g_cand.cand[i];
        }
        return g_cand.n;
    }
    kbd_misses++;
    CapSet a;
    collect_caps(app, &a);
    int nc = z_lm_candidates(lower, kbd_subst_cost, &a, out, 3);
    snprintf(g_cand.word, sizeof(g_cand.word), "%s", lower);
    for (int i = 0; i < nc; i++) {
        g_cand.cand[i] = out[i];
    }
    g_cand.n = nc;
    g_cand.gen = z_lm_generation();
    g_cand.valid = true;
    return nc;
}

// APOSTROPHES, PUT BACK (P49 item 3b).
//
// The words themselves are lexical data about English and live with the rest of
// it (CONTRACTIONS_EN in words_en.h, behind z_lm_contraction) — including the
// rule that decides what may be in the table at all: only contractions whose
// BARE FORM IS NOT ITSELF AN ENGLISH WORD. "were", "its" and "lets" are real
// words and stay bare forever.
//
// What is a KEYBOARD decision, and therefore here, is that an expansion is a
// CORRECTION LIKE ANY OTHER: it comes out of kbd_autocorrect, so it appears in
// the suggestion strip before it fires, the middle slot rejects it, and one
// backspace reverts it. P48 wrote down that expansion "changes a character the
// user did not type into one they cannot see" and needs the strip to be
// defensible. The strip exists now; this is the follow-through, and it ships
// through the visible path rather than as a quiet rewrite.

// Does autocorrect fire on `word`, and to what? The single decision the strip and
// every boundary share. Fires only when the word is NOT itself in the dictionary
// and there is a candidate clearly better than it — an exact hit is returned by
// z_lm_candidates with a score far above any correction, so "already a word" and
// "one edit from a word" are told apart by the same list.
static bool kbd_autocorrect(ZApp *app, const char *word, char *out, size_t n) {
    if (!predict_on(app)) {
        return false;
    }
    int wl = (int)strlen(word);
    if (wl < 2) {
        return false;   // a one-letter word is not a typo worth touching
    }
    char lower[Z_TEXTFIELD_CAP];
    for (int i = 0; i < wl && i < (int)sizeof(lower) - 1; i++) {
        char c = word[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lower[i] < 'a' || lower[i] > 'z') {
            return false;   // not a plain word (a digit crept in): leave it
        }
    }
    lower[wl < (int)sizeof(lower) ? wl : (int)sizeof(lower) - 1] = '\0';
    // The contraction table is consulted BEFORE the dictionary guards, because
    // its whole subject is the words that ARE in the dictionary as bare forms —
    // z_lm_is_word("dont") is true, and it has to be, or "dont" would be
    // corrected into "dot" by edit distance.
    const char *full = z_lm_contraction(lower);
    if (full) {
        snprintf(out, n, "%s", full);
        if (word[0] >= 'A' && word[0] <= 'Z' && out[0] >= 'a' && out[0] <= 'z') {
            out[0] = (char)(out[0] - 32);
        }
        return strcmp(out, word) != 0;
    }
    if (z_lm_is_word(lower)) {
        return false;   // spelled a real word: never "correct" it
    }
    if (z_lm_is_prefix(lower)) {
        return false;   // part-way through a real word ("hel" -> hello): not a typo
    }
    ZLmWord cand[3];
    int nc = kbd_candidates(app, lower, cand);
    if (nc == 0) {
        return false;   // nothing close enough: leave it exactly as typed
    }
    // Case-match the correction to what was typed: a leading capital carries over
    // (a sentence start, a name), the rest is the dictionary's lower case.
    int cl = (int)strlen(cand[0].word);
    if (cl > (int)n - 1) {
        cl = (int)n - 1;
    }
    memcpy(out, cand[0].word, (size_t)cl);
    out[cl] = '\0';
    if (word[0] >= 'A' && word[0] <= 'Z' && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = (char)(out[0] - 32);
    }
    return strcmp(out, word) != 0;   // no-op if it "corrected" to the same string
}

// Tapping a strip slot commits that slot's word in place of what is being typed,
// plus a space — the explicit version of the correction the boundary would apply,
// and the way to REJECT a pending autocorrect (tap the middle slot, which is
// always the literal text). No revert is armed: the user chose this one, so a
// backspace after it means backspace.
static void on_suggest(ZApp *app, void *state, void *data) {
    KbdState *s = state;
    int idx = (int)(intptr_t)data;
    if (idx < 0 || idx > 2 || !s->slot[idx][0]) {
        return;
    }
    kbd_clear_revert(s);
    char word[Z_TEXTFIELD_CAP];
    int wl = kbd_cur_word(app, word, sizeof(word));
    if (wl > 0) {
        z_im_delete(app, wl);
    }
    z_im_commit_text(app, s->slot[idx]);
    z_im_commit_text(app, " ");
    fprintf(stderr, "[keyboard] suggest slot %d '%s' (was '%s')\n", idx,
            s->slot[idx], word);
    fflush(stderr);
}

// Recompute the strip, every build. slot[1] is the literal word; slot[0] and
// slot[2] are the dictionary's two best guesses that differ from it. slot_auto is
// the one autocorrect would apply on a boundary — the same call the space bar
// makes — so the highlight and the behaviour are one decision.
static void kbd_suggest(ZApp *app, KbdState *s) {
    for (int i = 0; i < 3; i++) {
        s->slot[i][0] = '\0';
    }
    s->slot_auto = -1;
    if (!predict_on(app)) {
        return;   // no strip in a password field
    }
    char word[Z_TEXTFIELD_CAP];
    if (kbd_cur_word(app, word, sizeof(word)) == 0) {
        return;   // between words: a blank strip, not stale suggestions
    }
    snprintf(s->slot[1], sizeof(s->slot[1]), "%s", word);

    char lower[Z_TEXTFIELD_CAP];
    int wl = (int)strlen(word);
    for (int i = 0; i < wl; i++) {
        char c = word[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[wl] = '\0';

    // THE PENDING CORRECTION FILLS SLOT 0 RATHER THAN BEING CHECKED AGAINST IT
    // (P49). P48 built the slots from the candidate list and then asked
    // kbd_autocorrect separately whether it agreed with slot 0 — which was one
    // decision computed twice, and the two could differ silently. Now the
    // boundary's own answer IS slot 0 whenever it fires, so the strip cannot show
    // a correction the space bar would not apply, or hide one it would. It is
    // also the only way the contraction table could ever reach the strip: "don't"
    // is not a candidate the trie can return.
    char corr[Z_TEXTFIELD_CAP] = {0};
    int put = 0;
    int slots[2] = {0, 2};
    if (kbd_autocorrect(app, word, corr, sizeof(corr))) {
        snprintf(s->slot[0], sizeof(s->slot[0]), "%s", corr);
        s->slot_auto = 0;
        put = 1;
    }
    // The correction is CASE-MATCHED to what was typed and a candidate is not, so
    // the two are compared with the case taken off — otherwise "The" in slot 0 and
    // "the" from the list read as different words and the strip shows the same
    // suggestion twice.
    for (int i = 0; corr[i]; i++) {
        if (corr[i] >= 'A' && corr[i] <= 'Z') {
            corr[i] = (char)(corr[i] + 32);
        }
    }
    ZLmWord cand[3];
    int nc = kbd_candidates(app, lower, cand);
    bool cap = word[0] >= 'A' && word[0] <= 'Z';
    for (int i = 0; i < nc && put < 2; i++) {
        if (strcmp(cand[i].word, lower) == 0) {
            continue;   // the literal already IS slot 1
        }
        if (s->slot_auto == 0 && strcmp(cand[i].word, corr) == 0) {
            continue;   // already shown as the pending correction
        }
        char *dst = s->slot[slots[put++]];
        snprintf(dst, sizeof(s->slot[0]), "%s", cand[i].word);
        if (cap && dst[0] >= 'a' && dst[0] <= 'z') {
            dst[0] = (char)(dst[0] - 32);
        }
    }
}

// --- the learned dictionary (P49 item 2) -------------------------------------
// THE KEYBOARD KNOWS 1620 WORDS AND NOT ONE OF THEM IS YOURS. That is the whole
// of what this adds. It is a privacy decision at least as much as a technical
// one, so the three questions are answered here, next to the code that
// implements each of them, rather than in a design document nobody reads while
// changing this file.
//
// 1. WHAT IS STORED. A word: lowercase a-z, two characters or more, that the
//    shipped dictionary does not carry, and that the user has shown was not a
//    typo. TWO SIGNALS COUNT, and they are deliberately different in kind:
//      - EXPLICIT (on_backspace): autocorrect changed the word and the user took
//        it back. That is a person saying "no, I meant this" about a word the
//        model already gave its opinion on, and it learns on the FIRST one.
//      - IMPLICIT (kbd_learn_seen): the same string survived KBD_LEARN_SEEN word
//        boundaries without being corrected. A word typed ONCE is a typo.
//    THE COUNTS ARE NEVER WRITTEN TO DISK — only words that reached the
//    threshold are. A file of "strings this person typed once" is exactly the log
//    a learned dictionary must not become, and the cost of keeping the counters
//    in RAM is precisely stated: a word typed twice today and once tomorrow
//    starts again. That is the right way round.
//
// 2. WHERE. /var/zelto, through the storage the OS already has (P11) — one
//    newline-separated file in the keyboard's own private directory, the same
//    shape as words_en.h, because a store the user cannot read is a log. It
//    survives a reboot, which is what makes it testable by a second sim boot on
//    the same ZELTO_DATA_DIR rather than by an emulator.
//
// 3. WHAT NEVER GETS IN. The password rule is necessary and it is nowhere near
//    sufficient, so all of these hold:
//      - NOTHING FROM A PASSWORD FIELD. predict_on() is false there, so the
//        counter is not even incremented — the string never enters the table in
//        RAM, let alone the file. That is the same purpose the compositor has
//        relayed since P21 and the classifier has honoured since P47.
//      - NOTHING THAT IS NOT A WORD. kbd_cur_word stops at the first non-letter,
//        so an email address, an API key, a postcode and a phone number are not
//        words to any of this. A capital anywhere but the first position is also
//        out (z_lm_learn takes a-z only), which removes most identifiers.
//      - NOTHING WITHOUT A WAY OUT. Settings > Keyboard shows how many words have
//        been learned and offers Clear Learned Words, which forgets them, blanks
//        the bytes and deletes the file. A store with no way out of it is not a
//        feature, it is a leak with a nice name.
//    WHAT IS DELIBERATELY NOT SHOWN: the words themselves. A screen listing what
//    somebody typed is a shoulder-surfing surface of its own, and the count plus
//    the delete answers "what do you have, and get rid of it" without building
//    one. If a later phase wants a per-word editor it should decide separately
//    whether it is worth that.
static void dict_publish(KbdState *s) {
    z_setting_set_int(ZELTO_KEY_KBD_LEARNED, s->learned_n);
}

// Write the learned words out. Called after every learn, because the alternative
// is deciding when a keyboard is about to be killed.
static void dict_save(void) {
    const char *w[KBD_LEARN_MAX];
    int n = z_lm_learned(w, KBD_LEARN_MAX);
    char buf[KBD_LEARN_MAX * (KBD_LEARN_WORD_CAP + 1)];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        size_t wl = strlen(w[i]);
        if (len + wl + 1 >= sizeof(buf)) {
            break;
        }
        memcpy(buf + len, w[i], wl);
        len += wl;
        buf[len++] = '\n';
    }
    if (!z_file_write(KBD_DICT_FILE, buf, len)) {
        fprintf(stderr, "[keyboard] learned: could not write %s\n",
                KBD_DICT_FILE);
        fflush(stderr);
    }
}

// Read them back at startup and hand each one to the model. This is the whole of
// "it survives a reboot": the model is built from a list, and the learned words
// join that list before anything asks it a question.
//
// IT ALSO HONOURS A CLEAR THAT HAPPENED WHILE WE WERE NOT RUNNING. Settings can
// be used, and the phone rebooted, without the keyboard process ever seeing the
// broadcast — so the clear is an EPOCH that is compared, not an event that is
// observed. The epoch we have honoured lives in our own prefs; if the broker's
// is different, we forget before we load.
static void dict_load(KbdState *s) {
    if (s->dict_loaded) {
        return;
    }
    s->dict_loaded = true;

    int64_t want = z_setting_get_int(ZELTO_KEY_KBD_FORGET, 0);
    s->clear_epoch = z_prefs_get_int("learned.clear_epoch", 0);
    if (want != s->clear_epoch) {
        s->clear_epoch = want;
        z_prefs_set_int("learned.clear_epoch", want);
        z_lm_forget_all();
        z_file_delete(KBD_DICT_FILE);
        s->learned_n = 0;
        dict_publish(s);
        fprintf(stderr, "[keyboard] learned: cleared (epoch %lld)\n",
                (long long)want);
        fflush(stderr);
        return;
    }

    ZBytes b = z_file_read(KBD_DICT_FILE);
    if (!b.ok) {
        dict_publish(s);
        return;   // nothing learned yet: not an error
    }
    char *p = b.data;
    while (*p && s->learned_n < KBD_LEARN_MAX) {
        char *q = p;
        while (*q && *q != '\n') {
            q++;
        }
        char save = *q;
        *q = '\0';
        if (z_lm_learn(p)) {
            s->learned_n++;
        }
        if (!save) {
            break;
        }
        p = q + 1;
    }
    free(b.data);
    fprintf(stderr, "[keyboard] learned: %d word(s) restored from %s\n",
            s->learned_n, KBD_DICT_FILE);
    fflush(stderr);
    dict_publish(s);
}

// Teach the model a word NOW: the explicit path (a revert) and the end of the
// implicit one (the counter reached the threshold) both land here.
static bool kbd_learn(ZApp *app, KbdState *s, const char *word) {
    if (!predict_on(app)) {
        return false;   // a password field teaches this keyboard nothing
    }
    if (s->learned_n >= KBD_LEARN_MAX) {
        return false;
    }
    char lower[KBD_LEARN_WORD_CAP];
    int wl = (int)strlen(word);
    if (wl < 2 || wl >= (int)sizeof(lower)) {
        return false;
    }
    for (int i = 0; i < wl; i++) {
        char c = word[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lower[i] < 'a' || lower[i] > 'z') {
            return false;
        }
    }
    lower[wl] = '\0';
    if (!z_lm_learn(lower)) {
        return false;   // already known, malformed, or the model is full
    }
    s->learned_n++;
    // The candidate memo is keyed on the model's generation, which z_lm_learn
    // just bumped — so nothing stale can survive this. Saying so here because the
    // invalidation is the kind of thing that is obvious until it is missing.
    dict_save();
    dict_publish(s);
    fprintf(stderr, "[keyboard] learned '%s' (%d total)\n", lower, s->learned_n);
    fflush(stderr);
    return true;
}

// Count a word that came through a boundary uncorrected. Returns quietly for
// everything that is already known, which is nearly every word — the table only
// ever holds strings the dictionary does not have.
static void kbd_learn_seen(ZApp *app, KbdState *s, const char *word) {
    if (!predict_on(app)) {
        return;
    }
    char lower[KBD_LEARN_WORD_CAP];
    int wl = (int)strlen(word);
    if (wl < 2 || wl >= (int)sizeof(lower)) {
        return;
    }
    for (int i = 0; i < wl; i++) {
        char c = word[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        if (lower[i] < 'a' || lower[i] > 'z') {
            return;
        }
    }
    lower[wl] = '\0';
    if (z_lm_is_word(lower)) {
        return;   // the dictionary already has it: nothing to learn
    }
    int slot = -1, weakest = 0;
    for (int i = 0; i < KBD_SEEN_SLOTS; i++) {
        if (strcmp(s->seen[i].word, lower) == 0) {
            slot = i;
            break;
        }
        if (s->seen[i].seen < s->seen[weakest].seen) {
            weakest = i;
        }
    }
    if (slot < 0) {
        slot = weakest;   // the least-seen candidate makes room; see the note above
        snprintf(s->seen[slot].word, sizeof(s->seen[slot].word), "%s", lower);
        s->seen[slot].seen = 0;
    }
    s->seen[slot].seen++;
    fprintf(stderr, "[keyboard] learn candidate '%s' seen %d/%d\n", lower,
            s->seen[slot].seen, KBD_LEARN_SEEN);
    fflush(stderr);
    if (s->seen[slot].seen >= KBD_LEARN_SEEN) {
        s->seen[slot].seen = 0;
        s->seen[slot].word[0] = '\0';
        kbd_learn(app, s, lower);
    }
}

// Settings cleared the learned words while we were running. The same code path
// dict_load() takes on a cold boot, because it is the same decision — the only
// difference is whether the process happened to be alive when it was made.
static void on_setting(ZApp *app, const char *key, const char *value, void *ud) {
    KbdState *s = ud;
    if (strcmp(key, ZELTO_KEY_KBD_FORGET) != 0) {
        return;
    }
    int64_t want = atoll(value);
    if (want == s->clear_epoch) {
        return;   // our own echo, or a repeat: idempotent, the client never loops
    }
    s->clear_epoch = want;
    z_prefs_set_int("learned.clear_epoch", want);
    z_lm_forget_all();
    z_file_delete(KBD_DICT_FILE);
    for (int i = 0; i < KBD_SEEN_SLOTS; i++) {
        s->seen[i].word[0] = '\0';   // the pending candidates go too
        s->seen[i].seen = 0;
    }
    s->learned_n = 0;
    dict_publish(s);
    fprintf(stderr, "[keyboard] learned: cleared on request (epoch %lld)\n",
            (long long)want);
    fflush(stderr);
    z_invalidate(app);
}

// --- the live press ---------------------------------------------------------
// What the callout shows, refreshed on every move: run the classifier at the
// current point and remember its answer plus the frame of the cap that won.
static void refresh_preview(ZApp *app, KbdState *s) {
    CapSet a;
    collect_caps(app, &a);
    if (a.n == 0) {
        s->preview = false;
        return;
    }
    int i = kbd_pick(app, &a, s->px, s->py, NULL, 0);
    if (i < 0) {
        s->preview = false;
        return;
    }
    snprintf(s->hold_name, sizeof(s->hold_name), "%s", a.name[i]);
    s->base_x = a.key[i].x;
    s->base_y = a.key[i].y;
    s->base_w = a.key[i].w;
    s->base_h = a.key[i].h;
    // Only character caps get a callout. A modifier's mark is not obscured by the
    // finger in the way a 32pt letter is, and iOS shows one for exactly the same
    // set — the callout is there so you can read what you hit, not to celebrate
    // every press.
    if (a.key[i].ch && a.key[i].ch != Z_LM_BOUNDARY) {
        char c = a.key[i].ch;
        if (kbd_upper(s) && c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        snprintf(s->preview_ch, sizeof(s->preview_ch), "%c", c);
        s->preview = true;
    } else if (!a.name[i][1]) {
        snprintf(s->preview_ch, sizeof(s->preview_ch), "%s", a.name[i]);
        s->preview = true;   // a digit or a symbol cap
    } else {
        s->preview = false;
    }
    s->preview_x = a.key[i].x;
    s->preview_y = a.key[i].y;
    s->preview_w = a.key[i].w;
    s->preview_h = a.key[i].h;
    // Logged on CHANGE, not per build (the hold ticks at 40Hz), and with the
    // geometric answer beside it — because the whole question about this callout
    // is which of the two it shows, and a log line naming only one of them could
    // not settle it.
    if (s->preview && strcmp(s->preview_ch, s->shown_ch) != 0) {
        snprintf(s->shown_ch, sizeof(s->shown_ch), "%s", s->preview_ch);
        ZProbeHit g = z_probe_at(app, s->px, s->py);
        char gname[16] = "-";
        if (g.found) {
            key_name(g.on_data, g.data, g.on_plain, gname, sizeof(gname));
        }
        fprintf(stderr, "[keyboard] callout '%s' (geom '%s')\n", s->preview_ch,
                gname);
        fflush(stderr);
    }
}

// z_press_hook: the finger, from landing to lifting.
static void kbd_press(ZApp *app, void *state, ZPressPhase phase, float x,
                      float y) {
    KbdState *s = state;
    if (!s->visible) {
        return;
    }
    s->px = x;
    s->py = y;
    if (phase == Z_PRESS_DOWN) {
        s->down = true;
        s->down_s = z_now_seconds();
        s->hold_fired = false;
        s->repeating = false;
        s->repeats = 0;
        s->word_mode = false;
        refresh_preview(app, s);
    } else if (phase == Z_PRESS_MOVE) {
        // While the popup is open the callout goes away: the popup IS the
        // feedback, and a second floating letter over it would be two answers to
        // the same question.
        if (!s->accents) {
            refresh_preview(app, s);
        }
    } else {
        s->down = false;
        s->preview = false;
        s->shown_ch[0] = '\0';
        s->repeating = false;
    }
    z_invalidate(app);
}

// The hold, ticked from body() while a finger is down. It lives on the build
// rather than on a timer callback because z_after is ONE shared slot (the P45
// note on ZELTO_PROBE_AT) and the harness's tap sequence already owns it — two
// state machines on one slot is how the second one silently stops running.
static void kbd_hold_tick(ZApp *app, KbdState *s) {
    double now = z_now_seconds();
    double held = now - s->down_s;
    z_tick_every(app, 40);
    if (!s->hold_fired && held >= KBD_HOLD_S) {
        const struct AccentRow *r =
            s->hold_name[0] && !s->hold_name[1] ? accents_for(s->hold_name[0])
                                                : NULL;
        if (r) {
            s->accents = true;
            s->accent_base = r->base;
            s->hold_fired = true;
            fprintf(stderr, "[keyboard] accents open for '%c' (%d)\n", r->base,
                    r->n);
            fflush(stderr);
        } else if (strcmp(s->hold_name, "BKSP") == 0) {
            s->repeating = true;
            s->hold_fired = true;
            s->next_repeat_s = now;   // the first one, immediately
            fprintf(stderr, "[keyboard] backspace repeat begins\n");
        } else {
            s->hold_fired = true;   // a hold on a key with nothing to offer
            fprintf(stderr, "[keyboard] hold on '%s': nothing to offer\n",
                    s->hold_name);
        }
        fflush(stderr);
        z_invalidate(app);
    }
    if (!s->repeating || now < s->next_repeat_s) {
        return;
    }
    double repeating_for = held - KBD_HOLD_S;
    // WORDS, not characters, once it has been going long enough to mean "get rid
    // of that". Sent as ONE delete_surrounding_text rather than N backspaces so
    // the field sees a single edit — N of them would race the surrounding-text
    // update this counts FROM, and a stale count deletes the same word twice.
    if (repeating_for >= KBD_REPEAT_WORD_S) {
        const char *p = kbd_prefix(app);
        int n = (int)strlen(p);
        int i = n;
        while (i > 0 && p[i - 1] == ' ') { i--; }
        while (i > 0 && p[i - 1] != ' ') { i--; }
        int del = n - i;
        s->word_mode = true;
        s->repeats++;
        s->next_repeat_s = now + KBD_REPEAT_WORD_GAP_S;
        if (del > 0) {
            z_im_delete(app, del);
        }
        fprintf(stderr, "[keyboard] backspace repeat n=%d mode=word deleted %d\n",
                s->repeats, del);
    } else {
        double gap = repeating_for >= KBD_REPEAT_ACCEL_S ? KBD_REPEAT_FAST_S
                                                         : KBD_REPEAT_S;
        s->repeats++;
        s->next_repeat_s = now + gap;
        z_im_backspace(app);
        fprintf(stderr,
                "[keyboard] backspace repeat n=%d mode=char gap=%.0fms\n",
                s->repeats, gap * 1000.0);
    }
    fflush(stderr);
}

// --- the touch-target audit -------------------------------------------------
// ZELTO_KBD_CAPS=1 measures every cap on the laid-out grid and prints it. The
// numbers are the assertion; this file does not decide what passes.
static void audit_caps(ZApp *app) {
    CapSet a;
    collect_caps(app, &a);

    float min_w = 0.0f, min_h = 0.0f;
    for (int i = 0; i < a.n; i++) {
        // Reported in POINTS as well as units, because 44pt is the number anybody
        // arguing about a touch target reaches for, and converting it in a shell
        // script is how a metric ends up wrong in two places.
        fprintf(stderr,
                "[keyboard] cap '%s' x=%.0f y=%.0f w=%.0f h=%.0f "
                "(%.1fpt x %.1fpt)\n",
                a.name[i], a.key[i].x, a.key[i].y, a.key[i].w, a.key[i].h,
                in_pt(a.key[i].w), in_pt(a.key[i].h));
        if (i == 0 || a.key[i].w < min_w) {
            min_w = a.key[i].w;
        }
        if (i == 0 || a.key[i].h < min_h) {
            min_h = a.key[i].h;
        }
    }
    fprintf(stderr,
            "[keyboard] caps: %d total, smallest %.0fx%.0f units "
            "(%.1fpt x %.1fpt), targets=%s\n",
            a.n, min_w, min_h, in_pt(min_w), in_pt(min_h),
            predict_on(app) ? z_lm_name() : "geometry");

    // Per row: are the caps uniform, how much of the row PAINTS nothing, and how
    // much of it MEANS nothing?
    //
    // The two are different questions and P46 only had the first. A cap is a
    // rounded face with a KEY_GAP between it and the next, and that gap is
    // painted by no key — 11 units, about 15% of every row. Under plain
    // rectangles it was also HIT by no key: a press there reached the material
    // behind and did nothing, which is the "dead" number P46 printed and could
    // not fix without a paint-vs-hit split the toolkit lacks.
    //
    // The classifier is that split, arrived at from the other end. It never
    // needed the toolkit to grow a hit region, because it does not ask which
    // rectangle contains the point — it asks which key the point most likely
    // MEANT, and every point on the strip means something. So `gutter` is still
    // ~15% (nothing has been repainted) and `dead` is 0. Both are printed,
    // because a run in which they were equal again would be the classifier
    // silently not running, and one number cannot show that.
    //
    // `dead` is measured through kbd_pick — the same function the finger's
    // resolver calls — at one-unit steps across the row, so it is an answer about
    // the real press path and not about a formula written twice.
    for (int i = 0; i < a.n; i++) {
        if (i > 0 && a.key[i].y == a.key[i - 1].y) {
            continue;             // same row, already summarised
        }
        float y = a.key[i].y;
        float lo = a.key[i].x, hi = a.key[i].x + a.key[i].w;
        float covered = 0.0f, cmin = 0.0f, cmax = 0.0f;
        int cells = 0, chars = 0;
        for (int j = 0; j < a.n; j++) {
            if (a.key[j].y != y) {
                continue;
            }
            covered += a.key[j].w;
            if (a.key[j].x < lo) { lo = a.key[j].x; }
            if (a.key[j].x + a.key[j].w > hi) { hi = a.key[j].x + a.key[j].w; }
            cells++;
            // Uniformity is a claim about the CHARACTER caps: a modifier is
            // deliberately 1.6x or 5.6x a letter, so folding them in would make
            // every row look ragged and hide the thing being watched for.
            if (!a.name[j][1]) {
                if (chars == 0 || a.key[j].w < cmin) { cmin = a.key[j].w; }
                if (chars == 0 || a.key[j].w > cmax) { cmax = a.key[j].w; }
                chars++;
            }
        }
        float span = hi - lo;
        int dead = 0, sampled = 0;
        // WHAT A PRESS COSTS, MEASURED (P48 item 3). The classifier had never
        // been timed: it runs on every DOWN, and again on every MOVE while a
        // finger is held, which kbd_hold_tick drives at 40Hz. P45 measured plain
        // hit-testing at 124 nodes / 6.1us and both that brief and P44 were wrong
        // about when it ran, so this is a number and not a shrug.
        //
        // The sweep below IS the measurement: it is the same kbd_pick the finger
        // calls, over the whole row at one-unit steps, so per-call cost falls out
        // of the wall clock divided by the sample count. Note what is and is not
        // in it — collect_caps() walked the tree ONCE, outside this loop, so this
        // times the scoring and not the layout walk. The two are reported
        // separately below for that reason.
        double t0 = z_now_seconds();
        for (float x = lo; x <= hi; x += 1.0f) {
            sampled++;
            if (kbd_pick(app, &a, x, y + a.key[i].h * 0.5f, NULL, 0) < 0) {
                dead++;
            }
        }
        double pick_us = sampled > 0
                             ? (z_now_seconds() - t0) * 1e6 / (double)sampled
                             : 0.0;
        // ...and what the tree walk costs, which a real press pays too: the
        // resolver calls collect_caps() before it calls kbd_pick(), so a press is
        // one of these plus one of those.
        double t1 = z_now_seconds();
        CapSet scratch;
        for (int r = 0; r < 32; r++) {
            collect_caps(app, &scratch);
        }
        double collect_us = (z_now_seconds() - t1) * 1e6 / 32.0;
        fprintf(stderr,
                "[keyboard] cost row y=%.0f: classify %.2fus/press over %d caps, "
                "collect %.2fus/press, press total %.2fus\n",
                y, pick_us, a.n, collect_us, pick_us + collect_us);
        fprintf(stderr,
                "[keyboard] row y=%.0f: %d caps, span %.0f, covered %.0f, "
                "gutter %.0f (%.1f%%), dead %d of %d (%.1f%%), "
                "char widths %.0f..%.0f\n",
                y, cells, span, covered, span - covered,
                span > 0.0f ? 100.0f * (span - covered) / span : 0.0f, dead,
                sampled, sampled > 0 ? 100.0f * (float)dead / (float)sampled
                                     : 0.0f,
                cmin, cmax);
    }

    // WHAT THE SUGGESTION STRIP COSTS, MEASURED (P49 item 1).
    //
    // P48 measured the classifier — the thing it was asked to measure — and in
    // the same phase shipped something an order of magnitude more expensive
    // without a number on it. z_lm_candidates runs an edit-distance scan over the
    // whole word list, kbd_suggest() called it from kbd_body() on EVERY BUILD,
    // and kbd_body rebuilds at 40Hz while a finger is held. Nothing looked
    // broken, which is the shape of every bug this project keeps finding.
    //
    // Measured here rather than in a benchmark because the substitution cost is a
    // fact about the LAID-OUT CAPS: a synthetic row would time a different
    // function. Two numbers come out — the raw scan, and the hit rate of the memo
    // that means a build almost never pays for it (kbd_candidates).
    {
        CapSet a;
        collect_caps(app, &a);
        static const char *PROBE[] = {"hel", "hello", "wrold", "keyboard"};
        int np = (int)(sizeof(PROBE) / sizeof(PROBE[0]));
        ZLmWord cand[3];
        double worst = 0.0;
        const char *worst_w = "";
        for (int i = 0; i < np; i++) {
            double t = z_now_seconds();
            for (int r = 0; r < 8; r++) {
                z_lm_candidates(PROBE[i], kbd_subst_cost, &a, cand, 3);
            }
            double per = (z_now_seconds() - t) * 1e6 / 8.0;
            if (per > worst) {
                worst = per;
                worst_w = PROBE[i];
            }
        }
        // The memo counters are a snapshot AT THIS POINT of the boot — the audit
        // runs on the first build, so on a boot with no typing they are both
        // zero and the scan number is the whole of what this line says.
        fprintf(stderr,
                "[keyboard] cost suggest: %.0fus worst scan (\"%s\") over %d "
                "words; memo %d hit / %d miss so far; a 40Hz build pays the "
                "scan only when the word changes\n",
                worst, worst_w, z_lm_word_count(), kbd_hits, kbd_misses);
    }
    fflush(stderr);
}

// The end of a held press: slide onto the named target if there is one, then
// release. Split out of tap_tick because the wait between them is a timer, and
// because the SLIDE has to resolve a control the HOLD created — the accent cells
// do not exist until the popup opens, so their frames cannot be looked up before
// the press.
static void hold_release(ZApp *app, void *ud) {
    KbdState *s = ud;
    float px = s->hold_px, py = s->hold_py;
    if (s->slide_to[0]) {
        const struct AccentRow *r = accents_for(s->accent_base);
        const char *target = NULL;
        for (int i = 0; r && i < r->n; i++) {
            if (strcmp(r->alt[i], s->slide_to) == 0) {
                target = r->alt[i];   // the POINTER is the identity, not the text
            }
        }
        ZProbeTap t = target ? z_probe_frame(app, on_accent,
                                             (void *)(intptr_t)target, NULL)
                             : (ZProbeTap){0};
        if (!t.found) {
            fprintf(stderr,
                    "[keyboard] slide to '%s': NO SUCH ACCENT ON SCREEN\n",
                    s->slide_to);
            fflush(stderr);
        } else {
            px = t.x + t.w * 0.5f;
            py = t.y + t.h * 0.5f;
            fprintf(stderr,
                    "[keyboard] slide to '%s' at %.0f,%.0f (frame %.0f,%.0f "
                    "%.0fx%.0f)\n",
                    s->slide_to, px, py, t.x, t.y, t.w, t.h);
            fflush(stderr);
            z_probe_press_phase(app, Z_PRESS_MOVE, px, py);
        }
    }
    z_probe_press_phase(app, Z_PRESS_UP, px, py);
    z_probe_press(app, px, py);
    fflush(stderr);
    z_after(app, 250, tap_tick, s);
}

// Press the next cap named by ZELTO_KBD_TAP, then re-arm.
//
// The wait for the slide is not politeness. The grid rides an Offset driven by
// the show spring, and an Offset bakes into the layout — so while the keyboard is
// sliding up, every cap's frame is genuinely somewhere else. Pressing then would
// resolve a frame that is about to move and, worse, might still be off the bottom
// of the surface where the hit walk's own surface clip refuses it.
//
// IT WAS A GUARD NOTHING EXERCISED, AND NOW IT IS THE MECHANISM (P47). P46 wrote
// this down honestly: removing the check made no test fail, because the first
// press was armed 400ms after the show handshake and the spring had already
// arrived — the wait was being done by the arming delay, and this was dead code
// that happened to be correct. The arming delay is 50ms now (on_show), so the
// gate is what waits, on every boot of every keyboard test. The alternative was
// to delete it, and that would have meant the arming delay stayed a magic number
// tuned against a spring nobody re-measures.
//
// It also does double duty on the first call, when app->root may not exist yet:
// the spring reads 0 then, so the retry below covers both.
static void tap_tick(ZApp *app, void *ud) {
    KbdState *s = ud;
    if (z_animated_get(s->anim) < 0.999f) {
        z_after(app, 100, tap_tick, s);   // still sliding; the caps are moving
        return;
    }
    // The audit runs off the same settled grid, and on its own: measuring the
    // caps is a claim about the LAYOUT, so it must not need a tap sequence.
    if (!s->audited) {
        s->audited = true;
        if (getenv("ZELTO_KBD_CAPS")) {
            audit_caps(app);
        }
    }
    const char *spec = getenv("ZELTO_KBD_TAP");
    if (!spec || !spec[0]) {
        return;
    }

    // Walk to token number s->tapped.
    const char *p = spec;
    for (int i = 0; i < s->tapped && p; i++) {
        p = strchr(p, ',');
        if (p) {
            p++;
        }
    }
    if (!p || !*p) {
        fprintf(stderr, "[keyboard] taps done (%d)\n", s->tapped);
        fflush(stderr);
        return;
    }
    char tok[32];
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof(tok)) {
        len = sizeof(tok) - 1;
    }
    memcpy(tok, p, len);
    tok[len] = '\0';
    s->tapped++;

    // A token is a key name, optionally followed by a DELIBERATE MISS:
    // "l@-45" presses 45 units left of where the layout put the 'l' cap's centre,
    // "e@0:20" 20 units below it. The offsets are coordinate-free in the sense
    // that matters — they are relative to the frame the layout answered with, so
    // they move when the keyboard moves — and they are the only way to test the
    // thing this keyboard is FOR: a press that is not on the key.
    //
    // A token may also be a HOLD, and a hold may end on something the hold itself
    // created:
    //   "BKSP~2600"   press, wait 2600ms, release  (the repeating delete)
    //   "e~600>é"     press, wait 600ms, then SLIDE onto the popup cell that
    //                 commits "é", and release there
    // The slide target is named, not measured: the accent's frame is resolved
    // from the tree the popup laid out, so the gesture follows the popup wherever
    // it goes — including the top row, where the popup is below the key instead
    // of above it. There is no coordinate in a hold token either.
    char name[sizeof(tok)];
    float off_x = 0.0f, off_y = 0.0f;
    int hold_ms = 0;
    char slide_to[16] = "";
    {
        char *arrow = strchr(tok, '>');
        if (arrow) {
            *arrow = '\0';
            snprintf(slide_to, sizeof(slide_to), "%s", arrow + 1);
        }
        char *tilde = strchr(tok, '~');
        if (tilde) {
            *tilde = '\0';
            hold_ms = atoi(tilde + 1);
        }
        char *at = strchr(tok, '@');
        if (at) {
            *at = '\0';
            char *colon = strchr(at + 1, ':');
            if (colon) {
                *colon = '\0';
                off_y = (float)atof(colon + 1);
            }
            off_x = (float)atof(at + 1);
        }
        snprintf(name, sizeof(name), "%s", tok);
    }

    ZTapAction on_data;
    void *data;
    ZAction on_plain;
    if (!key_handler(name, &on_data, &data, &on_plain)) {
        fprintf(stderr, "[keyboard] tap '%s': no such key name\n", name);
        fflush(stderr);
        z_after(app, 250, tap_tick, s);
        return;
    }
    ZProbeTap t = z_probe_frame(app, on_data, data, on_plain);
    if (!t.found) {
        // Not a crash: on the symbols layer there is no 'q' and no SHIFT, and a
        // test that asks for one should read a specific line saying so.
        fprintf(stderr, "[keyboard] tap '%s': NOT ON THIS LAYER\n", name);
        fflush(stderr);
        z_after(app, 250, tap_tick, s);
        return;
    }
    float px = t.x + t.w * 0.5f + off_x;
    float py = t.y + t.h * 0.5f + off_y;
    // What the RECTANGLES say is at the point about to be pressed, taken BEFORE
    // pressing it and reported separately. This is what stops the whole exercise
    // from being vacuous: "the keyboard typed hello" proves nothing unless the
    // presses really were off the keys, and the only witness to that is the plain
    // geometric walk saying it found the neighbour, or nothing at all.
    ZProbeHit g = z_probe_at(app, px, py);
    char gname[16] = "-";
    if (g.found) {
        key_name(g.on_data, g.data, g.on_plain, gname, sizeof(gname));
    }
    fprintf(stderr,
            "[keyboard] tap '%s' cap x=%.0f y=%.0f w=%.0f h=%.0f at %.0f,%.0f "
            "off %.0f,%.0f geom='%s'\n",
            name, t.x, t.y, t.w, t.h, px, py, off_x, off_y, gname);
    fflush(stderr);
    if (hold_ms > 0) {
        // A HOLD is the same gesture a finger makes: down, wait, (slide,) up. The
        // down goes through the press hook, which is what arms everything the hold
        // is for, and the release goes through the ordinary tap path.
        s->hold_px = px;
        s->hold_py = py;
        snprintf(s->slide_to, sizeof(s->slide_to), "%s", slide_to);
        z_probe_press_phase(app, Z_PRESS_DOWN, px, py);
        z_after(app, hold_ms, hold_release, s);
        return;
    }
    // Then press it the way a finger does. A TAP IS ALSO A GESTURE — down, then
    // up, then the resolve — and emitting only the resolve would make the harness
    // the one caller in the system that skips the press phases, which is exactly
    // the shape of gap that leaves a feature (the callout) untested because the
    // test cannot reach it. The two phases are back to back, so nothing has time
    // to become a hold.
    z_probe_press_phase(app, Z_PRESS_DOWN, px, py);
    z_probe_press_phase(app, Z_PRESS_UP, px, py);
    z_probe_press(app, px, py);
    fflush(stderr);
    // A gap wide enough for the rebuild a modifier triggers: shift and the layer
    // key both change what the NEXT cap is, and the next resolve reads the tree
    // the last build laid out.
    z_after(app, 250, tap_tick, s);
}

// --- key views --------------------------------------------------------------
// A key cap. Flanking the mark with Spacers centres it on the key's *main*
// (horizontal) axis — .align only governs the cross (vertical) axis, so without
// them the mark hugs the left edge.
//
// SHARE, NOT GROW. This was Grow for twenty-five phases, which means every cap
// was as wide as the letter printed on it: measured at 720x1440, 'w' came out 69
// units and 'i' 49 in the same row, and latching shift re-measured the uppercase
// glyphs and moved every key in the row sideways under the finger. Grow divides
// the SLACK left after each child is measured, so the content leaks through;
// Share drops the intrinsic and lets the weights divide the row (zelto/ui.h).
// A key cap is a grid cell that happens to have a letter in it, so the letter
// must not have a vote. Found by the P46 cap audit, which is the first thing that
// ever measured a key.
//
// Character caps are the LIGHTEST surface in the system (SURFACE_4): on a heavy
// dark material, a field of forty SURFACE_3 caps reads as one grey slab with
// hairlines in it. The things you aim at have to be the light ones, and the
// modifiers a step below them.
static ZView key_cap(ZView inner, ZAction act, float grow, ZColor bg) {
    ZView face = Shadow(Z_ELEV_1,
        Background(bg,
            CornerRadius(Z_RADIUS_CHIP,
                Frame(0.0f, (float)ZELTO_KEY_H,
                    HStack(Spacer(), inner, Spacer(),
                           .align = Z_ALIGN_CENTER)))));
    return Share(grow, act ? OnTap(act, face) : face);
}
static ZView glyph(const char *label) {
    return Weight(Z_WEIGHT_MEDIUM,
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_CALLOUT, Text("%s", label))));
}
// A key whose fill is LIGHT (the active shift) needs dark ink on it.
static ZView glyph_on(const char *label, ZColor ink) {
    return Weight(Z_WEIGHT_SEMIBOLD,
        Foreground(ink, Font(Z_FONT_SUBHEAD, Text("%s", label))));
}

// The modifier marks. These were the words "shift", "del" and "enter", which is
// what a keyboard looks like when nobody has drawn it: three English words in the
// middle of a grid of single letters, each one wider than the key it sits on and
// each one needing to be READ. They are marks now — the same three every phone
// keyboard has drawn for fifteen years — built from the toolkit's round-capped
// polyline in the unit box, so they scale with the cap and ship no bitmaps.
// The marks are drawn in the unit box, so only the frame around them carries a
// size — and that size used to be 22, chosen against a 56-unit key cap. A cap is
// ZELTO_KEY_H (Apple's 42pt) now, so the frame is a point size too: ~20pt is the
// keyboard glyph on the phone this copies. The two non-square marks keep the
// aspect they were drawn at.
#define MARK_H ((float)Z_PT(20))                 // 37
#define MARK_W_DELETE (MARK_H * 26.0f / 22.0f)   // the drawn glyph's aspect
#define MARK_W_RETURN (MARK_H * 24.0f / 22.0f)
#define MARK_STROKE ((float)Z_PT(2))             // 3 — the ink, at the glyph's scale

// The shift arrow, and — when caps lock is on — the BAR under it. The bar is not
// decoration: a one-shot shift and a caps lock produce visibly different text
// from the same-looking key, so the cap has to say which one it is set to. Every
// phone draws exactly this, and the alternative (a lit key for both) is the state
// people complain about not being able to see.
static ZView mark_shift(ZColor ink, bool locked) {
    static const float arrow[] = {0.50f, 0.12f, 0.88f, 0.50f, 0.68f, 0.50f,
                                  0.68f, 0.82f, 0.32f, 0.82f, 0.32f, 0.50f,
                                  0.12f, 0.50f};
    static const float arrow_lock[] = {0.50f, 0.08f, 0.88f, 0.46f, 0.68f, 0.46f,
                                       0.68f, 0.70f, 0.32f, 0.70f, 0.32f, 0.46f,
                                       0.12f, 0.46f};
    static const float bar[] = {0.32f, 0.86f, 0.68f, 0.86f};
    if (!locked) {
        return Frame(MARK_H, MARK_H,
            Stroke(.points = arrow, .count = 7, .thickness = MARK_STROKE,
                   .color = ink, .closed = true));
    }
    return Frame(MARK_H, MARK_H,
        ZStack(
            Frame(MARK_H, MARK_H,
                Stroke(.points = arrow_lock, .count = 7,
                       .thickness = MARK_STROKE, .color = ink, .closed = true)),
            Frame(MARK_H, MARK_H,
                Stroke(.points = bar, .count = 2, .thickness = MARK_STROKE,
                       .color = ink)),
            .align = Z_ALIGN_CENTER));
}
static ZView mark_delete(ZColor ink) {
    static const float body[] = {0.36f, 0.20f, 0.94f, 0.20f, 0.94f, 0.80f,
                                 0.36f, 0.80f, 0.06f, 0.50f};
    static const float x1[] = {0.54f, 0.37f, 0.80f, 0.63f};
    static const float x2[] = {0.80f, 0.37f, 0.54f, 0.63f};
    return Frame(MARK_W_DELETE, MARK_H,
        ZStack(
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = body, .count = 5, .thickness = MARK_STROKE,
                       .color = ink, .closed = true)),
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = x1, .count = 2, .thickness = MARK_STROKE, .color = ink)),
            Frame(MARK_W_DELETE, MARK_H,
                Stroke(.points = x2, .count = 2, .thickness = MARK_STROKE, .color = ink)),
            .align = Z_ALIGN_CENTER));
}
static ZView mark_return(ZColor ink) {
    static const float hook[] = {0.86f, 0.22f, 0.86f, 0.60f, 0.22f, 0.60f};
    static const float head[] = {0.42f, 0.42f, 0.22f, 0.60f, 0.42f, 0.78f};
    return Frame(MARK_W_RETURN, MARK_H,
        ZStack(
            Frame(MARK_W_RETURN, MARK_H,
                Stroke(.points = hook, .count = 3, .thickness = MARK_STROKE,
                       .color = ink)),
            Frame(MARK_W_RETURN, MARK_H,
                Stroke(.points = head, .count = 3, .thickness = MARK_STROKE,
                       .color = ink)),
            .align = Z_ALIGN_CENTER));
}

// A single character key (letter or symbol), tap commits it.
static ZView char_key(KbdState *s, char c) {
    char up = (kbd_upper(s) && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    char lbl[2] = {up, '\0'};
    return OnTapData(on_char, (void *)(intptr_t)c,
        key_cap(glyph(lbl), NULL, 1.0f, Z_COLOR_SURFACE_4));
}

// A half-key gutter at the end of a row (the a-s-d-f row is inset by half a key
// on both sides, so its nine keys sit UNDER the gaps of the ten above them). It
// is a share-weighted empty cap with no fill, not a Spacer inside a Frame. Share
// for the same reason the caps use it: half a key means half of what a key gets,
// which is only true if a key's width is its share of the row.
static ZView half_gutter(void) {
    return Share(0.5f, Rect(.color = z_rgba(0, 0, 0, 0)));
}

// A row of character keys from a NUL-terminated string, optionally inset by half
// a key at both ends (the home row).
static ZView char_row(KbdState *s, const char *chars, bool inset) {
    ZStackOpts row = {.spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                      .grow = 1.0f};
    int k = 0;
    if (inset) {
        row.children[k++] = half_gutter();
    }
    for (const char *p = chars; *p && k < Z_MAX_CHILDREN - 1; p++) {
        row.children[k++] = char_key(s, *p);
    }
    if (inset) {
        row.children[k++] = half_gutter();
    }
    return z_stack(Z_AXIS_HORIZONTAL, &row);
}

// A modifier key carrying a MARK. Its fill is a step below a character cap, so
// the letters — the things you are actually aiming at — stay the light ones.
static ZView mark_key(ZView mark, ZAction act, float grow, ZColor bg) {
    return key_cap(mark, act, grow, bg);
}

// A modifier key carrying a WORD (only "123" / "ABC" and "space" survive; the
// rest are marks now). Words take the smaller type step — a 20px "space" next to
// a 20px "q" makes the word look like it is shouting.
static ZView word_key(const char *label, ZAction act, float grow, ZColor bg,
                      ZColor ink) {
    return key_cap(glyph_on(label, ink), act, grow, bg);
}

static ZView keyboard_grid(KbdState *s) {
    ZColor sp = Z_COLOR_SURFACE_3;      // a modifier cap
    ZColor spi = Z_COLOR_TEXT;          // its ink
    // Shift latched: a LIGHT key with dark ink, the way a phone shows it (this is
    // the same "lit" treatment as an active Control Center toggle).
    bool lit = kbd_upper(s);
    ZColor shift_bg = lit ? Z_COLOR_PRIMARY : sp;
    ZColor shift_ink = lit ? Z_COLOR_ON_PRIMARY : spi;

    ZView row1 = char_row(s, s->symbols ? "1234567890" : "qwertyuiop", false);
    ZView row2 = char_row(s, s->symbols ? "@#$%&-+()/" : "asdfghjkl",
                          !s->symbols);

    // Row 3: shift (letters only), the last char keys, delete.
    ZStackOpts r3 = {.spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                     .grow = 1.0f};
    int k = 0;
    if (!s->symbols) {
        r3.children[k++] = mark_key(
            mark_shift(shift_ink, s->shift == SHIFT_LOCK), on_shift, 1.6f,
            shift_bg);
    }
    const char *r3c = s->symbols ? ".,?!'\";:" : "zxcvbnm";
    for (const char *p = r3c; *p; p++) {
        r3.children[k++] = char_key(s, *p);
    }
    r3.children[k++] = mark_key(mark_delete(spi), on_backspace, 1.6f, sp);
    ZView row3 = z_stack(Z_AXIS_HORIZONTAL, &r3);

    // Row 4: the layer key, space, return. The "paste" and "hide" keys are gone.
    // Paste was a keyboard key doing a TEXT FIELD's job — the field's own
    // selection menu already pastes (sdk/src/app.c field_paste_cb), which is where
    // you are looking when you want it, and where iOS puts it. "hide" was a button
    // whose whole purpose was to undo the thing you did to get the keyboard up:
    // the keyboard hides when the field blurs, and there is a home gesture for
    // leaving. Space keeps the LIGHTER character-key fill, because it IS a
    // character key.
    ZView row4 = HStack(
        word_key(s->symbols ? "ABC" : "123", on_symbols, 1.7f, sp, spi),
        word_key("space", on_space, 5.6f, Z_COLOR_SURFACE_4, Z_COLOR_TEXT),
        mark_key(mark_return(spi), on_enter, 2.0f, sp),
        .spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER, .grow = 1.0f);

    // A heavy material: the app behind shows only as a hint. See kbd_body for why
    // this one is a tint rather than a compositor blur.
    //
    // NOT Fill'd here (P48). The caller decides: the bare keyboard Fills the whole
    // surface, but with the strip up the keys go in a fixed-height Frame — and a
    // Fill node in a stack takes the WHOLE axis (layout.c arrange), overriding the
    // Frame's height and pushing the space row off the bottom. So the fill is the
    // caller's to add, once, where it means "the surface" and not "a sub-box".
    // NO .grow on the rows (P48). Grow spreads the four rows to fill whatever box
    // the VStack is given, so a box even a few units taller than KBD_H inflates the
    // row pitch and walks the space row off the bottom edge. Without it the rows
    // sit at their natural pitch (KEY_H + KEY_GAP) and their sum IS KBD_H, so the
    // keyboard is the same whether it is Filled into the whole surface or pinned to
    // a KBD_H frame under the strip.
    return Background(Z_COLOR_MATERIAL_THICK,
        VStack(row1, row2, row3, row4,
               .spacing = (float)ZELTO_KEY_GAP,
               .padding = (float)ZELTO_KEY_PAD));
}

// --- the floating layers: the accent popup and the preview callout -----------
//
// Both are absolutely placed over the grid, which a plain ZStack cannot do (it
// centres its children at their own size). The idiom is the home grid's: put the
// thing in a depth stack and OffsetXY it by (want - centred), where `centred` is
// (stack - own) / 2. Written once here, in place().
static ZView place(ZView v, float x, float y, float w, float h, float sw,
                   float sh) {
    return OffsetXY(x - (sw - w) / 2.0f, y - (sh - h) / 2.0f, Frame(w, h, v));
}

// A row of accents. SHARE, not Grow — the P46 lesson applied one layer out: with
// Grow every cell would be as wide as the mark printed on it, so "ß" and "à"
// would be different sizes in the same popup and would move as the row changed.
static ZView accent_row(const struct AccentRow *r, bool upper) {
    ZStackOpts row = {.spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                      .grow = 1.0f};
    for (int i = 0; i < r->n && i < Z_MAX_CHILDREN; i++) {
        // The DISPLAY is upper-cased when shift is held; the TAP DATA stays the
        // stored lower-case pointer, because that is the identity the release and
        // the harness slide resolve by (on_accent applies the case at commit).
        char up[4];
        const char *shown = upper ? accent_upper(r->alt[i], up) : r->alt[i];
        row.children[i] = OnTapData(on_accent, (void *)(intptr_t)r->alt[i],
            Share(1.0f,
                Background(Z_COLOR_SURFACE_4,
                    CornerRadius(Z_RADIUS_CHIP,
                        Frame(0.0f, (float)ZELTO_KEY_H,
                            HStack(Spacer(),
                                   Weight(Z_WEIGHT_MEDIUM,
                                       Foreground(Z_COLOR_TEXT,
                                           Font(Z_FONT_TITLE2,
                                                Text("%s", shown)))),
                                   Spacer(), .align = Z_ALIGN_CENTER))))));
    }
    return Shadow(Z_ELEV_3,
        Background(Z_COLOR_MATERIAL_THICK,
            CornerRadius(Z_RADIUS_WIDGET,
                Padding((float)ZELTO_KEY_PAD,
                    z_stack(Z_AXIS_HORIZONTAL, &row)))));
}

// The preview callout: the letter, floating above the key, while a finger is on
// it. Pure feedback, and the one place the adaptive targets become visible.
//
// IT SHOWS WHAT THE CLASSIFIER CHOSE, NOT WHAT IS UNDER THE FINGER, and that is
// the whole question worth asking about it. Showing the cap under the finger
// would be the honest-looking option and it would be a lie: the letter that
// appears is not the letter that will be committed, so the one moment the user
// could have caught the correction is the moment the keyboard misinforms them.
// Showing the choice makes the mechanism legible — you see 'l' pop up over a
// press that landed on 'k', and the keyboard has told you what it is about to do
// while your finger is still down and you can still slide. iOS shows the chosen
// key for the same reason.
static ZView callout(const char *ch) {
    return Shadow(Z_ELEV_3,
        Background(Z_COLOR_SURFACE_4,
            CornerRadius(Z_RADIUS_CHIP,
                HStack(Spacer(),
                       Weight(Z_WEIGHT_SEMIBOLD,
                           Foreground(Z_COLOR_TEXT,
                               Font(Z_FONT_TITLE2, Text("%s", ch)))),
                       Spacer(), .align = Z_ALIGN_CENTER))));
}

// --- the suggestion strip, drawn (P48) ---------------------------------------
// Three Share-divided slots, so each is a third of the row whatever word sits in
// it — the P46 lesson again, a slot must not be as wide as its own text. The
// middle is the literal; the pending autocorrection (slot_auto) is filled and
// tinted, the way iOS bolds the word it is about to apply. An empty slot carries
// no handler, so it is not a tappable the classifier or the probe will ever find.
static ZView suggest_slot(KbdState *s, int idx) {
    const char *txt = s->slot[idx];
    bool pending = (idx == s->slot_auto);
    ZView label =
        txt[0]
            ? Weight(pending ? Z_WEIGHT_SEMIBOLD : Z_WEIGHT_MEDIUM,
                     Foreground(pending ? Z_COLOR_PRIMARY : Z_COLOR_TEXT,
                                Font(Z_FONT_CALLOUT, Text("%s", txt))))
            : Spacer();
    ZView face =
        Frame(0.0f, (float)ZELTO_KEY_H,
              HStack(Spacer(), label, Spacer(), .align = Z_ALIGN_CENTER));
    if (pending) {
        face = Background(Z_COLOR_SURFACE_4, CornerRadius(Z_RADIUS_CHIP, face));
    }
    if (!txt[0]) {
        return Share(1.0f, face);   // an empty slot is not a control
    }
    return Share(1.0f, OnTapData(on_suggest, (void *)(intptr_t)idx, face));
}
static ZView suggest_strip(KbdState *s) {
    return Background(Z_COLOR_MATERIAL_THICK,
        Padding((float)ZELTO_KEY_PAD,
            HStack(suggest_slot(s, 0), suggest_slot(s, 1), suggest_slot(s, 2),
                   .spacing = (float)ZELTO_KEY_GAP, .align = Z_ALIGN_CENTER,
                   .grow = 1.0f)));
}

// --- body -------------------------------------------------------------------
static ZView kbd_body(ZApp *app, KbdState *s) {
    if (!s->inited) {
        s->inited = true;
        s->anim = z_animated_value(app, 0.0f);
        z_im_bind(app, on_show, on_hide, s);
        // This surface resolves its own presses (P47). Everything that makes a
        // 32pt cap typeable lives behind this one call; see kbd_resolve and
        // system/keyboard/predict.h.
        z_tap_resolver(app, kbd_resolve);
        // ...and it tracks the whole gesture, not just its end: the callout, the
        // hold that opens the accents, and the repeating delete.
        z_press_hook(app, kbd_press);
        // THE DICTIONARY IS BUILT HERE, EAGERLY, AND IT SAYS WHAT IT COST (P48).
        // The model builds itself lazily on its first question, which on a real
        // boot would be the first press — so the one press in the user's life
        // that pays for the whole word list is the first one they make, which is
        // exactly the press you do not want to be slow. Building it while the
        // surface is being created moves that cost to a moment nothing is waiting
        // on, and the number below is what makes "moved it somewhere cheap" a
        // measurement rather than an assertion.
        // ...and the learned words join the list BEFORE that report, so the
        // number it prints is the model the keyboard is actually about to use.
        dict_load(s);
        z_settings_observe(app, on_setting, s);
        char lm[256];
        z_lm_report(lm, sizeof(lm));
        fprintf(stderr, "[keyboard] lm %s\n", lm);
        fflush(stderr);
        // Headless test hook: ZELTO_KBD_SHOW=1 raises the keyboard on the first
        // build (without a real text-input focus handshake) so the QWERTY layout is
        // screenshot-verifiable. Keys go nowhere with no focused field — this is a
        // layout capture only. ZELTO_KBD_SYMBOLS=1 shows the symbols layer instead.
        const char *ks = getenv("ZELTO_KBD_SHOW");
        if (ks && ks[0] == '1') {
            s->visible = true;
            s->symbols = getenv("ZELTO_KBD_SYMBOLS") &&
                         getenv("ZELTO_KBD_SYMBOLS")[0] == '1';
            z_animated_set(s->anim, 1.0f);
        }
    }

    // AUTO-CAPITALISATION, recomputed rather than remembered.
    //
    // It is a fact about the text, so it is derived from the text every time the
    // text changes — not latched when a full stop is typed. That distinction is
    // what makes it survive everything the keyboard did not do: a paste ending in
    // "?", the app clearing the field, a caret moved back into the middle of a
    // word. All three arrive here as a new surrounding text and are answered
    // correctly without a single special case.
    //
    // auto_off is cleared on the same edge. It means "the user overrode the
    // capital HERE"; once the text moves on, here is somewhere else.
    if (predict_on(app) && z_im_autocap(app)) {
        const char *ctx = kbd_prefix(app);
        if (!s->ctx_seen || strcmp(ctx, s->last_ctx) != 0) {
            s->ctx_seen = true;
            snprintf(s->last_ctx, sizeof(s->last_ctx), "%s", ctx);
            bool want = sentence_start(ctx);
            fprintf(stderr, "[keyboard] auto-capital %s after '%s'\n",
                    want ? "on" : "off", ctx);
            fflush(stderr);
            s->auto_shift = want;
            s->auto_off = false;
        }
    } else if (s->auto_shift) {
        s->auto_shift = false;
    }

    // THE STRIP CHANGES THE HEIGHT, so the height is decided here, once, and every
    // number below reads from it. The strip is up whenever the keyboard is and the
    // field is not a password (predict_on) — a password field gets the bare keys,
    // both because there is nothing to suggest and because the strip is one more
    // surface that could leak the word. The surface RESIZES between the two heights
    // rather than always reserving the taller one, so a password field's app is not
    // shrunk by a strip it never sees.
    bool strip = s->visible && predict_on(app);
    int surf_h = strip ? ZELTO_KBD_TOTAL_H : ZELTO_KBD_H;
    z_layer_resize(app, 0, surf_h);   // width 0 = keep the anchored full width

    // Recompute the strip's slots from the word being typed (a no-op that clears
    // them in a password field). Done before the exclusive zone is set so a build
    // that turns the strip off also stops reserving its height in the same frame.
    kbd_suggest(app, s);

    // Reserve our height only while shown (app shrinks to keep the field above
    // the keyboard); catch input only while shown (else taps fall through).
    z_layer_set_exclusive_zone(app, s->visible ? surf_h : 0);
    if (s->visible) {
        z_layer_set_input_region(app, 0, 0, 0, 0);   // whole surface (input on)
    } else {
        z_layer_set_input_none(app);                 // fall through to the app
    }
    // NO z_backdrop here, deliberately. The keyboard is the one system surface a
    // blur does nothing for — it is a dense field of opaque keys, so almost none of
    // the backdrop survives to be seen — and asking for one adds a surface commit
    // per build, which perturbs the order the compositor hands out exclusive zones
    // between the two bottom-anchored bars: the keyboard would take the bottom edge
    // and shove the nav bar up into its own key rows. It gets a heavy tint instead.

    // The hold, ticked while a finger is down: accents, repeating delete.
    if (s->down) {
        kbd_hold_tick(app, s);
    }

    // Slide: v animates 0->1; parked slides the whole surface off the bottom edge.
    float v = z_animated_get(s->anim);
    float slide = (1.0f - v) * (float)surf_h;
    z_full_repaint(app);   // a big translated subtree wants a full repaint

    // The surface is the strip over the keys, or just the keys. Both children
    // carry a FIXED height — the strip its SUGGEST_H, the keys their KBD_H — and
    // they sum to surf_h exactly, so the VStack neither grows nor shrinks either
    // one. (A growing VStack over a Fill'd grid stretched the keys past the
    // surface and pushed the space row off the bottom edge — measured, not
    // guessed.) The floating popups place() against sh = surf_h below, and the
    // caps' frames the classifier reads already carry the strip's offset, so
    // nothing downstream needs to know the strip is there.
    // The strip over the keys, stacked at their NATURAL heights — no Frame, no
    // grow. Each already measures to exactly its safe-area height on its own
    // (suggest_strip is a row of KEY_H caps in KEY_PAD, = SUGGEST_H; keyboard_grid
    // is four rows in KEY_PAD, = KBD_H) and they sum to surf_h, so the VStack has
    // nothing to divide. A Frame around either would DOUBLE-COUNT its padding —
    // measure() treats a Frame's fixed height as the INNER height and adds the
    // node's own 2*KEY_PAD on top (the CLAUDE.md trap), which is exactly how the
    // space row first walked off the bottom edge.
    ZView grid = strip ? Fill(VStack(suggest_strip(s), keyboard_grid(s),
                                     .spacing = 0.0f))
                       : Fill(keyboard_grid(s));

    // The floating layers, if any. Both sit ABOVE the key they belong to, and
    // both fall BELOW it when there is no room — which is not an edge case, it is
    // the top row: 'q' and 'o' have 14 units of strip above them, and a popup
    // that clipped there would make the accents on the top row the only ones you
    // cannot see. Below is worse than above (the finger covers it) and it is the
    // only other place, so it is where they go, and the fact that they move is
    // why the position is computed from the cap's frame rather than written down.
    float sw = (float)z_screen_width(app);
    float sh = (float)surf_h;
    if (s->accents) {
        const struct AccentRow *r = accents_for(s->accent_base);
        if (r) {
            float cw = s->base_w;
            float pw = r->n * cw + (r->n - 1) * (float)ZELTO_KEY_GAP +
                       2.0f * (float)ZELTO_KEY_PAD;
            float ph = (float)ZELTO_KEY_H + 2.0f * (float)ZELTO_KEY_PAD;
            float x = s->base_x + s->base_w / 2.0f - pw / 2.0f;
            if (x < 0.0f) { x = 0.0f; }
            if (x + pw > sw) { x = sw - pw; }
            float y = s->base_y - ph - (float)ZELTO_KEY_GAP;
            if (y < 0.0f) {
                y = s->base_y + s->base_h + (float)ZELTO_KEY_GAP;
            }
            grid = ZStack(grid,
                          place(accent_row(r, kbd_upper(s)), x, y, pw, ph, sw, sh),
                          .align = Z_ALIGN_CENTER);
        }
    } else if (s->preview && s->down) {
        float cw = s->preview_w * 1.35f;
        float ch = (float)ZELTO_KEY_H;
        float x = s->preview_x + s->preview_w / 2.0f - cw / 2.0f;
        if (x < 0.0f) { x = 0.0f; }
        if (x + cw > sw) { x = sw - cw; }
        float y = s->preview_y - ch - (float)ZELTO_KEY_GAP;
        if (y < 0.0f) {
            y = s->preview_y + s->preview_h + (float)ZELTO_KEY_GAP;
        }
        grid = ZStack(grid, place(callout(s->preview_ch), x, y, cw, ch, sw, sh),
                      .align = Z_ALIGN_CENTER);
    }
    return Offset(NULL, slide, grid);
}

// TOP layer, bottom-anchored, full width. Created at KBD_TOTAL_H (keys + strip)
// and resized down to KBD_H when the strip is off (a password field); exclusive
// zone is toggled at runtime (0 hidden / surf_h shown). keyboard=false: the
// on-screen keyboard takes NO wl_keyboard focus — it drives text via
// input-method-v2.
Z_LAYER_APP(KbdState, kbd_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .height = ZELTO_KBD_TOTAL_H,
            .keyboard = false,
            // A cap's letter is a label for a FINGER, not something to read, and
            // the cap is sized as a touch target (ZELTO_KEY_H = 42pt). Scaling
            // the type here grows the letters inside caps built for the shipped
            // size, and at the largest step 'space' wants 102x49 in a 73x35 cap.
            // No phone scales its keyboard with Dynamic Type either.
            .fixed_type = true)
