# Zelto Script: Runtime

How Zelto Script executes, and what that means for performance and limits.

## Status

The runtime is **live** ([`script/`](../contributing/repo-layout.md)): `zelto-script
ENTRY.js` boots a QuickJS context bound to `libzelto` and runs the app loop, and the
shipped **JS Demo** app (`system/apps/jsdemo/`) is written entirely in it.

| Available today | Not yet |
|---|---|
| ES modules: `zelto`, `zelto/ui`, `zelto/storage`, `zelto/net`, `zelto/notifications`, `zelto/settings`, `zelto/intents`, `zelto/permissions`, `zelto/sensors`, plus the app's own relative imports | Media, camera, background tasks — the system APIs the C SDK does not have yet either |
| Sensors + location: `zelto/sensors` (accelerometer/gyroscope/orientation/…, GPS), and the **andemu** Android-compat route ([android-compat.md](android-compat.md)) that maps `android.*` onto it | Real sensor HAL (values are simulated today), `always` background location |
| Hooks: `useState`, `useEffect`, `useMemo`, `useCallback`, `useRef`, `useAnimatedValue`, `useTextField` | `List`/`Grid` virtualisation, widgets, `z_db_*` (SQLite) and the file APIs |
| Views: stacks, `Text`, `Button`, `Rect`, `Image`, `Spacer`, `Scroll`, `TextField`, `Navigator`, chainable modifiers, the full design-token palette | `Stroke`, `Shadow`-driven custom drawing, `Widget` |
| Gestures + motion: `onTap`, `onPan`, `onLongPress`, spring-backed animated values with the named motion tokens (Reduce Motion honoured) | Multi-touch, pinch/rotate |
| Globals: `console`, `setTimeout`/`setInterval`, `queueMicrotask`, promises + `async/await`, `fetch` (via `zelto/net`), the ECMAScript standard library | `crypto`, `TextEncoder`/`TextDecoder`, `structuredClone`, WebSockets |
| Packaging: ships as a signed `.zap` (`script=` payload), installed by `zelto-install` | AOT bytecode (`zelto build`), the remote inspector, `native:*` C modules, TypeScript-annotation erasure |

A script app is an ordinary app to the rest of the OS: it maps a normal window, appears
in the launcher and switcher, receives intents, posts notifications, and its permissions
and storage are scoped to the `id` its manifest passes as `--id`
([../packaging/manifest.md](../packaging/manifest.md)). Anything a C app can reach, a
script app can reach — through the same brokered system services, with the same consent
prompts, and shipped in the same signed package format.

## Permissions are awaited, never blocked on

A capability the user must approve (`network`, `notifications`) is asked for through the
same zsysd consent dialog a C app raises — a script cannot draw it, skip it, or fake it.
Because a human is in the loop, the ask is a **promise**:

```js
import * as net from "zelto/net";

// The `network` grant is awaited inside net.fetch: the consent modal runs on the live
// frame loop while YOUR script suspends. A denial rejects, so it lands in the same
// catch as a dead socket.
const res = await net.get("http://10.0.2.2:8080/hello");
if (res.ok) console.log(res.json());
```

Awaiting suspends the script, not the app loop — the UI keeps drawing (which is the
point: the user is answering a dialog rendered by that very loop).

## The engine

Zelto Script runs on an embedded [QuickJS](https://bellard.org/quickjs/) interpreter —
small, fast to start, and ES2020+ compliant. The engine is hosted by the app process and
bound to `libzelto` and the system APIs.

```
your .js  --bundle-->  bytecode  -->  QuickJS  <-->  libzelto / system APIs (C)
                                          |
                                     app loop thread
```

`zelto build` will bundle and compile your modules to QuickJS bytecode ahead of time, so
the app doesn't parse source at launch
([../packaging/zap-format.md](../packaging/zap-format.md)). Until it does, the runtime
loads and compiles the source modules at launch — fine at this size (QuickJS parses the
demo app in single-digit milliseconds), and transparent to the app either way.

## Execution model

- A single **app loop** runs your code, timers, promise jobs, and UI rebuilds. Treat it
  as single-threaded (like a browser's main thread).
- Don't block it: use `async/await` for I/O, and offload heavy compute to a **native C
  module** ([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)).
- Your component function IS the rebuild: the framework calls it to produce the view
  tree, then lays out and paints in C. A tap runs your JS closure and rebuilds; a spring
  in flight rebuilds per frame. So keep the component cheap — it is on the frame path.
- Views are arena memory owned by the build that made them. Store *state* between
  renders, never a view: one from a previous render is rejected, not dereferenced.

## Performance guidance

- **UI logic in Script is fine.** The hot path (layout, compositing, animation) is C.
- **Avoid per-frame `setState` loops** — use `useAnimatedValue` for continuous motion.
- **Memoize** expensive derivations (`useMemo`) and keep `List`/`Grid` `row` builders
  cheap ([../ui/components/list.md](../ui/components/list.md)).
- **Drop to C** for image/audio/data processing, parsing, crypto-heavy work.

## Memory

- QuickJS is garbage-collected (reference counting + cycle collector). Don't hold
  unbounded caches; release subscriptions in `useEffect` cleanups.
- Binary data uses typed arrays backed by native buffers; large buffers should be freed
  by dropping references.

## Limits & sandbox

- No dynamic code eval; modules are resolved at build time.
- App memory and background time are bounded by the system
  ([../guides/background-tasks.md](../guides/background-tasks.md)).
- Each app runs in its own sandboxed process
  ([../platform/permissions.md](../platform/permissions.md)).

## Debugging

The runtime exposes a remote inspector (breakpoints, console, profiler) via
`zelto run`/`zelto debug` — see [../tooling/debugging.md](../tooling/debugging.md) and
[../tooling/profiling.md](../tooling/profiling.md).

## Next

- [../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)
- [../tooling/profiling.md](../tooling/profiling.md)
