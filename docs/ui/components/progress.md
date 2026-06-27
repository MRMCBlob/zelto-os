# Progress & Spinner

Show progress (determinate) or loading (indeterminate).

## Spinner (indeterminate)

```js
Spinner()
Spinner({ size: "large" })
```

| Option | Type | Default |
|---|---|---|
| `size` | `"small" \| "large"` | `"small"` |
| `tint` | color | `accent` |

Use while waiting on async work ([../../guides/networking.md](../../guides/networking.md)):

```js
if (!data) return Spinner({ size: "large" });
```

## Progress (determinate)

```js
Progress({ value: 0.6 })            // 0..1 bar
Progress({ value: pct, total: 100 })
```

| Option | Type | Default | Notes |
|---|---|---|---|
| `value` | number | — | Current progress |
| `total` | number | `1` | Max value |
| `style` | `"bar" \| "ring"` | `"bar"` | Linear or circular |

## Circular progress

```js
Progress({ value: downloaded, total: size, style: "ring" });
```

## In context

```js
VStack({ spacing: 8 }, [
  Progress({ value: pct }),
  Text(`${Math.round(pct * 100)}%`).font("caption"),
]);
```

## C

```c
Spinner(.size = Z_SPINNER_LARGE);
Progress(.value = 0.6f);
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
