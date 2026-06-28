# Hello World

This walkthrough builds, runs, and edits your first Zelto app — in **Zelto Script** (the
recommended path) and in **C** (for native modules / maximum performance). It uses only
documented CLI commands and APIs.

Prerequisite: [install-sdk.md](install-sdk.md) and a working `zelto doctor`.

## 1. Scaffold

```sh
zelto new hello
cd hello
```

This creates:

```
hello/
  zelto.toml        # app manifest (see ../packaging/manifest.md)
  src/
    main.js         # entry point (Zelto Script)
  assets/
    icon.png
```

The generated `zelto.toml`:

```toml
[app]
id = "dev.example.hello"
name = "Hello"
version = "1.0.0"
icon = "assets/icon.png"
entry = "src/main.js"

[permissions]
notifications = false
```

## 2. The app (Zelto Script)

`src/main.js`:

```js
import { VStack, Text, Button } from "zelto/ui";
import { useState } from "zelto";

export default function App() {
  const [count, setCount] = useState(0);

  return VStack({ spacing: 12, padding: 16, align: "center" }, [
    Text(`Count: ${count}`).font("title"),
    Button("Increment", () => setCount(count + 1)).filled(),
  ]);
}
```

- `export default` is your root component — a function that returns a view tree.
- `useState` holds state; calling `setCount` re-runs the function and the framework
  updates only what changed. See [../guides/state-management.md](../guides/state-management.md).
- `.font(...)` and `.filled()` are **modifiers** (chained styling) — see
  [../guides/styling-theming.md](../guides/styling-theming.md).

## 3. Run it in the simulator

```sh
zelto run --simulator
```

A phone-sized window opens running your app. Edits to `src/main.js` hot-reload. See
[simulator.md](simulator.md).

## 4. Same app in C

For a native (C) app, set the entry to a native library and write the view tree with the
`libzelto` macros. `zelto.toml`:

```toml
[app]
id = "dev.example.hello"
name = "Hello"
version = "1.0.0"
entry = "lib"        # native entry; build produces libapp.so
```

`src/main.c`:

```c
#include <zelto/ui.h>

typedef struct { int count; } State;

// Tap handler: a named ZAction (C has no inline closures under strict ISO C).
static void increment(ZApp *app, void *state) {
  State *s = state;
  s->count++;
  z_invalidate(app);
}

static ZView body(ZApp *app, State *s) {
  (void)app;
  return VStack(
    Text("Count: %d", s->count),
    Button(increment, "Increment"),
    .spacing = 12, .padding = 16, .align = Z_ALIGN_CENTER);
}

Z_APP(State, body)   // declares the entry point + state type
```

- `VStack(...)` and friends are variadic macros that build the tree. Stack children
  are listed first; layout options follow as designated initializers.
- `Button(on_tap, fmt, ...)` takes a named `ZAction`; tapping it (pointer or, while
  focused, Enter/Space) runs the handler. `OnTap`/`OnKey` add gestures to any view.
- `z_invalidate(app)` requests a rebuild (the C equivalent of `setState`).
- `Z_APP(StateType, bodyFn)` generates the entry point. See
  [../api-reference/c/ui.md](../api-reference/c/ui.md).

Build + run:

```sh
zelto run --simulator
```

## 5. Build a package

```sh
zelto build --release      # produces hello-1.0.0.zap
```

See [../packaging/zap-format.md](../packaging/zap-format.md) and
[run-on-device.md](run-on-device.md) to install it on a phone.

## Where to go next

- [../guides/declarative-ui.md](../guides/declarative-ui.md) — the view-tree model.
- [../guides/layout.md](../guides/layout.md) — stacks, flex, frames.
- [../ui/components-index.md](../ui/components-index.md) — every built-in view.
