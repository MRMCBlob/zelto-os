# Scroll

A scrollable region for content larger than the viewport. For long, data-driven lists
use [list.md](list.md) instead (it recycles views).

```js
Scroll({}, VStack({ spacing: 12, padding: 16 }, children));
```

## Options

| Option | Type | Default | Notes |
|---|---|---|---|
| `axis` | `"vertical" \| "horizontal" \| "both"` | `"vertical"` | Scroll direction |
| `bounces` | boolean | `true` | Rubber-band over-scroll |
| `showsIndicator` | boolean | `true` | Scrollbar visibility |
| `paging` | boolean | `false` | Snap to viewport pages |
| `onScroll` | `(e) => void` | — | `{ offsetX, offsetY }` |

## Horizontal scroll

```js
Scroll({ axis: "horizontal" },
  HStack({ spacing: 12 }, cards));
```

## Pull to refresh

```js
Scroll({ onRefresh: async () => { await reload(); } },
  content);
```

A refresh control appears at the top; resolve the promise to dismiss it.

## Scroll position

```js
const sc = useScroll();
Scroll({ ref: sc }, content);
Button("Top", () => sc.scrollTo({ y: 0, animated: true }));
```

## Sticky headers

```js
Scroll({}, VStack({}, [
  SectionHeader("A").sticky(),
  ...itemsA,
  SectionHeader("B").sticky(),
  ...itemsB,
]));
```

## Keyboard avoidance

Wrap forms in `Scroll` so focused `TextField`s stay visible when the keyboard appears
([textfield.md](textfield.md)).

## Performance

`Scroll` renders all children eagerly. For hundreds+ of items, use [list.md](list.md),
which only builds visible rows.

## C

```c
Scroll(.axis = Z_AXIS_VERTICAL,
  VStack(.spacing = 12, children));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
