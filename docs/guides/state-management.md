# State Management

State is data that, when it changes, should update the UI. Zelto re-runs your component
function on state change and diffs the result. This page covers local state, derived
state, shared stores, and effects.

## Local state: `useState`

```js
import { useState } from "zelto";

export default function Counter() {
  const [count, setCount] = useState(0);
  return Button(`Count: ${count}`, () => setCount(count + 1));
}
```

- `useState(initial)` returns `[value, setValue]`.
- `setValue(next)` (or `setValue(prev => next)`) schedules a rebuild.
- State persists across rebuilds; it is keyed by call order, so call hooks
  unconditionally at the top level (same rule as React).

In **C**, state is a struct you own; request a rebuild with `z_invalidate`:

```c
typedef struct { int count; } State;

static ZView body(ZApp *app, State *s) {
  return Button(ZACT({ s->count++; z_invalidate(app); }), "Count: %d", s->count);
}
```

## Derived state

Compute values during render; don't store what you can derive:

```js
const [items, setItems] = useState([]);
const total = items.reduce((a, b) => a + b.price, 0);   // derived, not stored
return Text(`Total: ${total}`);
```

Memoize expensive derivations with `useMemo`:

```js
const sorted = useMemo(() => heavySort(items), [items]);
```

## Effects: `useEffect`

Run side effects (subscriptions, timers, fetches) after render:

```js
import { useEffect, useState } from "zelto";

const [now, setNow] = useState(Date.now());
useEffect(() => {
  const id = setInterval(() => setNow(Date.now()), 1000);
  return () => clearInterval(id);   // cleanup on unmount / dep change
}, []);                              // [] = run once
```

Network and storage calls are async — see [networking.md](networking.md) and
[storage.md](storage.md).

## Shared state: stores

For state shared across screens, use a **store**: a value container any component can
subscribe to.

```js
import { createStore, useStore } from "zelto";

export const cart = createStore({ items: [] });

// in a component:
function CartBadge() {
  const { items } = useStore(cart);
  return Text(`${items.length}`);
}

// anywhere:
cart.set(s => ({ items: [...s.items, product] }));
```

`useStore` subscribes the component and rebuilds it when the store changes. Stores are
plain modules, so they work across navigation.

## Forms and inputs

Bind inputs to state:

```js
const [name, setName] = useState("");
return TextField({ value: name, onChange: setName, placeholder: "Name" });
```

See [../ui/components/textfield.md](../ui/components/textfield.md).

## Rules of hooks

- Call hooks at the top level of a component, not inside loops/conditions.
- Hooks are for components only (functions that return views).

## Next

- [networking.md](networking.md) — async data.
- [navigation.md](navigation.md) — passing state across screens.
- [../api-reference/script/zelto-core.md](../api-reference/script/zelto-core.md) — hook signatures.
