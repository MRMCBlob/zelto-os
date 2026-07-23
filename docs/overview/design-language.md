# Design Language

Zelto OS has its own visual identity: the **structural clarity** of Material with the
**polish and motion** of iOS. This page defines the design tokens and motion principles
that the System UI and `libzelto` defaults follow. Apps inherit these automatically and
can override them per-theme.

## Principles

1. **Content first.** Chrome is quiet; content gets contrast and space.
2. **Soft depth.** Layering via blur, subtle shadow, and rounded corners — not hard
   borders.
3. **Physical motion.** Transitions use springs, not linear fades. Things move like they
   have mass.
4. **One accent.** A single dynamic accent color drives interactive elements; everything
   else is neutral.
5. **Legible by default.** Type scale and spacing are generous; tap targets ≥ 44 px.

## Design tokens

Tokens are the single source of truth. They resolve to concrete values per theme
(light/dark) and per accent. Apps read them via the theme API
([../guides/styling-theming.md](../guides/styling-theming.md)).

### Color

| Token | Role |
|---|---|
| `color.bg` | Window background |
| `color.surface` | Cards, sheets, bars |
| `color.surface.elevated` | Raised surfaces (menus, dialogs) |
| `color.text` | Primary text |
| `color.text.secondary` | Secondary text |
| `color.accent` | Interactive / selected |
| `color.separator` | Hairline dividers |
| `color.scrim` | Modal dimming |

Dark mode and the accent color are dynamic; never hard-code hex values — read tokens.

### Spacing

A 4 px base scale: `space.1`=4, `space.2`=8, `space.3`=12, `space.4`=16, `space.6`=24,
`space.8`=32. Default screen padding is `space.4` (16).

### Radius

`radius.sm`=8, `radius.md`=14, `radius.lg`=22, `radius.full`=9999. Cards default to
`radius.md`; sheets to `radius.lg` on the top corners.

### Type

**The scale is written in POINTS; the screen is in PIXELS.** The handset output is
720×1440 raw pixels against a 390pt design reference, so one point is ~1.85 screen units.
The C toolkit spells that conversion out once, in `Z_TYPE()`
([../api-reference/c/ui.md](../api-reference/c/ui.md)); the "px" column below is what the
renderer actually receives. Every *layout* metric — frames, insets, icon sizes — is
written directly in screen units, so a bare number next to a type step is not the same
kind of number. Name the step; never write a pixel size into `Font()`.

| Token | Use | Points | Screen px |
|---|---|---|---|
| `font.display` | The lock clock | 92 | 170 |
| `font.largeTitle` | Screen titles | 40 / bold | 74 |
| `font.title` | Section titles | 28 / semibold | 51 |
| `font.title2` | Sub-sections | 24 | 44 |
| `font.callout` | Emphasised body | 20 | 37 |
| `font.body` | Body text | 17 / regular | 31 |
| `font.headline` | Body + semibold | 17 | 31 |
| `font.subhead` | Dense body | 15 | 27 |
| `font.footnote` | Secondary caption | 13 | 24 |
| `font.caption` | Labels | 12 | 22 |
| `font.caption2` | Dense metadata | 11 | 20 |

Emphasis is a separate axis: pair a step with a weight rather than reaching for the step
above it (which is why `headline` is body-sized). Text is shaped with HarfBuzz +
FreeType; the default family is the Zelto system font (Satoshi, a variable face, so
weights are real rather than synthesised).

**The table above is the DEFAULT size, not the only one.** The user's text size
(`sys.text_size`, Settings ▸ Accessibility) shifts the whole ladder, and it does so at a
single seam — `z_font_units()` — so a surface that names a step gets the user's size
without knowing one exists. Never cache a step's pixel value; ask for the step.

The ladder shifts by a constant **point offset** per size step, not by a multiplier:

| Step | 0 | 1 | 2 | 3 (default) | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| Offset (pt) | −3 | −2 | −1 | 0 | +2 | +4 | +6 |
| `font.body` (px) | 26 | 27 | 29 | 31 | 35 | 38 | 42 |
| `font.largeTitle` (px) | 68 | 70 | 72 | 74 | 77 | 81 | 85 |

An additive rule is a large ratio on a small number and a small one on a large number,
which is why the small steps grow more (`caption2` ×1.55 at the top of the range against
`largeTitle` ×1.18) — the same behaviour Apple's Dynamic Type table has, falling out of
one rule instead of a twelve-row lookup. Two exceptions, both deliberate:

- **`font.display` never scales.** It is the lock clock — a number sized against the
  screen's height, not a body of prose.
- **The small end floors at `caption2`'s own size** rather than passing under itself.

**Three surfaces opt out entirely** (`ZLayerOpts.fixed_type`): the **status bar** and the
**home indicator**, whose heights are exclusive-zone contracts other processes offset by,
and the **keyboard**, whose caps are touch targets sized in points — a letter on a cap is
a label for a finger, not something you read, and growing the caps walks the bottom row
off the surface. Safe areas are physical and do not scale either.

**Bold Text** (`sys.bold_text`) is the same setting's other term: it raises the floor on
weight (Regular → Semibold) where the shaper is configured, so a measurement and a paint
can never disagree about it.

**The range is the seven standard sizes.** iOS's five accessibility sizes (body to 53pt)
are not shipped: at those sizes a label and its control stop fitting on one row, so every
row in the OS has to reflow to a vertical layout — a different design for the same
screen, not a taller row.

**A fixed height that holds text is a bug waiting for a large text size.** `Z_ROW_H`
(44pt) is a touch-target FLOOR, not a row height; use `z_row_h()`, and derive any other
box that holds a line from `z_line_height()` rather than from a spec sheet.
`ZELTO_PROBE_TAPS=1` reports every string that needs more room than its box, on both
axes, which is how the surfaces above were found rather than guessed at.

### Elevation

`elevation.0` (flat) … `elevation.3` (dialog). Elevation combines a soft shadow with an
optional background blur on translucent surfaces (bars, sheets).

## Motion

- **Standard spring:** `stiffness 220, damping 26` — default for layout, sheets, pushes.
- **Snappy spring:** `stiffness 320, damping 30` — toggles, small controls.
- **Durations** are derived from the spring, not fixed; avoid linear easing for movement.
- **Shared-element transitions** are supported by the navigator (see
  [../guides/navigation.md](../guides/navigation.md)).

See [../guides/animation.md](../guides/animation.md) for the animation API.

## Iconography

Icons are a single-weight line set on a 24 px grid, with a 2 px stroke at default size.
Company/brand logos used inside apps must come from real brand assets, never redrawn.

## Applying the design language

- **System UI** ships these tokens as the global theme.
- **Apps** get them for free through `libzelto` defaults and may define an app theme that
  overrides tokens — see [../guides/styling-theming.md](../guides/styling-theming.md).
