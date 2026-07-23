# Accessibility

What Zelto OS does for a person who cannot read the default type, and what it
does not do yet. Three preferences ship, all on **Settings ▸ Accessibility**, all
brokered as `sys.*` keys and honoured by every process without an app having to
opt in.

| Setting | Key | What it moves |
| --- | --- | --- |
| Text Size | `sys.text_size` | The whole type scale, 12 steps (P50, P51) |
| Bold Text | `sys.bold_text` | A floor on font weight where the weight reaches the shaper (P50) |
| Increase Contrast | `sys.increase_contrast` | Three design tokens (P51) |
| Reduce Motion | `sys.reduce_motion` | Every spring collapses to an instant change (P31) |

## Text Size, and the reflow

The scale is twelve steps: the seven standard sizes (xSmall … xxxLarge) plus the
five accessibility sizes AX1 … AX5. `Z_TEXT_SIZE_DEFAULT` is 3, so an
unconfigured device draws exactly what it drew before Dynamic Type existed.

A `ZFont` is a semantic *step*, not a size. Exactly one function turns a step
into the units the shaper sees — `z_font_units()` — and everything routes through
it (the node default, `Font()`, `WrapText`, `EllipsizeText`, `z_line_height`), so
no surface in the OS knows the setting exists. The ladder is an **additive point
offset**, not a multiplier; the reasoning and the table are in `<zelto/ui.h>`.

Above `Z_TEXT_SIZE_REFLOW_FIRST` (step 8, AX2) rows **reflow**: a label wraps to
the card's text column and its control moves onto its own line underneath. That
is one predicate — `z_text_size_reflows()` — asked by every surface that has a
choice to make, rather than a threshold each surface reimplements.

### Where the break came from

It was measured, not chosen. Settings ▸ Lock Screen — the densest rows in the OS —
was booted at every step with `ZELTO_PROBE_TAPS=1`:

| step | widest stepper row |
| --- | --- |
| AX1 (7) | the pill's `+` key ends at x=671 on a 720 screen — fits |
| AX2 (8) | the same key starts at x=701 and ends at 753 — **gone** |

A control 33 units off the edge of the screen cannot be pressed, and nothing on
the screen says it is there. Everything below AX2 survives as a *taller* row,
which is what `z_row_h()` is for.

### What reflows, and what cannot

| Surface | At AX sizes |
| --- | --- |
| Settings rows | Label wraps; control moves under it, leading-aligned |
| Settings screen title | `WrapText` — "Display & Sound" measures 798 units at AX5 |
| Home widgets | A 2×1 bento cell becomes full-width (`entry_span`) |
| Home app icons | Unchanged — a picture with an ellipsizing label under it |
| The keyboard | **Does not scale at all.** A cap is a touch target sized in points |
| Status bar, home indicator | **Do not scale.** Their heights are exclusive-zone contracts |

The three opt-outs are a declaration (`ZLayerOpts.fixed_type` /
`z_text_scaling_disable()`), not an oversight; the reasoning is in `<zelto/ui.h>`
and the Accessibility screen's own footer tells the user.

### Where the range stops

It does not stop. Every one of the eight surfaces the audit covers
(`test/test_text_size_overflow_sim.sh`) reports zero text wider than its box,
zero text taller than its box, and no growth in the sideways count at AX5. What
AX5 costs is **screenfuls**: Settings ▸ Lock Screen is six rows at the default
size and rather more than one screen at AX5, so the scroll does more work. That
is the correct trade — a row you have to scroll to is a row you can read.

## Increase Contrast

The palette was never measured. `<zelto/gfx.h>` carried the comment "Text (on the
surface tones — all AA)" and it was not true:

| ink | BG | SURFACE | SURFACE_2 | SURFACE_3 |
| --- | --- | --- | --- | --- |
| `TEXT` | 18.82 | 15.25 | 12.49 | 10.17 |
| `TEXT_MUTED` | 8.18 | 6.63 | 5.43 | **4.42** |
| `TEXT_FAINT` | 4.02 | 3.25 | **2.67** | **2.17** |

WCAG 2.1 AA is 4.5:1 for body text and 3:1 for large text. `TEXT_FAINT` is the
disclosure chevron on every Settings row and the disabled detail column, and on a
card it does not reach even the large-text bar.

Increase Contrast gives three tokens a second value — the *least* brightening
that clears AA on the darkest surface each is drawn on, so the ink hierarchy
survives. The seam is the same shape as `z_font_units()`: the macros already
expanded to a function call, so all 121 migrated call sites keep working and none
of them learns a setting exists. `test/test_contrast_tokens.c` computes the
ratios from the tokens themselves, so the table above cannot drift.

Two failures the table found that this setting deliberately does **not** fix:
`TEXT` on `WARN` is 1.96:1 and `ON_PRIMARY` on `SUCCESS` is 3.71:1. Those want
the *fill* redrawn, which is a palette change rather than a preference.

## Is the probe an accessibility tree?

`ZELTO_PROBE_TAPS=1` walks the retained view tree on every settled build and
reports, per process: every tappable with its handler and its frame, every string
with its box, what is off the surface, and what needs more room than it was
given. That is the entire *content* of an accessibility tree — labels, roles, hit
rects — computed on a real layout and then written to stderr.

**The walk is the right foundation. The protocol is not started, and three things
stand between them.** None is a rewrite, and each has a piece of the answer
already in the tree:

**1. A dump is a snapshot; a client needs a live tree.** The probe fires once, on
the first settled build, behind an env var. A screen reader has to be told when
the tree changes and what changed — and the toolkit already computes exactly
that. `sdk/src/reconcile.c` diffs each build against the previous one to decide
which pixels to repaint; the same walk, driven from the same place, is a change
*event* rather than a second traversal. The work is the subscription and the
lifetime, not the diff.

**2. A handler pointer is meaningless across a process boundary.** The probe
names a control by the function pointer of its handler, which is how
`z_probe_tap()` can dispatch without coordinates — and which is an address in
*this* process's text segment. An external client needs a stable id. Two partial
answers exist and neither is enough on its own: `ZELTO_TAP_LABEL` resolves by
string (not unique — the Accessibility screen has two controls whose only text is
"A"), and `z_animated_keyed()` proves the tree can carry caller-supplied stable
keys (optional, so most nodes have none). What is needed is an id derived from
the build path, which the reconciler already walks.

**3. A `Text` node is not a label.** The launcher's app name and its badge count
are two `Text` nodes in one cell, and a reader has to say them as one thing. The
probe has a first attempt at this — `probe_label()` takes the *longest* string
under a tappable — and it is on its second revision already, because the first
one printed a home tile's placeholder initial ('G') instead of its name
('Greeter'). Grouping is the genuinely unsolved part: "longest string wins" is a
heuristic, and an accessibility tree needs the node that *owns* its strings to
say so.

Until those three are answered, the walk is a debugging instrument that happens
to compute the right thing. It is worth keeping in that shape: everything it
learns is learned from a real layout of a real build, which is the property an
accessibility tree bolted on afterwards never has.

## Not shipped

The OS is **LTR-only**. `Text` measures with HarfBuzz, which shapes Arabic and
Hebrew correctly today, but every layout in the toolkit says *leading* where it
means *left* and nothing mirrors.
