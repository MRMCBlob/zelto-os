# Button

A tappable control that triggers an action.

```js
Button("Save", () => save())
Button("Save", () => save()).filled()
Button.icon("share", () => share())
```

## Forms

`Button(title, onTap, options?)`

| Option | Type | Default | Notes |
|---|---|---|---|
| `role` | `"normal" \| "primary" \| "destructive"` | `"normal"` | Affects default styling |
| `disabled` | boolean | `false` | Non-interactive + dimmed |
| `loading` | boolean | `false` | Shows a spinner, blocks taps |

## Styles (modifiers)

| Modifier | Look |
|---|---|
| `.filled()` | Solid accent background |
| `.tinted()` | Soft accent background |
| `.plain()` | Text only (default) |
| `.bordered()` | Outlined |

```js
Button("Delete", confirmDelete, { role: "destructive" }).bordered();
```

## Icon buttons

```js
Button.icon("settings", openSettings);                 // icon only
Button("Share", share, { icon: "share" }).tinted();    // icon + label
```

Icon-only buttons should provide an accessibility label:

```js
Button.icon("settings", openSettings).a11yLabel("Settings");
```

## Loading state

```js
const [saving, setSaving] = useState(false);
Button("Save", async () => { setSaving(true); await save(); setSaving(false); },
  { loading: saving }).filled();
```

## Sizing & tap targets

Buttons meet the 44 px minimum target by default; use `.hitSlop(n)` to enlarge small
custom buttons without changing layout ([../../guides/gestures.md](../../guides/gestures.md)).

## C

```c
Filled(Button(ZACT({ save(); }), "Save"));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
