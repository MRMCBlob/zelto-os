# List

An efficient, scrolling list for large or dynamic data. Unlike a mapped `VStack` inside
`Scroll`, `List` **recycles** row views — only visible rows are built — so it stays smooth
with thousands of items.

```js
List({
  data: items,
  key: (it) => it.id,
  row: (it) => Row({ user: it }),
});
```

## Options

| Option | Type | Notes |
|---|---|---|
| `data` | array | Items to render |
| `key` | `(item) => string \| number` | Stable identity (required for diffing) |
| `row` | `(item, index) => view` | Builds a row |
| `onEndReached` | `() => void` | Infinite scroll / pagination |
| `onRefresh` | `() => Promise` | Pull-to-refresh |
| `separator` | view \| boolean | Row separator |
| `header` / `footer` | view | List header/footer |

## Sections

```js
List({
  sections: [
    { title: "A", data: itemsA },
    { title: "B", data: itemsB },
  ],
  key: (it) => it.id,
  row: (it) => Text(it.name),
  sectionHeader: (s) => Text(s.title).font("title"),
});
```

## Pagination / infinite scroll

```js
List({
  data: items,
  key: it => it.id,
  row: renderRow,
  onEndReached: () => loadMore(),
  footer: loading ? Spinner() : null,
});
```

## Swipe actions

```js
List({
  data: items, key: it => it.id,
  row: (it) => Row(it).swipeActions({
    trailing: [{ title: "Delete", role: "destructive", onTap: () => remove(it.id) }],
  }),
});
```

## Selection

```js
List({ data, key, row, selectable: true, selected, onSelect: setSelected });
```

## Performance notes

- Provide a stable `key` so diffing reuses rows instead of rebuilding.
- Keep `row` cheap; heavy per-row work negates recycling.
- Prefer fixed/estimated row heights for smooth scrolling; set `estimatedRowHeight` for
  variable heights.

## C

```c
List(.data = items, .count = n, .key = item_key, .row = build_row);
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
