# Zelto Script: Standard Library

The built-in globals available to every Zelto Script app, independent of the `zelto/*`
modules. This is a curated subset of the JavaScript standard library plus a few Zelto
additions. For platform features (UI, net, storage…) see
[../api-reference/script/zelto-core.md](../api-reference/script/zelto-core.md).

## Globals

| Global | Notes | Status |
|---|---|---|
| `console` | `log`, `info`, `debug`, `warn`, `error` | Available |
| `setTimeout` / `clearTimeout` | Timers, on the app loop | Available |
| `setInterval` / `clearInterval` | Repeating timers | Available |
| `queueMicrotask` | Microtask scheduling | Available |
| `Promise` | Full promise support + `async/await` | Available |
| `AbortController` / `AbortSignal` | Cancellation (used by `fetch`) | Planned |
| `structuredClone` | Deep clone | Planned |
| `crypto` | `randomUUID`, `getRandomValues`, `subtle` (digest/HMAC) | Planned |

Timers run on the same single loop as your component and your tap handlers
([runtime.md](runtime.md)), so a callback can call `setState` and the UI rebuilds — but a
callback that blocks stalls the frame.

## Built-in objects

Standard ECMAScript objects: `Object`, `Array`, `Map`, `Set`, `WeakMap`, `WeakSet`,
`Math`, `Date`, `JSON`, `RegExp`, `Number`, `String`, `Boolean`, `Symbol`, `BigInt`,
`Error` (and subclasses), `Proxy`, `Reflect`.

The ECMAScript standard library below is whatever QuickJS provides (ES2023), so
`Object`/`Array`/`Map`/`JSON`/`RegExp`/`BigInt` and friends are all there today. The
"Planned" rows above are *web platform* globals the host has yet to bind — they are not
part of the engine.

## Typed arrays & binary

`Uint8Array`, `Int8Array`, `Uint16Array`, `Int32Array`, `Float32Array`, `Float64Array`,
`ArrayBuffer`, `DataView`. Used for binary network/file data.

```js
const bytes = await (await fetch(url)).bytes();   // Uint8Array
```

## Text encoding

```js
const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = enc.encode("hello");
const str = dec.decode(bytes);
```

`btoa` / `atob` are available for base64.

## Crypto

```js
crypto.randomUUID();
crypto.getRandomValues(new Uint8Array(16));
const hash = await crypto.subtle.digest("SHA-256", bytes);
```

## Not included

- No DOM, `window`, `document`, `localStorage` — use `zelto/storage`.
- No `XMLHttpRequest` — use `zelto/net` `fetch`.
- No Node.js (`fs`, `process`, `require`) — use `zelto/*` modules and ES `import`.
- No `eval` / dynamic code generation in app sandboxes.

## Next

- [language.md](language.md) · [runtime.md](runtime.md)
- [../api-reference/script/zelto-core.md](../api-reference/script/zelto-core.md)
