# Layout

Zelto lays out UI with **stacks** (vertical, horizontal, depth) plus **flex** and
**frame** modifiers. It is a single-pass, constraint-light system tuned for fast,
predictable mobile layout.

## Stacks

| Stack | Direction |
|---|---|
| `VStack` | top → bottom |
| `HStack` | leading → trailing |
| `ZStack` | back → front (overlap) |

```js
VStack({ spacing: 12, align: "leading", padding: 16 }, [
  Text("Title").font("title"),
  Text("Subtitle").font("callout"),
]);
```

Stack options: `spacing`, `align` (`leading`/`center`/`trailing` cross-axis), `padding`,
`justify` (`start`/`center`/`end`/`between` main-axis when the stack has extra space).

## Flexible sizing

Children size to their content by default. Use flex to distribute free space:

```js
HStack({}, [
  Text("Fixed"),
  Spacer(),                 // eats all free space
  Text("Right"),
]);

HStack({}, [
  Panel().grow(1),          // share free space 1:2
  Panel().grow(2),
]);
```

- `Spacer()` is a flexible gap.
- `.grow(n)` lets a view expand to fill available main-axis space, weighted by `n`.

## Frame and sizing modifiers

```js
Image(url).frame(120, 120);          // fixed width, height
Box().frame({ maxWidth: 480 });      // constraints
Text("Hi").frame({ width: "fill" }); // fill parent cross-axis
```

`frame` accepts fixed numbers or `{ width, height, minWidth, maxWidth, minHeight,
maxHeight }`, with `"fill"` to match the parent.

## Padding and insets

```js
View.padding(16);                    // all sides
View.padding({ x: 16, y: 8 });       // horizontal / vertical
View.padding({ top: 8 });            // one side
```

Use spacing tokens for consistency: `.padding("space.4")`.

## Safe areas

Phones have notches, rounded corners, and system bars. Content should respect **safe-area
insets**:

```js
Screen({ safeArea: true }, [ ...content ]);   // insets content automatically

// or read insets directly:
const inset = useSafeArea();   // { top, bottom, leading, trailing }
```

Backgrounds typically extend edge-to-edge while content stays inside the safe area.
Test insets with simulator profiles ([../getting-started/simulator.md](../getting-started/simulator.md)).

## Scrolling

Wrap overflowing content in `Scroll`; for long, data-driven, recycled lists use `List`.

```js
Scroll({}, VStack({ spacing: 12 }, children));
```

See [../ui/components/scroll.md](../ui/components/scroll.md) and
[../ui/components/list.md](../ui/components/list.md).

## Grids

```js
Grid({ columns: 3, spacing: 8 }, items.map(Cell));
```

See [../ui/components/grid.md](../ui/components/grid.md).

## C equivalents

Stacks and modifiers exist in C with the same names via macros:

```c
VStack(.spacing = 12, .align = Z_ALIGN_LEADING, .padding = 16,
  Font(Z_FONT_TITLE, Text("Title")),
  Spacer(),
  Frame(120, 120, Image(url)));
```

See [../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [styling-theming.md](styling-theming.md)
- [animation.md](animation.md)
