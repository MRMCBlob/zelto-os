# Zelto Script: Standard Library

The built-in globals available to every Zelto Script app, independent of the `zelto/*`
modules. This is a curated subset of the JavaScript standard library plus a few Zelto
additions. For platform features (UI, net, storage…) see
[../api-reference/script/zelto-core.md](../api-reference/script/zelto-core.md).

## Globals

| Global | Notes |
|---|---|
| `console` | `log`, `warn`, `error`, `debug`, `assert` |
| `setTimeout` / `clearTimeout` | Timers |
| `setInterval` / `clearInterval` | Repeating timers |
| `queueMicrotask` | Microtask scheduling |
| `Promise` | Full promise support + `async/await` |
| `AbortController` / `AbortSignal` | Cancellation (used by `fetch`) |
| `structuredClone` | Deep clone |
| `crypto` | `randomUUID`, `getRandomValues`, `subtle` (digest/HMAC) |

## Built-in objects

Standard ECMAScript objects: `Object`, `Array`, `Map`, `Set`, `WeakMap`, `WeakSet`,
`Math`, `Date`, `JSON`, `RegExp`, `Number`, `String`, `Boolean`, `Symbol`, `BigInt`,
`Error` (and subclasses), `Proxy`, `Reflect`.

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
