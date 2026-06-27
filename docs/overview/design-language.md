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

| Token | Use | Default |
|---|---|---|
| `font.largeTitle` | Screen titles | 34 / bold |
| `font.title` | Section titles | 22 / semibold |
| `font.body` | Body text | 17 / regular |
| `font.callout` | Secondary | 15 / regular |
| `font.caption` | Labels | 13 / regular |

Text is shaped with HarfBuzz + FreeType; the default family is the Zelto system font.

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
