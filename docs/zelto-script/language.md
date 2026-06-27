# Zelto Script: Language

Zelto Script is the primary language for Zelto apps. It is a **JS-like** language: if you
know JavaScript/TypeScript, you already know it. This page covers the syntax and the
small set of differences from standard JavaScript.

> Runtime: Zelto Script runs on an embedded [QuickJS](https://bellard.org/quickjs/)
> engine. See [runtime.md](runtime.md) for performance and limits.

## Syntax overview

Standard modern JS syntax is supported:

```js
// variables
const x = 1;
let y = 2;

// functions & arrows
function add(a, b) { return a + b; }
const mul = (a, b) => a * b;

// destructuring, spread, template literals
const { name, age } = user;
const all = [...a, ...b];
const greeting = `Hello ${name}`;

// classes
class Counter {
  count = 0;
  inc() { this.count++; }
}

// async
async function load() { return await fetch(url); }
```

## Types

Dynamically typed, same primitive set as JS: `number`, `string`, `boolean`, `null`,
`undefined`, `object`, `array`, `function`, plus `Promise`, `Map`, `Set`, `Uint8Array`.

### Optional type annotations

Zelto Script accepts **TypeScript-style annotations** for tooling (autocomplete,
checking). They are erased at build time:

```js
function greet(name: string): string {
  return `Hi ${name}`;
}
```

Annotations are optional everywhere; you can write plain JS.

## Modules

ES module syntax. Import system modules from `zelto/*`, native C modules from `native:*`,
and local files by relative path:

```js
import { VStack, Text } from "zelto/ui";
import { useState } from "zelto";
import { blur } from "native:imgproc";
import { formatDate } from "./util.js";

export default function App() { /* root component */ }
export function helper() {}
```

The app entry is the `export default` of the file named by `entry` in `zelto.toml`
([../packaging/manifest.md](../packaging/manifest.md)).

## Components

A component is a function returning a view ([../guides/declarative-ui.md](../guides/declarative-ui.md)).
By convention components are PascalCase:

```js
function Card({ title }) {
  return VStack({ padding: 16 }, [ Text(title).font("title") ]);
}
```

## Differences from standard JavaScript

- **No DOM / browser globals** (`window`, `document`, etc.). UI is the view tree.
- **No `eval`/`Function(string)`** in app sandboxes (security).
- **Module-scoped**, strict mode always on.
- A curated global set — see [stdlib.md](stdlib.md). Node.js APIs are not available; use
  `zelto/*` modules instead.

## Error handling

```js
try {
  await risky();
} catch (e) {
  console.error(e.message);
}
```

See [../api-reference/conventions.md](../api-reference/conventions.md).

## Next

- [stdlib.md](stdlib.md) — built-in globals.
- [runtime.md](runtime.md) — engine, performance, limits.
- [../guides/interop-c-and-script.md](../guides/interop-c-and-script.md) — calling C.
