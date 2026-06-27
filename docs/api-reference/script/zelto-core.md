# Zelto Script API: `zelto` (core)

The core module: hooks, stores, and app utilities. Import from `"zelto"`. Conventions:
[../conventions.md](../conventions.md).

## Hooks

Call at the top level of a component (a function that returns a view), unconditionally.

### `useState`

```js
const [value, setValue] = useState(initial);
```

Returns current value and a setter. `setValue(next)` or `setValue(prev => next)`
schedules a rebuild. State persists across rebuilds, keyed by call order.

### `useEffect`

```js
useEffect(() => {
  // side effect
  return () => { /* cleanup */ };
}, [deps]);
```

Runs after render when `deps` change; `[]` runs once on mount. The returned function runs
on unmount or before the next run.

### `useMemo` / `useCallback`

```js
const sorted = useMemo(() => heavySort(items), [items]);
const onTap  = useCallback(() => doThing(id), [id]);
```

Memoize an expensive value / a stable callback across rebuilds.

### `useRef`

```js
const ref = useRef(initial);   // ref.current is mutable, does not trigger rebuilds
```

## Stores (shared state)

```js
import { createStore, useStore } from "zelto";

export const cart = createStore({ items: [] });

cart.get();                        // current value
cart.set(s => ({ items: [...s.items, p] }));
cart.subscribe(fn);                // returns unsubscribe

function Badge() {
  const { items } = useStore(cart);   // subscribes + rebuilds on change
  return Text(`${items.length}`);
}
```

See [../../guides/state-management.md](../../guides/state-management.md).

## App

```js
import { app } from "zelto";

app.quit();
app.version;                 // string from the manifest
app.id;                      // package id
app.onLifecycle(cb);         // see ../../platform/app-lifecycle.md
```

## Timers & async

Standard `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, `queueMicrotask`,
and `Promise` are available (see [../../zelto-script/stdlib.md](../../zelto-script/stdlib.md)).

## Logging

```js
console.log(...), console.warn(...), console.error(...)
```

Output is visible via `zelto run` and the inspector
([../../tooling/debugging.md](../../tooling/debugging.md)).

## Module map

| Import | Area | Reference |
|---|---|---|
| `zelto` | hooks, stores, app | this page |
| `zelto/ui` | components, modifiers | [ui.md](ui.md) |
| `zelto/net` | http, websockets | [net.md](net.md) |
| `zelto/storage` | prefs, files, db, secure | [storage.md](storage.md) |
| `zelto/notifications` | notifications | [notifications.md](notifications.md) |
| `zelto/media` | audio/video/images | [media.md](media.md) |
| `zelto/sensors` | sensors, location | [sensors.md](sensors.md) |
| `zelto/platform` | lifecycle, permissions, intents | [permissions.md](permissions.md) |
| `native:<name>` | your C modules | [../../guides/interop-c-and-script.md](../../guides/interop-c-and-script.md) |
