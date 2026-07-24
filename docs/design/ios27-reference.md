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
| Large Title | 34 | 63 | ~~74~~ → **62** | ~~40~~ → **34** | **closed** | hero ran +6pt |
| Title 1 | 28 | 52 | 51 (`TITLE`) | 28 | −1 | match |
| Title 2 | 22 | 41 | ~~44~~ → **40** | ~~24~~ → **22** | **closed** | ran +2pt |
| Title 3 | 20 | 37 | — | — | — | iOS step Zelto lacks — **deliberately not added**, see below |
| Headline | 17 (Semibold) | 31 | 31 (`HEADLINE`+SB) | 17 | 0 | match |
| Body | 17 | 31 | 31 (`BODY`) | 17 | 0 | match |
| Callout | 16 | 30 | ~~37~~ → **29** | ~~20~~ → **16** | **closed** | ran +4pt; see the note below |
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
**STAGE 1c DECISION — tracking is deliberately NOT modelled.** The whole effect is −2u at
Large Title, −1u through the Title/Body band, and **zero below ~16pt** where it rounds
away — invisible on most of this OS and worth about one unit where it is not. Against
that: a tracking axis must be threaded through **both** measure and paint, and if the two
disagree the text overflows its box **silently**. That is the exact failure this project
has already paid for twice (P45's `wrap_measure` truncating at 512 bytes; P51's fixed
column holding text). One unit of Title tightening does not buy that risk. If it is added
later, `z_text_measure` and the glyph loop change in the *same* commit, with a test that
measures a tracked string and asserts the resulting frame.

**STAGE 1c OUTCOME — eight of eleven steps already matched**, which is itself the finding:
the three that did not were **drift**, not a systematic error. That is a different bug from
P43, where *every* step was wrong by the same factor, and it takes a different fix — per
step, not to the converter.

**The Callout correction, and the misreading attached to it.** This dossier first read
Zelto's 20pt `CALLOUT` as actually being iOS's **Title 3** (20pt), a step the ladder lacks
— which would have meant renaming it and adding a real 16pt Callout. Checking the **22 call
sites** says otherwise: every one is a control label (a keyboard function key, a consent
button, "Done", the remove badge) or emphasised body (a status line, a strapline, "No
running apps"). **Not one is a compact title.** So the *number* was Title 3 while every
*use* was a Callout, and the fix is the number. **No `Z_FONT_TITLE3` was added** — a token
no surface asks for is the step that gets picked by whoever wants "a bit bigger", which is
how a ladder stops meaning anything.

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

**THE SHIPPED SCALE (Stage 1a)** — steps of `Z_PT`, so it retunes from one numerator.
`Z_PT` is integer arithmetic and truncates, so `Z_PT(8)` is 14, not 15:

```
Z_SPACE_2XS Z_PT(2)   =  3    a subtitle under its title
Z_SPACE_XS  Z_PT(4)   =  7    a caption under the control it names
Z_SPACE_S   Z_PT(8)   = 14    THE DEFAULT: siblings in a row
Z_SPACE_M   Z_PT(12)  = 22    components in a column
Z_SPACE_L   Z_PT(16)  = 29    content margin  (== ROW_PAD)
Z_SPACE_XL  Z_PT(20)  = 37    between the groups of a grouped list (SEC_GAP)
Z_SPACE_2XL Z_PT(24)  = 44
Z_SPACE_3XL Z_PT(32)  = 59
```

**The histogram was the evidence, and it is a transcription bug, not an aesthetic.** The
three commonest gap literals in the OS were `8`, `10` and `12` — exactly SwiftUI's default
VStack spacing (8pt), its default HStack spacing (10pt), and the grid's 12pt step, typed
into screen units. The paddings were `16 / 20 / 24 / 32`, the grid's point steps. So every
gap was drawn at ~54% of what it was written for: the same defect as the P43 type scale
and the P44 safe areas, third occurrence, and invisible for the same reason — uniformly
wrong looks like a style. **The corroboration is already in the tree**: the two metrics
that had been through this (`ROW_PAD = Z_PT(16)`, `Z_ROW_H = Z_PT(44)`) are the correct
ones, and `settings/main.c:338` says so in P44's own words — "16 units is 8.6pt where the
list it copies uses 16pt". The row holding a correct 16pt outer margin was putting a
6.5pt gap between its own icon and its label.

`SEC_GAP` went 30 → `Z_SPACE_XL` (37). iOS's grouped-section gap reads ~35pt (the default
grouped section-header height), which would be 65u; `XL` is the largest step that does not
push the root list's last group off the fold. The 35pt figure stays here as the upper
bound to test against a shot rather than adopt at a desk.

Two carve-outs are deliberate and written into the header. A gap that IS a control's own
geometry (the passcode dots' pitch, the status bar's glyph cluster) takes the NEAREST step
rather than the role's reference — regridding gaps is not redrawing controls. And GLYPH
internals (the battery meter's segments, the App Library's 2×2 quad mark) plus
Button/TextField's own insets stay off the scale entirely: the first is drawing, the
second is control metrics (see §6).

---

## 3. Corner radii — continuous corners

| token | Zelto u | Zelto pt | iOS reference | delta | note |
|---|---|---|---|---|---|
| `CHIP` | 10 | 5.4 | small control; iOS 26 favours capsule | — | keep (0.43× icon corner) |
| `CARD` | 16 | 8.6 | inset-list card ~10pt = 19u **[unpub]** | −3u | already flagged tight ~15% |
| `PANEL` | 22 | 11.9 | alert ~14pt = 26u **[unpub]** | −4u | Zelto alert radius under iOS |
| `WIDGET` | 40 | 21.6 | home widget (no iOS number) | — | keep; anchored to icon corner 23.3u |
| `SHEET` | **45** (was 32) | 24.3 | large detent = **device screen radius** | derived | see below |
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

**STAGE 1b OUTCOME. Four steps did not move; one did, and it was not chosen.**

`CHIP` / `CARD` / `PANEL` / `WIDGET` stay. Their iOS readings above are all **[unpub]**
(Apple publishes no control corner radii), every delta is under 4 units, and P45 already
declined exactly these two on exactly these grounds. Moving `CARD` would additionally move
the sheet, which is derived from it — so one eyeballed nudge would quietly become two.

`SHEET` went **32 → 45**, as the only number the concentric rule left free. Stage 1a put
the share sheet's content margin on Apple's 16pt (`Z_SPACE_L`, 29u) like every other
container in the OS; with the inset at 29 and the rows at `CARD`, the sheet's corner is
determined: **16 + 29 = 45**. Two things the old value was hiding:

- **The rows were inset 8.6pt.** P48 had closed the concentric deficit by pinning
  `SHEET_PAD` to whatever made the sum work, which made a *layout* metric the slave of a
  *radius*. A share sheet's rows sat twice as close to its edge as a Settings row does to
  its card. P52 reverses the dependency — the inset is the content margin, the row's corner
  is `Z_RADIUS_NESTED(SHEET, PAD)`, and the sheet's own radius is the sum.
- **The ladder was not monotonic.** At 32 the largest pulled surface in the OS had a
  *squarer* corner than a home widget (40). `test_radius_ladder.sh` rule 6 now forbids it —
  nothing in the previous five rules could see it, because each checked a radius against
  something other than its siblings.

**The exponent decision: n = 4, kept, written down.** It is implemented **twice** — the
SDK's `corner_coverage` and the compositor's `CORNER_N` — and the two must agree or a
material's blur is masked to a different curve than the surface painted over it. The SDK
specialises the arithmetic (`u²·u² + v²·v²`, no `pow()`, per pixel of every corner), so
`Z_CORNER_N` carries a `_Static_assert` tying the token to that code, and rule 7 asserts
the two renderers match. Chasing iOS's hand-tuned n≈5 would mean tracking a curve Apple
ships no closed form for, and the gap between n=4 and n=5 is far smaller than the gap
between a circular arc and either — which is the jump Zelto already made.

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

**Separator — MEASURED IN STAGE 1d, and the delta above was an artefact of the comparison.**
iOS `separator` is #545458 **at 60% alpha**; Zelto's `BORDER` is opaque. Comparing the raw
tokens compares different things. Composited over each surface, iOS's separator resolves to:

| over | iOS resolves to | ratio vs surface | Zelto `BORDER` #38383a | ratio |
|---|---|---|---|---|
| `BG` | #323234 | 1.64 | #38383a | 1.79 |
| `SURFACE` | #3d3d40 | 1.57 | #38383a | 1.45 |
| `SURFACE_2` | #444447 | 1.44 | #38383a | 1.19 |

Zelto's opaque hairline sits **inside iOS's own band**. **No change.** (One real difference:
iOS's is translucent, so it gets lighter as the surface does, while Zelto's is fixed — on
`SURFACE_2` Zelto's is the weaker line, 1.19 vs 1.44. The accessibility case is already
covered by the Increase Contrast value, #6c6c70.) **Width:** iOS's hairline is 1 physical
pixel; Zelto's logical unit **is** a physical pixel on this 720×1440 geometry, so a 1u
hairline is correct by construction, not by coincidence.

### The semantic colours — P51's diagnosis was wrong twice, and the real failure was elsewhere

P51 recorded two semantic failures and deferred them as "the fill wants redrawing". Measured
against what the OS actually **draws** (Stage 1d), both parts of that were wrong:

- **These tokens are mostly INK, not fills.** The charging battery glyph, the home widget's
  battery percentage, a status line — all `Foreground(Z_COLOR_SUCCESS, …)`. In that role the
  old green was the real failure, and nobody had written it down:
  `SUCCESS #1e7a4a` as ink measured **BG 3.94 · SURFACE 3.19 · SURFACE_2 2.61 · SURFACE_3
  2.13** — two surfaces under even WCAG's 3:1 *non-text* bar. It had been tuned to be "deep
  enough for white text at AA", i.e. for the role it is rarely used in.
- **The pair P51 named is not one the OS draws.** `ON_PRIMARY` on `SUCCESS` (3.71) — but
  `ON_PRIMARY` is documented for a `PRIMARY` fill. The pair that *was* drawn, and that the
  header itself prescribed, was `TEXT_INV` on a semantic fill: **1.99:1 on WARN**, worse than
  anything P51 listed. The header was telling callers to make an unreadable combination.

**The fix is iOS's dark semantic set, as one sourced decision rather than three nudges.**

| token | was | now | as ink: BG / SURFACE / SURFACE_2 / SURFACE_3 | as fill, with `z_on_fill` |
|---|---|---|---|---|
| `SUCCESS` | #1e7a4a | **#30d158** (iOS dark systemGreen) | 10.39 / 8.42 / 6.89 / 5.61 | 9.78 (dark ink) |
| `WARN` | #e0a53a | **#ff9f0a** (iOS dark systemOrange) | 10.22 / 8.28 / 6.78 / 5.52 | 9.62 (dark ink) |
| `DANGER` | #d9524f | **#ff453a** (iOS dark systemRed) | 6.16 / 4.99 / 4.09 / 3.33 | 5.81 (dark ink) |

Every ink use now clears 4.5 except `DANGER` on `SURFACE_3`, which clears 3.0. `z_on_fill(fill)`
returns whichever ink reaches further by WCAG — one computed rule, so retinting a fill moves
its ink with it. **Deliberate divergence from iOS:** Apple ships white on systemRed, which
measures **3.11:1** — Apple's own red badge does not clear AA. Zelto picks by measurement, so
a `DANGER` fill takes dark ink and looks slightly unlike iOS.

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
| `ELEV_1` 6u | a knob, a key cap, an app icon | iOS shadows these too | keep |
| `ELEV_2` 14u | home widget, dock plate | over the wallpaper — floating | keep |
| `ELEV_3` 28u | overlays/modals | iOS: soft shadow on floating overlays | keep |

**STAGE 1e OUTCOME — 20 shadow sites audited, exactly one was wrong, and the ladder itself
did not move.** The rule that came out of it is not "raised in the hierarchy" but **"is
there something UNDER it that it is floating over?"**:

- **Keeps its shadow:** a sheet or alert (over an app), the keyboard's accent popup and
  preview callout, the volume HUD, the switcher's cards, an app icon and the dock plate
  (over the wallpaper), a notification card (over wallpaper / an app / a blurred panel —
  never over a sibling surface), a knob riding a track.
- **Drops it:** a card *sitting on* a surface. It is already a step up the tone ladder from
  what it sits on, and that step **is** the separation.

The single offender was `z_card` in `system/common/app_chrome.h`. **The anchor was in the
repo, not in a spec:** Settings' own grouped cards cast no shadow — the only `Shadow()` in
that file is the switch knob — so the most-used list in the OS already followed the rule
and `z_card` was the outlier. Two idioms visibly disagreeing inside one product is worth
more than a style guide.

**No lint.** "Floating over something" vs "sitting on a surface" is semantic, not
syntactic; a rule a grep cannot check belongs in the header where it is read, not in a test
that would only approximate it.

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
| **Switch — track** | **51×31** | **94×57** | ~~52×32~~ → **94×57** | 51×31 | **closed** | ✅ measured in frame |
| Switch — knob | 27.5 **[unpub]** | 51 | ~~26~~ → **50** | 27 | **closed** | inset now derived |
| Slider — track | 4 | 7 | 6 (`thickness`) | 3.2 | −1 | ✓ close |
| Slider — thumb | ~28 **[unpub]** | ~52 | **24** (`t × 4`) | 13 | **−28** | ⚠️ see below |
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

**The slider's thumb is a RATIO, and the ratio is the thing that is wrong.** `sdk/src/slider.c`
computes `knob = thickness × 4`, so with the default 6-unit track the thumb is 24u = 13pt.
The track itself is nearly right (6u = 3.2pt against iOS's 4pt), which is what makes this
worth stating as a ratio rather than a size: **iOS's thumb-to-track ratio is 28/4 = 7×,
Zelto's is 4×.** Fixing the multiplier alone gets the thumb to 42u; putting both on `Z_PT`
(track `Z_PT(4)` = 7, thumb `Z_PT(28)` = 51) matches iOS outright. Stage 2 should do the
latter and keep the relationship as an expression, since a bare `× 7` would be the same
un-sourced constant one step along.

**A SECOND CONTROL IS UNDER THE TOUCH TARGET, found while regridding the spacing
(Stage 1a).** `sdk/src/view.c` gives `Button` a 14-unit inset and `ZTextField` a 12-unit
inset, each with a comment reading "~44px tall at body size". That comment was written
when Body was 17 *units* — it is pre-P43, and the P43 fix moved Body to 31 units without
moving these. Measured at today's ramp (`z_line_height(BODY)` ≈ 41u):

| control | inset | line box | total | in pt | vs 44pt min |
|---|---|---|---|---|---|
| `Button` | 14 × 2 | 41 | **69u** | 37pt | **−7pt** |
| `ZTextField` | 12 × 2 | 41 | **65u** | 35pt | **−9pt** |

Both are below Apple's 44pt minimum touch target, which `Z_ROW_H` already encodes
correctly at 81u. **Stage 2 fix:** the inset becomes whatever makes the total reach
`Z_ROW_H` — an expression over the line box, like `z_row_h()`, not a new literal. Same
shape of bug as the switch, in the toolkit rather than in an app.

**STAGE 2 OUTCOME — all three closed, and the switch verified in a real frame.** `ZELTO_PROBE_TAPS`
on Settings ▸ Lock Screen reports the switch's tappable at **`w=94 h=57`** (exactly `Z_PT(51)` ×
`Z_PT(31)`), its right edge 29u inside the card, and `0 overflowing / 0 clipped / 0 sideways`.
**57 in an 82-unit row is 70%** — iOS's proportion, against the 40% it had.

The touch targets took a new toolkit primitive rather than a bigger padding: `min_h`, a floor on a
node's **outer** height applied after padding. `fixed_h` was the wrong tool — `measure()` treats it
as an *inner* height and adds padding to it (the double-count trap that has cost two phases), and a
touch target is by definition the box a finger lands on. `Button` and `ZTextField` now ask for
`Z_ROW_H` by name, so they follow Dynamic Type for free: past the step where a Body line exceeds
the floor, the label's own line box wins.

**A process note.** The first PNG of the change *looked* like the stepper pills had grown to fill
their rows — a regression I was about to chase. The probe said `w=52 h=36`, unchanged, at row pitch
82. The frames were fine and the reading was wrong, which is exactly why the standing rule is *ask
the layout, never measure a coordinate off a screenshot.*

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

**STAGE 3 OUTCOME — the durations were not the problem; the REPRESENTATION was.**

The springs were hand-tuned `k`/`c` pairs, which are the wrong units to think in: nothing
about `k = 300, c = 30` says how long the motion takes or whether it overshoots, so no one
could tell whether a profile matched its own description. They are authored as **response**
and **bounce** now — Apple's own two axes, `bounce = 1 − dampingFraction` — and the physics
is derived (`ω = 2π/response`, `k = mω²`, `c = 2(1−bounce)√(km)`).

**The refactor is value-neutral, and that is the finding.** Converting the shipped constants
back through those formulas lands within a few percent of numbers a designer would have
chosen on purpose:

| profile | shipped | as (response, bounce) | derived back |
|---|---|---|---|
| `SNAPPY` | k 300, c 30 | 0.36s, 0.15 | k 304.6, c 29.67 |
| `PRESS` | k 520, c 34 | 0.28s, 0.25 | k 503.6, c 33.66 |
| `STANDARD` | k 170, c 26 | **0.50s, 0.00** | k 157.9, c 25.13 |

So the hand-tuning had been consistent with this model all along — it just could not be read.
`STANDARD` is the only one that moves: its true response was **0.482s** and its damping ratio
**0.997**, a hair off the critical damping it was meant to be. It is exactly 1.000 and 0.5s
now, which is Apple's `.smooth`.

**The duration divergence is kept, deliberately.** Apple gives smooth/snappy/bouncy the same
0.5s and lets bounce be the only axis. Zelto keeps `SNAPPY` at 0.36s and `PRESS` at 0.28s
because those two exist for moments where the motion **answers the finger** rather than
presents a surface — a carousel page flip, a touch-down highlight. A press veil that takes
half a second to appear reads as lag, not polish. `STANDARD`, which *is* the presenting case,
matches Apple exactly. **No `bouncy` profile was added**: no surface asks for one, and a token
nothing asks for is the step that gets picked by whoever wants "a bit more energy".

`test_motion_tokens.c` asserts the round trip in both directions, that the ladder stays
ordered (quicker ⇒ bouncier), that a zero-bounce profile is *exactly* critically damped, and
— the load-bearing one — that **every** spring entry point still honours Reduce Motion (P31),
counted structurally so a profile added later cannot forget.

---

## Highest-leverage fixes, sequenced — and what each turned out to be

Kept as written at Stage 0, with the outcome beside it. **Three of the eight predictions were
wrong**, and the ways they were wrong are the most useful thing in this file: a dossier
assembled from a spec is a set of hypotheses, and the codebase is what settles them.

| # | Predicted at Stage 0 | Outcome |
|---|---|---|
| 1 | The switch, ~55% undersized | ✅ **as predicted**, and the mechanism was the points-as-pixels bug — 51×31 **pt** drawn as 52×32 **u**. Proven in-frame: 40% → 70% of its row. |
| 2 | Spacing: no scale, `SEC_GAP` + inline `12` off-grid | ✅ **understated.** It was not "no scale" but the *same transcription bug*: the commonest literals `8/10/12` are SwiftUI's default VStack/HStack spacings and the grid's 12pt step, in units. |
| 3 | Concentric radii | ✅ done — and it **determined** `SHEET` (16 + 29 = 45) rather than merely tidying it, and exposed a non-monotonic ladder nothing could see. |
| 4 | Semantic fills: WARN 1.96, SUCCESS 3.71 | ❌ **wrong pairs.** `ON_PRIMARY`-on-`SUCCESS` is not drawn by this OS; the drawn failure was `TEXT_INV` on WARN (1.99). And the real defect was these tokens **as ink** — the old green hit 2.13 on a chip. |
| 5 | Type ramp: three drifts | ✅ **but Callout was misdiagnosed** as iOS Title 3. All 22 call sites are control labels or emphasised body — the *number* was Title 3, every *use* was a Callout. |
| 6 | Tracking: decide | ✅ recorded **out of scope**, with the risk (measure/paint must agree or text overflows silently) as the reason. |
| 7 | Shadow-vs-tone | ✅ **but far smaller than expected.** 19 of 20 sites were already right; exactly one (`z_card`) was wrong, and Settings' own cards were the in-repo anchor. |
| 8 | Hairline → iOS #545458 | ❌ **artefact of the comparison.** iOS's separator is 60% *alpha*; composited it lands in the band Zelto already occupies. **No change.** |

Two more that were not on the list at all, both found by working through the code rather than
the spec: the **`Button`/`ZTextField` touch targets** (69u/65u against an 81u minimum, stale
since P43), and the **slider's thumb-to-track ratio** (4× where iOS is 7×, hidden behind a
track that measured fine). And Stage 3 turned out not to be a tuning question at all — the
springs were **unreadable**, not mistuned.

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
