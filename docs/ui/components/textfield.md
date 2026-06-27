# TextField

Single- or multi-line text input, bound to state.

```js
const [name, setName] = useState("");
TextField({ value: name, onChange: setName, placeholder: "Name" });
```

## Options

| Option | Type | Default | Notes |
|---|---|---|---|
| `value` | string | — | Bound value (controlled) |
| `onChange` | `(text) => void` | — | Called on edit |
| `placeholder` | string | — | Empty-state hint |
| `keyboard` | `"default" \| "email" \| "number" \| "phone" \| "url"` | `"default"` | On-screen keyboard type |
| `secure` | boolean | `false` | Password field |
| `multiline` | boolean | `false` | Grows to multiple lines |
| `autoFocus` | boolean | `false` | Focus on mount |
| `returnKey` | `"done" \| "next" \| "search" \| "send"` | `"done"` | Return-key label |
| `onSubmit` | `() => void` | — | Return key pressed |

## Examples

```js
TextField({ value: email, onChange: setEmail, placeholder: "Email",
            keyboard: "email", returnKey: "next" });

TextField({ value: pw, onChange: setPw, placeholder: "Password", secure: true });

TextField({ value: note, onChange: setNote, multiline: true }).frame({ minHeight: 100 });
```

## Focus control

```js
const field = useFocus();
TextField({ value, onChange, focusRef: field });
Button("Edit", () => field.focus());
```

## Validation

Validation is app-driven — derive an error from state and show it:

```js
const error = email && !email.includes("@") ? "Invalid email" : null;
VStack({ spacing: 4 }, [
  TextField({ value: email, onChange: setEmail, placeholder: "Email" }),
  error && Text(error).font("caption").foreground("destructive"),
]);
```

## Keyboard & insets

The on-screen keyboard adjusts safe-area insets; wrap forms in `Scroll` so focused fields
stay visible ([../../guides/layout.md](../../guides/layout.md)).

## C

```c
TextField(.value = s->name, .on_change = on_name, .placeholder = "Name");
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
