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

## App API

```c
// In app state: a retained buffer the widget edits.
ZTextField note;   // { char text[256]; int len; on_change; }

// In body(): tapping this focuses it and raises the keyboard.
TextField(app, &state->note, "Type a note...");
// ... read state->note.text anywhere.
```

The keyboard app itself uses the input-method side:

```c
z_im_bind(app, on_show, on_hide, state);  // become the seat's input method
z_im_commit_text(app, "a");               // insert a character
z_im_backspace(app);                       // delete one char before the cursor
```

See `docs/api-reference/c/ui.md`, `compositor/src/text_input.c`, and
`system/keyboard/main.c`.
