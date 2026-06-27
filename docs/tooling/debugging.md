# Debugging

Tools for inspecting a running Zelto app: logs, the remote inspector, and the view-tree
inspector.

## Logs

```sh
zelto logs                       # simulator app logs
zelto logs --device              # on-device logs
zelto logs --device --filter "[net]"
```

`console.log/warn/error` from Zelto Script and C `z_log(...)` both stream here
([../zelto-script/stdlib.md](../zelto-script/stdlib.md)).

## Remote inspector

Attach a debugger to the running Zelto Script VM:

```sh
zelto debug
```

Provides:
- **Breakpoints** and stepping in your Script modules.
- **Console** with live evaluation in the app context.
- **Call stacks** and exception breakpoints.

Source maps from the bundle map back to your original files
([../zelto-script/runtime.md](../zelto-script/runtime.md)).

## View-tree inspector

Inspect the live UI:

```sh
zelto inspect
```

- Browse the rendered view tree and each node's resolved layout, style tokens, and
  modifiers.
- Highlight a node on the device/simulator screen.
- Toggle **layout bounds** and **repaint flashing** to spot over-rendering.

## Common issues

| Symptom | Look at |
|---|---|
| UI not updating | Are you mutating state without `setState`/`z_invalidate`? ([../guides/state-management.md](../guides/state-management.md)) |
| Jank while scrolling | Heavy `row` builders; use `List` recycling ([../ui/components/list.md](../ui/components/list.md)); profile ([profiling.md](profiling.md)) |
| Permission errors | Declared in manifest? Granted at runtime? ([../platform/permissions.md](../platform/permissions.md)) |
| Crash on launch | Check `zelto logs`; validate manifest with `zelto doctor` |
| Native module fails | ABI/build errors — `zelto build --verbose` ([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)) |

## Assertions

```js
console.assert(cond, "message");
```

```c
z_assert(cond, "message");   // logs + breaks in debug builds
```

## Crash reports

Debug builds capture a stack trace and the last log lines on crash, written to app
storage and surfaced by `zelto logs`. Release crash reporting is opt-in.

## See also

- [profiling.md](profiling.md) — performance.
- [simulator.md](simulator.md) — inner-loop dev.
