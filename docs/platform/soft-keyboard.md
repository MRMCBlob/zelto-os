# On-screen keyboard & text input

Zelto is a touch device, so text is entered through a system **on-screen
keyboard** (`zelto-keyboard`). It appears automatically when a text field is
focused, types into whatever app owns that field, and slides away when the field
blurs — the app needs no keyboard-specific code.

## How it works: text-input ↔ input-method

Two standard Wayland protocols meet in the compositor:

| Protocol | Who speaks it | Role |
|---|---|---|
| **text-input-v3** | every app with a text field (via the SDK `TextField`) | enables on focus, ships surrounding text, receives committed strings |
| **input-method-v2** | `zelto-keyboard` (the single input method) | told activate/deactivate (→ show/hide), sends `commit_string` / `delete_surrounding_text` |

The compositor (`compositor/src/text_input.c`) bridges them:

```
   app TextField                compositor relay                on-screen keyboard
   ─────────────                ────────────────                ──────────────────
   tap field ─ enable ────────► focused + enabled ─ activate ─► show (slide up)
                                                              ◄─ commit_string "h"
                ◄─ commit_string "h" ◄──── relay ─────────────
   append 'h'
   blur / app bg ─ disable ───► deactivate ───────────────────► hide (slide down)
```

Focus follows the seat's keyboard focus: when a surface gains keyboard focus the
relay sends the app's text input `enter`; losing it sends `leave` (and hides the
keyboard). So the keyboard shows for whichever app is in front and has a focused
field, with no per-app cooperation.

### Why not virtual-keyboard-v1?

`virtual-keyboard-v1` (a keyboard that injects raw keysyms) is the **documented
fallback**, not the primary path. It works with any app that reads `wl_keyboard`
even if the app has no text-input support, but it is "dumb": it cannot do smart
insertion (IME composition, surrounding-text edits) and, crucially, it carries
**no activate/deactivate**, so a virtual keyboard has no way to know when to
auto-show/hide. The authentic, verifiable choice for a phone keyboard that pops up
on focus is therefore **text-input-v3 / input-method-v2**. (The XML is vendored
under `protocols/` for a future fallback build; it is not wired in the MVP.)

## Show / hide, and reserving space

`zelto-keyboard` is a bottom-anchored **TOP-layer** layer-shell surface, always
running but only *shown* while a field is focused:

- **Hidden**: reserves **no exclusive zone**, catches **no input** (taps fall
  through to the app / nav bar), and slides its whole grid off the bottom edge (a
  vertical `Offset` driven by an animated value, recomputed each frame).
- **Shown**: reserves its height as a **real exclusive zone**, so the compositor
  shrinks the focused app and its field stays visible **above** the keyboard (no
  per-app panning). It catches input and slides up.

The keyboard's own **hide** key just parks it locally; the field keeps focus and
its text.

## Layering & the lock screen

The keyboard is in the **TOP** layer (like the status/nav bars). The pull-down
shade and the lock screen are **OVERLAY**, so they always composite *above* the
keyboard. A **locked** screen therefore shows its own passcode keypad, never an
app's keyboard — and in any case the lock grabs keyboard focus, which drops the
app's text-input focus and deactivates the keyboard. The lock keeps its bespoke
4-digit keypad (it must work with no app in focus); it does not use the soft
keyboard.

## Adaptive touch targets

A character cap is **31.5–32.1pt** wide by 41.6pt tall, and it cannot be wider:
ten keys across a 390pt phone is 38.9pt a key *with no gaps at all*. iOS's caps
are the same size for the same reason, and the 44pt guideline is not what its
keyboard obeys. What makes a cap that size typeable is not its width — it is that
**the touch targets are not the caps**.

The art never moves. The regions do, on every keystroke. After `hel` the region
that yields `l` is much larger than the region that yields `k` beside it, in
proportion to how likely each letter is. Implemented as a **classifier**, not as
resized rectangles (grown rectangles overlap, tie and leave gaps):

```
score(key) = P(touch | key) × P(key | prefix)          argmax wins
             ^ Gaussian on the distance   ^ the language model
               from the key's centre,       (system/keyboard/lm_trie.c)
               σ = 0.35 × the cap's size
```

Three consequences:

- **The dead gutter is gone.** The 11-unit gap between caps paints no key and
  used to *hit* no key — ~15% of every row did nothing. The classifier never asks
  which rectangle contains the point, so every point on the strip resolves to
  something. The cap audit prints `gutter` (still ~15%, unchanged) and `dead`
  (now 0) separately.
- **A dead-centre press is always that key.** No prefix can override the inner
  half of a cap, or the keyboard could not type a password, a name, or any word
  the model has not seen.
- **Password fields turn it off.** A secure `ZTextField` declares
  `CONTENT_PURPOSE_PASSWORD` on text-input-v3; the compositor relays it; the
  keyboard reads it with `z_im_purpose()` and falls back to geometry.

The model sits behind a seam (`system/keyboard/predict.h`) and says which one it
is: `z_lm_name()` is **`trie+bigram`**. It is a prefix tree over a shipped word
list (`words_en.h`, 1620 words, ~85KB built) backed off to the letter-pair table
(`lm_bigram.c`) the moment a walk leaves the tree — because a trie assigns
probability **zero** outside its list, and zero is not "unlikely", it is
"untypeable off-centre" for every name and password.

Two things the dictionary buys that a letter-pair table provably could not:

- **It sharpens with a longer prefix.** A bigram reads the last letter and throws
  the rest away, so `e` and `hel` are the same question to it. P(`p` | `hel`) is
  0.67 to the trie and 0.007 to the bigram.
- **The space bar grows on a complete word.** The space cap carries a
  `Z_LM_BOUNDARY` symbol competing in the *same* argmax, so P(space | `hello`) is
  0.88 and P(space | `hel`) is 0.03 — the target reaches 30 units above the bar's
  top edge after a word and 20 after a fragment. It is the one adaptive-target
  behaviour a user actually notices.

None of `Z_KBD_SIGMA`, `Z_KBD_CENTRE` or `Z_KBD_ODDS` changed when the dictionary
landed, and that is worth saying: the centre-zone guarantee is an inequality over
those constants and the key pitch alone, with no term from the model, so a
sharper model cannot break it.

The prefix comes from **text-input-v3 surrounding text**, relayed since P21 and
read since P47 — not from an echo of the keyboard's own keystrokes.

## Corrections: a different mechanism

Conflating the classifier with autocorrect is the trap, so they are separated by
an interface and not only by a comment:

|  | classifier | autocorrect |
|---|---|---|
| corrects | a **press**, before it commits | a **word**, already in the field |
| reads | the prefix | the whole word |
| when | every press | at a word boundary |
| visible? | no | yes — and sometimes wrong |
| undo | not needed | one backspace |

Autocorrect drives a three-slot **suggestion strip** above the keys (the surface
is `ZELTO_KBD_TOTAL_H`, and resizes down to `ZELTO_KBD_H` for a password field,
which gets no strip at all). Slot 1 is always the literal text; slot 0 is the
pending correction, tinted the way iOS bolds the word it is about to apply.
Tapping the middle slot is how you reject it. **Revert lives in surrounding text
plus one keystroke of keyboard state**: the field holds the corrected word, the
keyboard holds the original for exactly one key, and the next backspace restores
it.

A **word boundary is the end of a word, not a space**. Space, return, and every
punctuation cap on the symbols layer all end one — the rule is the same one that
finds the current word from the other side, so anything that is not a letter is a
boundary (except the apostrophe, which lives *inside* English words, and digits,
which say "this is not prose"). The double-space period runs first, because it
rewrites the text a correction would have read.

Two lexical rules ride on the same visible path:

- **Contractions.** A trie over a-z cannot hold `don't`, so the list carries the
  bare form — which is also what people type, the apostrophe being on the symbols
  layer. `CONTRACTIONS_EN` puts it back. The rule for what may be in that table is
  the whole safety argument: **only contractions whose bare form is not itself an
  English word.** `its`, `were` and `lets` are words and stay bare.
- **Inflections.** The list ships bases, not inflections, and a suffix rule
  (`inflected_word`) is what stops that from being destructive. Without it,
  measured over 60 real inflected forms, autocorrect **rewrote 49 of them**:
  `walked` → `walk`, `hands` → `and`, `dogs` → `does`, `reading` → `wedding`. The
  rule lives *inside* `z_lm_is_word`, so there is still exactly one function every
  caller asks.

## The learned dictionary

The keyboard learns words you type that its shipped list does not carry. It is a
privacy decision at least as much as a technical one, and all three answers are
written beside the code that implements them (`kbd_learn` in
`system/keyboard/main.c`):

- **What.** Lowercase a-z, two characters or more, not already known, and only
  after you have shown it was not a typo. Two signals: **reverting** an
  autocorrection learns the word immediately (a person disagreeing with the model
  is the strongest signal there is), and surviving **three** word boundaries
  uncorrected learns it too. A word typed once is a typo.
- **Where.** One newline-separated file in the keyboard's private directory under
  `/var/zelto` — the same shape as `words_en.h`, because a store you cannot read
  is a log. A learned word merges into the **list the trie is built from**, so it
  is answered by the same `z_lm_p` / `z_lm_is_word` / `z_lm_candidates` as a
  shipped one. There is no second model to disagree with the first.
- **What never gets in.** Nothing from a password field — the string never reaches
  the in-RAM counter, let alone the file. Nothing that is not a word: the current
  word stops at the first non-letter, so emails, keys and postcodes are not words
  to any of this. **The counts themselves are never written to disk** — only words
  that reached the threshold are, because a file of "strings this person typed
  once" is exactly the log this must not become.
- **The way out.** *Settings → Keyboard* shows how many words have been learned
  and offers **Clear Learned Words**, which forgets them, blanks the bytes and
  deletes the file. The words themselves are deliberately not listed: a screen
  showing what somebody typed is a shoulder-surfing surface of its own, and the
  count plus the delete answers the question without building one.

A learned word gets the weight of roughly the middle of the shipped list, not
rank 0 — a word you typed three times is common *for you*, not commoner than
"the".

## Holding a key

A tap handler hears about a press once, when it is over — enough for a button,
not for a keyboard. `z_press_hook` delivers the whole gesture (DOWN / MOVE / UP)
and three behaviours ride on it:

- **Long-press accents.** Hold a letter for 0.5s and its marks appear in a
  `Share`-divided row *above* the key — or **below** it when there is no room,
  which is the whole top row (`q`…`p` have 14 units of strip above them). Slide
  onto one and release. Releasing anywhere else commits **nothing**: a hold you
  did not follow through has to be cancellable, and the alternatives (nearest
  accent, or fall back to the base letter) mean an accidental pause silently
  changes what you typed. Lower-case marks only.
- **Backspace repeat.** 0.15s cadence, accelerating to 0.06s after 1.2s, then
  **whole words** after 2.0s — sent as one `delete_surrounding_text(N)`, not N
  backspaces, so the keyboard is not racing the surrounding-text update it counts
  the word from. Cadence is a function of **time held**, not of repetitions, so a
  nearly-empty field does not delete at a different speed from a full one.
- **Key preview callout.** The letter floats above the finger while pressed. It
  shows **what the classifier chose, not what is under the finger** — showing the
  cap would misinform the user at the one moment they can still slide and fix it,
  and it is the only place the adaptive targets are visible to a person at all.

`OnLongPress` (P16) is not enough for any of these: it fires once, on a node, and
says nothing about the finger before or after — and the accent release lands on a
control that did not exist when the press began.

## Text rules

Three behaviours people expect, all of which are rules about the **text** rather
than state machines on keys — so all three are answered from text-input-v3's
surrounding text, not from an echo of what the keyboard committed (an echo is
wrong the moment a paste, a caret move or an app clearing the field happens):

| Rule | Condition | Gated on |
|---|---|---|
| **double-space period** | the text ends `<alnum><space>` and you press space | not a password field |
| **autocorrect** | a word boundary ends a word that is not in the dictionary, is not a live prefix of one, and has a clearly better candidate | not a password field |
| **auto-capitalise** | the text is empty, ends `.`/`?`/`!` + space(s), or ends in a newline | the field's `autocap` hint |
| **caps lock** | two shift presses inside 0.35s | — |

Auto-capitalisation is **derived, not latched** — recomputed whenever the field's
contents change, which is what makes it right after a paste ending in "?" and
what turns it off again mid-word. Pressing shift while it is on overrides it for
that position only.

Caps lock is a third shift state, not a second flag, and the cap says which:
the arrow gains a bar under it. A one-shot shift and a lock produce different
text from an identical-looking press, so the mark has to distinguish them.

Its one cost is **latency**: the app re-declares surrounding text on the build
after a commit, so two presses closer together than a frame round-trip see the
same context.

## App API

```c
// In app state: a retained buffer the widget edits.
ZTextField note;   // { char text[256]; int len; secure; on_change; }
note.secure = true;   // renders bullets AND disables the keyboard's language model

// In body(): tapping this focuses it and raises the keyboard.
TextField(app, &state->note, "Type a note...");
// ... read state->note.text anywhere.
```

The keyboard app itself uses the input-method side:

```c
z_im_bind(app, on_show, on_hide, state);  // become the seat's input method
z_im_commit_text(app, "a");               // insert a character
z_im_backspace(app);                       // delete one char before the cursor
z_im_purpose(app);                        // NORMAL / PASSWORD / OTHER
z_im_surrounding(app, &cursor);           // the field's text, as its app reports it
z_tap_resolver(app, kbd_resolve);         // this surface resolves its own presses
```

See `docs/api-reference/c/ui.md`, `compositor/src/text_input.c`, and
`system/keyboard/main.c`.
