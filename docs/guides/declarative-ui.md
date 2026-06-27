# Declarative UI

Zelto's UI is **declarative**: you write a function that returns a view tree describing
what the UI should look like *for the current state*. When state changes, you return a
new tree; the framework diffs it against the old one and updates only what changed. You
never imperatively mutate widgets.

This is the SwiftUI / React model, available in both Zelto Script and C.

## The body function

A component is a function that returns a view. The root component is your app's entry.

```js
import { VStack, Text } from "zelto/ui";

export default function App() {
  return VStack({ spacing: 8 }, [
    Text("Hello"),
    Text("World"),
  ]);
}
```

Views are **values**, not objects you hold onto. `Text("Hello")` describes a label; it is
cheap to create and thrown away each rebuild.

## Composition

Split UI into small components — just functions that return views:

```js
function Avatar({ url, size = 40 }) {
  return Image(url).frame(size, size).cornerRadius(size / 2);
}

function Row({ user }) {
  return HStack({ spacing: 12 }, [
    Avatar({ url: user.avatar }),
    Text(user.name).font("body"),
  ]);
}
```

In C, components are functions returning `ZView`:

```c
static ZView Avatar(const char *url, float size) {
  return CornerRadius(size / 2,
           Frame(size, size, Image(url)));
}
```

## State drives rebuilds

State lives in the component via `useState` (Script) or app state + `z_invalidate` (C).
Changing it requests a rebuild. The full model is in
[state-management.md](state-management.md).

```js
const [open, setOpen] = useState(false);
return Button(open ? "Close" : "Open", () => setOpen(!open));
```

## Modifiers

Styling and behavior are applied as **chained modifiers** that wrap a view and return a
new view:

```js
Text("Tap me")
  .padding(12)
  .background("surface")
  .cornerRadius("md")
  .onTap(() => doThing());
```

Order matters: modifiers wrap outward (here padding is inside the background). See
[styling-theming.md](styling-theming.md).

## Lists and conditionals

Build views from data with normal language constructs:

```js
VStack({}, items.map(it => Row({ user: it })));

open && Text("Now visible");        // conditional child
```

For long, scrolling, recycled lists use `List`, not a mapped `VStack` — see
[../ui/components/list.md](../ui/components/list.md).

## How rendering works (brief)

The returned tree is diffed into a retained **scene graph** that `libzelto` renders on
the GPU through `zcomp`. Only changed nodes are re-laid-out and re-rasterized, and only
damaged screen regions are recomposited. Details:
[../contributing/sdk-internals.md](../contributing/sdk-internals.md).

## Next

- [state-management.md](state-management.md)
- [layout.md](layout.md)
- [../ui/components-index.md](../ui/components-index.md)
