# Styling & Theming

Styling in Zelto is done with **modifiers** that read from **design tokens**. Apps inherit
the system theme (the Zelto design language) and can override tokens for their own look,
including automatic light/dark and dynamic accent support.

See the token catalog in [../overview/design-language.md](../overview/design-language.md).

## Modifiers

Modifiers wrap a view and return a new view; chain them:

```js
Text("Hello")
  .font("title")
  .foreground("text")
  .padding(16)
  .background("surface")
  .cornerRadius("md")
  .shadow("elevation.1");
```

Common style modifiers:

| Modifier | Effect |
|---|---|
| `.font(token \| spec)` | Type style (`"title"`, `"body"`, …) |
| `.foreground(color)` | Text/icon color |
| `.background(color \| view)` | Fill or background view |
| `.padding(value)` | Inner spacing |
| `.cornerRadius(token \| n)` | Rounded corners |
| `.border(width, color)` | Stroke |
| `.shadow(token)` | Elevation shadow |
| `.opacity(0..1)` | Transparency |
| `.blur(radius)` | Background blur (for bars/sheets) |

Order matters — modifiers wrap outward. `.padding().background()` puts padding *inside*
the background; reverse them to pad *outside*.

## Tokens, not hard-coded values

Always prefer tokens so the app follows the system theme, dark mode, and accent:

```js
.background("surface")     // good — adapts to light/dark
.background("#ffffff")     // avoid — breaks in dark mode
.padding("space.4")        // good — consistent spacing scale
```

Token families: `color.*`, `space.*`, `radius.*`, `font.*`, `elevation.*`
([../overview/design-language.md](../overview/design-language.md)).

## Light / dark mode

Tokens resolve automatically based on the system appearance. Read the current scheme if
you must branch:

```js
import { useColorScheme } from "zelto/ui";
const scheme = useColorScheme();   // "light" | "dark"
```

Provide alternate assets with `Image.adaptive(lightUrl, darkUrl)` when needed.

## Accent color

The system supplies a dynamic accent (`color.accent`). Interactive components use it by
default (`Button().filled()`, selected tabs, switches). Avoid overriding it unless your
brand requires a fixed accent.

## App themes

Override tokens for your whole app or a subtree with a theme provider:

```js
import { Theme } from "zelto/ui";

export default function App() {
  return Theme({
    tokens: {
      "color.accent": { light: "#0a84ff", dark: "#0a84ff" },
      "radius.md": 18,
      "font.body": { size: 17, weight: "regular" },
    },
  }, RootView());
}
```

Themes nest; inner themes override outer ones for their subtree.

## Reusable styles

Factor repeated modifier chains into helper functions:

```js
const card = (v) => v.padding(16).background("surface").cornerRadius("md").shadow("elevation.1");

card(VStack({}, children));
```

## C styling

C uses modifier macros wrapping a `ZView`:

```c
Shadow(Z_ELEVATION_1,
  CornerRadius(Z_RADIUS_MD,
    Background(Z_COLOR_SURFACE,
      Padding(16, content))));
```

See [../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [animation.md](animation.md)
- [../ui/components-index.md](../ui/components-index.md)
