# Slider

Selects a continuous (or stepped) value within a range.

```js
const [volume, setVolume] = useState(0.5);
Slider({ value: volume, onChange: setVolume });   // 0..1 by default
```

## Options

| Option | Type | Default | Notes |
|---|---|---|---|
| `value` | number | — | Bound value |
| `onChange` | `(n) => void` | — | Called while dragging |
| `onCommit` | `(n) => void` | — | Called when the drag ends |
| `min` / `max` | number | `0` / `1` | Range |
| `step` | number | — | Snap increment (omit for continuous) |
| `disabled` | boolean | `false` | Non-interactive |

## Stepped slider with labels

```js
VStack({ spacing: 4 }, [
  Slider({ value: level, onChange: setLevel, min: 1, max: 5, step: 1 }),
  Text(`Level ${level}`).font("caption").foreground("text.secondary"),
]);
```

## Continuous vs. commit

Use `onChange` for live preview (e.g. brightness) and `onCommit` to persist once the user
lets go, to avoid excessive writes ([../../guides/storage.md](../../guides/storage.md)).

## C

```c
Slider(.value = s->volume, .on_change = on_volume, .min = 0, .max = 1);
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
