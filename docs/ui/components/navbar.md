# Navigator / NavBar

The `Navigator` manages a stack of screens; the navigation bar shows the title and bar
items for the current screen. See [../../guides/navigation.md](../../guides/navigation.md)
for pushing/popping and deep links.

## Navigator

```js
import { Navigator } from "zelto/ui";

export default function App() {
  return Navigator({ root: HomeScreen });
}
```

| Option | Type | Notes |
|---|---|---|
| `root` | component | Initial screen |
| `routes` | object | URL → screen, for deep links |

## Per-screen bar options

Each screen declares its bar via `options`:

```js
function DetailScreen() { /* ... */ }

DetailScreen.options = {
  title: "Details",
  largeTitle: true,                       // large title that collapses on scroll
  leading: () => Button.icon("chevron-left", () => nav.pop()),
  trailing: () => Button.icon("share", share),
  transparent: false,                     // blur over content when true
};
```

| Option | Type | Notes |
|---|---|---|
| `title` | string | Center/leading title |
| `largeTitle` | boolean | iOS-style large title |
| `leading` / `trailing` | `() => view` | Bar items |
| `transparent` | boolean | Blur bar over scrolled content |
| `hidden` | boolean | Hide the bar entirely |

## Large titles

With `largeTitle: true`, the title starts large and shrinks into the bar as the screen's
`Scroll` content moves up — wire the screen's scroll so the bar can track it.

## Back behavior

A leading back button and the edge-swipe-back gesture are provided automatically when a
screen is pushed; override `leading` to customize. Intercept with `useBackGuard`
([../../guides/gestures.md](../../guides/gestures.md)).

## C

```c
Navigator(.root = HomeScreen);
// screen options via Z_SCREEN_OPTIONS(...)
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
