# Text

Displays text. Shaped with HarfBuzz + FreeType; styled via type tokens.

```js
Text("Hello world")
Text(`Count: ${n}`).font("title")
```

## Options

`Text(content, options?)` — `content` is a string.

| Option | Type | Default | Notes |
|---|---|---|---|
| `lines` | number | unlimited | Max lines before truncation |
| `truncate` | `"end" \| "middle" \| "none"` | `"end"` | When `lines` is set |
| `align` | `"leading" \| "center" \| "trailing"` | `"leading"` | Text alignment |
| `selectable` | boolean | `false` | Allow text selection |

## Common modifiers

| Modifier | Effect |
|---|---|
| `.font(token \| spec)` | Type style (`"body"`, `"title"`, or `{ size, weight }`) |
| `.foreground(color)` | Text color token |
| `.bold()` / `.italic()` | Weight / slant |

```js
Text("Subtitle")
  .font("callout")
  .foreground("text.secondary")
  .lines(2);
```

## Rich / styled runs

```js
Text.rich([
  { text: "Total: " },
  { text: "$42.00", weight: "bold", color: "accent" },
]);
```

## Dynamic type

Font tokens scale with the user's text-size setting; prefer tokens over fixed sizes so
text respects accessibility settings.

## C

```c
Font(Z_FONT_TITLE, Text("Count: %d", n));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
