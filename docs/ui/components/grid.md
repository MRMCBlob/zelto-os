# Grid

Lays children out in a grid. For large grids, `Grid` recycles cells like
[list.md](list.md).

```js
Grid({ columns: 3, spacing: 8 }, items.map(Cell));
```

## Options

| Option | Type | Default | Notes |
|---|---|---|---|
| `columns` | number \| `"auto"` | — | Fixed count, or `"auto"` to fit `minItemWidth` |
| `minItemWidth` | number | — | Used with `columns: "auto"` |
| `spacing` | number \| token | `0` | Gap (both axes) |
| `rowSpacing` / `columnSpacing` | number | `spacing` | Per-axis override |
| `aspectRatio` | number | — | Cell width:height ratio |

## Fixed columns

```js
Grid({ columns: 2, spacing: 12 },
  photos.map(p => Image(p.url).aspectRatio(1).cornerRadius("md")));
```

## Adaptive columns

```js
Grid({ columns: "auto", minItemWidth: 120, spacing: 8 }, cells);
```

The grid fits as many columns as the width allows, each ≥ `minItemWidth`.

## Data-driven (recycled) grid

```js
Grid({
  columns: 3,
  data: items,
  key: it => it.id,
  cell: it => Thumb(it),
});
```

Use the data form for large grids so off-screen cells aren't built.

## C

```c
Grid(.columns = 3, .spacing = 8, children);
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
