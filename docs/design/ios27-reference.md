# iOS design reference — the P52 polish dossier

The measured spec the P52 polish pass cites. A change with no number in this file
behind it does not get made. This is Stage 0: no code changes, only numbers.

## How to read this file

- **The unit.** Zelto's surface coordinate is a raw device pixel on a 720×1440
  screen; the design reference is a 390pt handset, so **1pt = 1.85 units** (720/390
  = 1.846, `Z_PT` in `<zelto/ui.h>`). Apple numbers are in POINTS and are converted
  ×1.85 (rounded to int). A number derived from the screen (a fraction of the width,
  a cell dividing a grid) is already in units and is NOT converted.
- **Every row pairs a number with its delta.** `Apple (pt) | Apple (units) | Zelto
  (units) | Zelto (pt) | delta | note`. "Different" is not "better" — a delta is a
  candidate to investigate, and the note says whether closing it is worth it.
- **This is iOS 26, not 27.** All three research passes independently found that **no
  reputable source publishes any iOS 27 numeric spec** (as of 2026-07). The community
  "iOS 27" Figma is speculative and carries no measured type/spacing/radius/control
  values. So the file is named `ios27-reference.md` (the phase's chosen path) but its
  values are the **iOS 26 solid floor**. Do not encode any iOS 27 number — none exists.
- **Apple publishes far less than you'd think.** Materials (blur alphas), vibrancy,
  Liquid-Glass optics, system shadows, most control corner radii, button heights, the
  switch knob, the sheet grabber — **none are in Apple's public docs.** They are
  runtime-adaptive or community-measured. Where a value is unpublished it is marked
  **[unpub]** and the number is a measured convention with a source, not a spec. This
  matters: for those tokens "match iOS" is impossible — Zelto **owns** the constant.

---

## 1. Type ramp — SF Pro, Dynamic Type "Large" (default)

Zelto's base ramp is closer to iOS than the "+6pt" note suggested — that offset is the
*Dynamic Type scaling direction* (Large→xxxLarge), not the base ramp. The base ramp
mostly lands on iOS; three steps drift.

| Style | Apple pt | Apple u | Zelto u | Zelto pt | delta (u) | note |
|---|---|---|---|---|---|---|
| Large Title | 34 | 63 | 74 (`LARGE_TITLE`) | 40 | **+11** | Zelto hero runs +6pt big |
| Title 1 | 28 | 52 | 51 (`TITLE`) | 28 | −1 | match |
| Title 2 | 22 | 41 | 44 (`TITLE2`) | 24 | **+3** | +2pt |
| Title 3 | 20 | 37 | — | — | — | **iOS step Zelto lacks**; `CALLOUT` fills the slot |
| Headline | 17 (Semibold) | 31 | 31 (`HEADLINE`+SB) | 17 | 0 | match |
| Body | 17 | 31 | 31 (`BODY`) | 17 | 0 | match |
| Callout | 16 | 30 | 37 (`CALLOUT`) | 20 | **+7** | Zelto's Callout is really iOS **Title3** (20pt) |
| Subheadline | 15 | 28 | 27 (`SUBHEAD`) | 15 | −1 | match |
| Footnote | 13 | 24 | 24 (`FOOTNOTE`) | 13 | 0 | match |
| Caption 1 | 12 | 22 | 22 (`CAPTION`) | 12 | 0 | match |
| Caption 2 | 11 | 20 | 20 (`CAPTION2`) | 11 | 0 | match |

**Default weights:** Apple's system default is **Regular** for every title (design comps
render them Bold — a convention, not the OS). **Headline (Semibold) is the only
non-Regular default.** Zelto matches this (`HEADLINE` = Body-sized + `Weight(SEMIBOLD)`).

**Tracking (letter-spacing) — NOT modeled in Zelto (finding).** iOS auto-tracks: negative
(tight) on large text, crossing zero at Subheadline (15pt), slightly positive on small.
Per-style, in units it rounds to:

| Style | Apple tracking (pt) | ≈ units |
|---|---|---|
| Large Title | −1.05 | −2 |
| Title 1 | −0.80 | −1 |
| Title 2 | −0.70 | −1 |
| Headline / Body | −0.43 | −1 |
| Callout | −0.32 | −1 |
| Subheadline | 0.00 | 0 |
| Footnote → Caption2 | +0.03 … +0.15 | 0 |

Full per-size table (1/1000 em, `tracking_pt = value/1000 × size`): 12pt→0, 13→−6,
15→−16, 16→−20, 17→−24, 18→−25; going positive below 12pt (11→+6, 10→+12).
**Verdict for Stage 1:** the Title-level tightening (−1 to −2u) is visible and cheap;
below ~16pt tracking rounds to 0u and is negligible. Decide in Stage 1 whether to add a
`.tracking` axis or record it deliberately out of scope — either is defensible, but the
*absence* must be a written decision, not an oversight.

**Leading:** a reference to VALIDATE against, never a literal. Zelto derives line height
from the face (`z_line_height`), whose true ratio runs 1.318→1.253 across the scale; the
`font×1.31` fudge already bit once. Apple spec leading (Body 22pt, Large Title 41pt) is
here to sanity-check `z_line_height`, not to hardcode.

---

## 2. Spacing — the 8pt grid

**Apple ships no named spacing scale.** The 8pt grid + 4pt half-step + 16pt iPhone edge
margin are conventions + layout-guide APIs (`layoutMarginsGuide`, `readableContentGuide`,
`systemMinimumLayoutMargins`), not an enumerated token table. So a Zelto spacing scale is
**invented on the grid**, grounded in Apple's convention — not mirrored from a table.

| Apple quantity | pt | units | Zelto today |
|---|---|---|---|
| Half-step | 4 | 7 | — (inline literals) |
| Base grid | 8 | 15 | — |
| — | 12 | 22 | inline `spacing=12` (value↔pill) |
| Edge margin (iPhone) | 16 | 30 | `ROW_PAD` = `Z_PT(16)` = 29 ✓ |
| Section gap | 16–20 | 30–37 | `SEC_GAP` = **30 literal** (16.2pt, off-grid, not `Z_PT`) |
| — | 24 | 44 | — |
| — | 32 | 59 | — |

**Findings.** There is no scale — `SEC_GAP`, `ROW_PAD`, `gap()` calls and inline literals
each chose their own number. `ROW_PAD` is correct (`Z_PT(16)`). `SEC_GAP` (30u) and the
inline `spacing=12` are literals off the grid.

**Proposed Stage 1 scale** (steps of `Z_PT`, so it retunes from one numerator, and off-grid
literals become steps of it):

```
Z_SPACE_XS  Z_PT(4)   =  7    tight intra-component
Z_SPACE_S   Z_PT(8)   = 15    base
Z_SPACE_M   Z_PT(12)  = 22    value↔control
Z_SPACE_L   Z_PT(16)  = 29    edge margin / row inset  (== ROW_PAD)
Z_SPACE_XL  Z_PT(20)  = 37    section gap (measure SEC_GAP against this)
Z_SPACE_2XL Z_PT(24)  = 44
Z_SPACE_3XL Z_PT(32)  = 59
```

`SEC_GAP`'s replacement is measured against a re-shoot in Stage 1 (iOS grouped-section gap
runs larger than 16pt — ~35pt between sections — so `Z_SPACE_XL` (20pt) or a dedicated
section step is the candidate, confirmed by a shot, not chosen at a desk).

---

## 3. Corner radii — continuous corners

| token | Zelto u | Zelto pt | iOS reference | delta | note |
|---|---|---|---|---|---|
| `CHIP` | 10 | 5.4 | small control; iOS 26 favours capsule | — | keep (0.43× icon corner) |
| `CARD` | 16 | 8.6 | inset-list card ~10pt = 19u **[unpub]** | −3u | already flagged tight ~15% |
| `PANEL` | 22 | 11.9 | alert ~14pt = 26u **[unpub]** | −4u | Zelto alert radius under iOS |
| `WIDGET` | 40 | 21.6 | home widget (no iOS number) | — | keep; anchored to icon corner 23.3u |
| `SHEET` | 32 | 17.3 | large detent = **device screen radius** | open | not a fixed iOS pt |
| `ICON` | 0.2237× | — | **0.2237× width** + 0.6 smoothing | 0 | ratio matches ✓ |

**The math.** iOS continuous corner = a superellipse `|x|^n+|y|^n=r^n` at **n≈5**
(bezier-approximated, hand-tuned — no closed form Apple ships). Zelto uses **n=4** (true
squircle). "Continuous" means curvature ramps in smoothly instead of an arc meeting a
straight edge with a curvature jump. **Decision for Stage 1:** whether to push Zelto's
corner exponent from 4 toward 5 is a render-math change (`corner_coverage` in
`sdk/src/render.c` + the compositor mask must agree) — approximating the true superellipse
is possible but must be done in BOTH places or the app and its compositor mask disagree.
Recommend: keep n=4 for now (it is already a squircle, not a circle-arc box; the visible
gap to n=5 is small) and record the option. The icon ratio (22.37%) and its 0.6 smoothing
are the one rock-solid radius fact and Zelto already has the ratio.

**Concentric corners are now first-class in iOS 26** (`ConcentricRectangle` /
`.containerConcentric`): **inner radius = outer radius − padding**, computed automatically.
This is the load-bearing iOS 26 rule and it matches Zelto's own standing rule ("a reserve
that names its parts must be an expression over them"). **Stage 1: make the concentric rule
real** where a rounded thing sits in another (chip in card, card in sheet) — nested radius
as an EXPRESSION `outer − padding`, never a second guessed constant. Watch the
`Frame`+padding double-count trap.

---

## 4. Colour — the dark palette

**The tone ladder already matches iOS** — this is the good news of the phase:

| Zelto token | Zelto hex | iOS dark token | iOS hex | match |
|---|---|---|---|---|
| `BG` | #000000 | systemBackground / systemGroupedBackground | #000000 | ✓ |
| `SURFACE` | #1c1c1e | secondarySystemGroupedBackground (card) | #1c1c1e | ✓ |
| `SURFACE_2` | #2c2c2e | tertiarySystemGroupedBackground | #2c2c2e | ✓ |
| `SURFACE_3` | #3a3a3c | systemGray4 | #3a3a3c | ✓ |
| `SURFACE_4` | #555559 | ~systemGray2/3 band | #48–63 | ~ |

**Labels differ in CONSTRUCTION, not far in value.** iOS dark labels are near-white
`#EBEBF5` carried at opacity; Zelto uses opaque greys (which is *why* P51 could compute a
contrast table — opaque is reason-about-able). Reference values:

| tier | iOS (dark) | Zelto | note |
|---|---|---|---|
| label / primary | #FFFFFF 100% (pure white) | `TEXT` #f2f2f7 | Zelto soft-white by choice (21:1 white tires the eye) |
| secondaryLabel | #EBEBF5 @60% ≈ #9c9ca1 on #1c1c1e | `TEXT_MUTED` #a1a1a8 / #c7c7cc (contrast) | close ✓ |
| tertiaryLabel | #EBEBF5 @30% | `TEXT_FAINT` #6c6c70 / #a8a8b0 (contrast) | Zelto faint darker |
| quaternaryLabel | #EBEBF5 @18% | — | Zelto has no 4th tier |

**Separator.** iOS `separator` = **#545458 @60%** (translucent), hairline = 1 physical pixel
= **0.33pt @3x** (≈ 0.54u on this geometry), never a fixed pt constant. Zelto `BORDER` =
opaque #38383a at 1u — darker and thicker than iOS. Stage 1 candidate: lift the hairline
toward #545458 (opaque equivalent) and confirm 1u vs a thinner line reads right on target.

**The two semantic-fill contrast failures P51 left for P52** (both want the FILL redrawn,
not the ink — a palette change, which is now in scope):

| pair | ratio now | AA target | fix direction |
|---|---|---|---|
| `TEXT` (#f2f2f7) on `WARN` (#e0a53a) | **1.96:1** | 4.5 (3 large) | WARN is bright-orange → light text can't sit on it. Use **dark ink** on WARN (like iOS orange badges) or darken WARN. |
| `ON_PRIMARY`… on `SUCCESS` (#1e7a4a) | **3.71:1** | 4.5 | The white-ink/SUCCESS pair; darken `SUCCESS` (deeper green) so white clears AA, or use it only for large text. |

Stage 1 extends `test_contrast_tokens.c` to cover both, picks the least-invasive fix
(iOS systemGreen is #30d158 in dark — brighter, but iOS puts *dark* text on it; iOS
systemOrange #ff9f0a likewise takes dark text). Recommend: **dark ink on WARN**, and
**darken SUCCESS** to clear white at AA — chosen against the computed table, verified on
target.

---

## 5. Materials, elevation, hairlines

**Materials: Apple publishes ZERO numbers.** No tint hex, alpha, blur radius/sigma for any
of the six materials (ultraThin→ultraThick + bar), no vibrancy alphas, no Liquid-Glass
optics (lensing/refraction, specular-highlight intensity, adaptive shadow). All
runtime-adaptive, computed at composite from a background sample. **Zelto OWNS these
constants** — "match iOS" is impossible by construction. Zelto's material tints
(`THIN`/`REGULAR`/`THICK`/`SHEET` at ~55/72/86/90%) are therefore authoritative, not a
delta to close. The only Apple *rule* to honour: don't stack material on material; use a
vibrant edge (Zelto `MATERIAL_EDGE` white@12%) so the panel doesn't dissolve into a busy
backdrop.

**iOS 26 Liquid Glass** (recorded, not encoded): lensing (refracts background, not just
blur) + motion-tracked specular highlights + concentric corners + `.regular`/`.clear`
variants. Even fewer fixed numbers than legacy materials. Out of reach for a software
compositor this phase — the one transferable idea is the **specular edge highlight**, which
Zelto already approximates with `MATERIAL_EDGE`.

**Elevation: iOS builds depth from TONE + HAIRLINES, not drop shadows.** Apple publishes no
shadow spec for cards/sheets/popovers/alerts. Grouped-list cards are separated by tone
(#000→#1c1c1e) + hairlines, not shadow. Real shadows appear only on genuinely floating
transient surfaces (popover/menu), and even there the numbers aren't documented.

| Zelto token | value | iOS convention | verdict |
|---|---|---|---|
| `ELEV_1` 6u | chips/rows | iOS: **no shadow** on rows — tone only | candidate to drop on flat cards |
| `ELEV_2` 14u | cards/widgets | iOS: tone + hairline | keep for genuinely raised (widget) |
| `ELEV_3` 28u | overlays/modals | iOS: soft shadow on floating overlays | keep |

**Stage 1/2 finding:** audit where Zelto casts a shadow on something iOS would separate by
tone (a settings card sitting on the page). Reserve `Shadow()` for floating transient
surfaces; lean on the tone ladder + hairline for cards. This is an optical change with a
rule behind it, not a taste call.

---

## 6. Control metrics

| Control | Apple pt | Apple u | Zelto u | Zelto pt | delta | status |
|---|---|---|---|---|---|---|
| Min touch target | 44 | 81 | 81 (`Z_ROW_H` floor) | 44 | 0 | ✓ published |
| Grouped row min height | 44 | 81 | 81 (`z_row_h` floor) | 44 | 0 | ✓ |
| Row / separator inset | 16 | 30 | 29 (`ROW_PAD`) | 16 | −1 | ✓ align to text margin |
| Nav bar height | 44 | 81 | 81 (`BAR_H`) | 44 | 0 | ✓ contract |
| Large-title total area | ~96 | ~177 | — | — | — | Zelto has no large-title nav yet |
| Tab bar | 49 (+34 safe) | 90 (+62) | — | — | — | Zelto uses homebar, no tab bar |
| **Switch — track** | **51×31** | **94×57** | **52×32** | 28×17 | **−42×−25** | ⚠️ **headline finding** |
| Switch — knob | 27.5 **[unpub]** | 51 | 26 | 14 | **−25** | ⚠️ ~half size |
| Slider — thumb | ~28 **[unpub]** | ~52 | (check sdk) | — | — | verify Stage 2 |
| Segmented — height | ~32 **[unpub]** | ~59 | (CC toggles) | — | — | verify Stage 2 |
| Stepper — intrinsic | ~94×30 **[unpub]** | ~174×55 | ~105×36 | ~57×19 | narrower | Zelto pill 2×52+divider |
| Chevron / disclosure | ~7×12 **[unpub]** | ~13×22 | 16×16 | 8.6×8.6 | squarer | Zelto chevron squarer, iOS taller+semibold |
| Sheet grabber | 36×5 **[unpub]** | 66×9 | (check) | — | — | verify Stage 2 |
| Button height | **none published** | — | — | — | — | iOS 26 = `ControlSize` + **Capsule**; no number to match |

**The headline finding: the toggle switch is ~55% of iOS's proportional size — and the
mechanism is the P43/P44 points-as-pixels bug, still alive in one control.** The track
(51×31pt → 94×57u) is the ONE solid, Apple-published toggle number; Zelto draws it at
**52×32u**, and 51→52 / 31→32 is not a coincidence in a codebase that has already been
caught spending points as pixels twice (the type scale, P43; the safe areas, P44). The
switch is the spec sheet transcribed straight into screen units with no `Z_PT`. The
corroboration is in the frame: iOS's switch is 31 of a 44pt row = **70% of the row
height**; Zelto's is 32 of 81 = **40%**. It looks small next to its own row. The knob (27.5pt → 51u, [unpub] but consistent across sources) is at 26u. This is
the single most visible metric miss in the OS — a switch appears on Settings rows and the
Control Center. **Stage 2 fix**, measured: track → ~94×57u, knob → ~51u with a ~3u inset,
track radius = height/2 (fully rounded). Re-shoot Settings to confirm the row still lays
out (a taller switch changes `z_row_h`'s floor interaction — verify against the AX audit).

**Buttons have no iOS point height** — iOS 26 sizes via `ControlSize`/`buttonSizing` and
defaults to **Capsule** (radius = height/2). Do not hardcode a button height "to match
iOS"; there is none. Capsule = height/2 is the rule to adopt where Zelto draws pill buttons.

**Separator/row insets align to the TEXT layout margin (16pt), not a fixed edge literal** —
encode as "align to content margin," which Zelto's `ROW_PAD` already does.

---

## 7. Motion

Zelto's springs, converted (`response = 2π√(m/k)`, `ζ = c/(2√(km))`, m=1):

| Zelto token | k | c | response | ζ | iOS analogue | verdict |
|---|---|---|---|---|---|---|
| `STANDARD` | 170 | 26 | **0.48s** | **1.00** | `.smooth` (0.5s, ζ1.0) | **≈ exact match** ✓ |
| `SNAPPY` | 300 | 30 | 0.36s | 0.87 | `.snappy` (0.5s, ζ0.85) | same bounce, **faster** |
| `PRESS` | 520 | 34 | 0.28s | 0.74 | press-family (~0.1–0.15s) | tight+fast; fine for touch |

**iOS springs are one number: duration 0.5s.** smooth/snappy/bouncy differ only by bounce
(0 / 0.15 / 0.3 → ζ 1.0 / 0.85 / 0.7), where `bounce = 1 − dampingFraction`. Zelto's
`STANDARD` already IS iOS `.smooth`. `SNAPPY` has the right bounce (ζ0.87 ≈ iOS 0.85) but
runs faster (0.36s vs 0.5s) — a phone feeling quicker than the desk default is a defensible
choice, but it is a **delta to decide** in Stage 3, not leave unexamined. `PRESS` is tighter
than iOS's press feedback, appropriate for immediate touch-down.

**Durations:** iOS standard transition ≈ 0.25–0.3s (not a single published constant).
**Reduce Motion** replaces slide/scale/zoom with cross-fades and suppresses overshoot —
Zelto already collapses every spring to an instant jump (P31); **do not regress that.**

**Stage 3 candidates:** (a) `SNAPPY` response 0.36→~0.5s to match iOS, or keep faster and
document why; (b) a `bouncy` profile (ζ0.7) if any surface wants overshoot — currently none
does; (c) confirm press feedback timing against the frozen-transition catalogue shots.

---

## Highest-leverage fixes, sequenced

1. **The switch (Stage 2).** ~55% undersized; most visible metric miss. Track 52×32→94×57u,
   knob 26→51u. Solid Apple number. Re-shoot Settings + Control Center.
2. **The spacing scale (Stage 1).** No scale exists; `SEC_GAP` and inline `12` are off-grid
   literals. Establish `Z_SPACE_*` on `Z_PT`, replace literals. Highest leverage — one
   ladder tightens every surface at once.
3. **Concentric radii (Stage 1).** Make nested = outer − padding an expression (iOS 26's own
   load-bearing rule). Fixes chip-in-card / card-in-sheet consistency.
4. **Semantic-fill contrast (Stage 1).** WARN 1.96:1 and SUCCESS 3.71:1 — redraw the fills
   (dark ink on WARN, darken SUCCESS), extend `test_contrast_tokens.c`. Was deferred from
   P51 precisely because it needs the palette change now in scope.
5. **Type ramp drifts (Stage 1).** Large Title +6pt, Callout +4pt (really Title3), Title2
   +2pt. Small, deliberate-looking; decide per-step and RE-RUN the AX5 overflow/reflow audit
   after any change (a bigger line box can clip a box that fit).
6. **Tracking (Stage 1 decision).** Add Title-level tightening (−1 to −2u) or record out of
   scope — but write the decision down.
7. **Shadow-vs-tone (Stage 1/2).** Audit shadows Zelto casts where iOS separates by tone;
   reserve `Shadow()` for floating transient surfaces.
8. **Hairline (Stage 1).** `BORDER` #38383a → toward iOS #545458; confirm 1u width on target.

## What Apple does not publish (so Zelto owns it, and "match iOS" doesn't apply)

Materials (all alphas/blur), vibrancy levels, Liquid-Glass optics, system shadows, the
switch knob, slider thumb, segmented/stepper heights, sheet corner radius + grabber, button
heights, most control corner radii. For every one of these the Zelto constant is a chosen
value with a measured-on-target check — not a spec to conform to.

## Sources

Type/spacing/radii: Apple HIG Typography & Layout; SF Pro tracking table (Apple Fonts,
eonist gist, Sketch-SF-UI-Font-Fixer); squircle.js superellipse math; Liam Rosenfeld icon
quest; ConcentricRectangle (DevTechie, Nil Coalescing). Colour/materials/elevation: Sarunw
dark-color cheat sheet (resolved UIColor); Apple HIG Materials; createwithswift vibrancy;
conor.fyi Liquid Glass reference; NGSystemColorComparison. Components/motion: Apple HIG
(Accessibility, Nav/Tab bars, Lists, Motion); Apple SwiftUI `snappy`/`bouncy`/`smooth`,
`ControlSize`, `buttonSizing`, `separatorInset`, `preferredCornerRadius`; WWDC23 Animate
with Springs; Amos Gyamfi spring cheat sheet; hacknicity bar heights; useyourloaf separator
inset; conorluddy LiquidGlassReference. Full URLs in the research transcripts.
