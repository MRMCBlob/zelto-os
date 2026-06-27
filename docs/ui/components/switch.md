# Switch

An on/off toggle bound to a boolean.

```js
const [on, setOn] = useState(false);
Switch({ value: on, onChange: setOn });
```

## Options

| Option | Type | Default | Notes |
|---|---|---|---|
| `value` | boolean | — | Bound state |
| `onChange` | `(bool) => void` | — | Toggle handler |
| `disabled` | boolean | `false` | Non-interactive |
| `tint` | color | `accent` | On-state color |

## In a settings row

```js
HStack({ align: "center" }, [
  Text("Wi-Fi"),
  Spacer(),
  Switch({ value: wifi, onChange: setWifi }),
]).padding({ x: 16, y: 12 });
```

## Behavior

- Animates with the snappy spring ([../../guides/animation.md](../../guides/animation.md)).
- Uses the accent token by default so it matches the system theme.

## C

```c
Switch(.value = s->wifi, .on_change = on_wifi);
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
