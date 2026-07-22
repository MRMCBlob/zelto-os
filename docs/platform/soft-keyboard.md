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
               from the key's centre,       (system/keyboard/lm_bigram.c)
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
is. Today it is a compiled-in letter-pair table, which knows that `l` often
follows `e` but has no idea the word `hello` exists — so it cannot grow the space
bar when the prefix is a complete word. A prefix tree over a shipped word list
replaces `lm_bigram.c` alone.

The prefix comes from **text-input-v3 surrounding text**, relayed since P21 and
read since P47 — not from an echo of the keyboard's own keystrokes, which would
be wrong the moment anything else edited the field.

## Text rules

Three behaviours people expect, all of which are rules about the **text** rather
than state machines on keys — so all three are answered from text-input-v3's
surrounding text, not from an echo of what the keyboard committed (an echo is
wrong the moment a paste, a caret move or an app clearing the field happens):

| Rule | Condition | Gated on |
|---|---|---|
| **double-space period** | the text ends `<alnum><space>` and you press space | not a password field |
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
