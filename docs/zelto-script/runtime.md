# Zelto Script: Runtime

How Zelto Script executes, and what that means for performance and limits.

## The engine

Zelto Script runs on an embedded [QuickJS](https://bellard.org/quickjs/) interpreter —
small, fast to start, and ES2020+ compliant. The engine is hosted by the app process and
bound to `libzelto` and the system APIs.

```
your .js  --bundle-->  bytecode  -->  QuickJS  <-->  libzelto / system APIs (C)
                                          |
                                     app loop thread
```

`zelto build` bundles and compiles your modules to QuickJS bytecode ahead of time, so the
app doesn't parse source at launch ([../packaging/zap-format.md](../packaging/zap-format.md)).

## Execution model

- A single **app loop** runs your code, timers, promise jobs, and UI rebuilds. Treat it
  as single-threaded (like a browser's main thread).
- Don't block it: use `async/await` for I/O, and offload heavy compute to a **native C
  module** ([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)).
- UI rendering and animation run on separate compositor/render threads in `libzelto`, so
  smooth scrolling/animation doesn't depend on the JS loop
  ([../guides/animation.md](../guides/animation.md)).

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
