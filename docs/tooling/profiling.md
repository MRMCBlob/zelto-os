# Profiling

Measure and fix performance: frame timing, GPU, CPU/JS, and memory. The target is a
steady 60/120 fps with no dropped frames during interaction.

## Frame profiler

```sh
zelto profile --device --frames
```

Reports per-frame:
- **Frame time** vs. the display budget (16.6 ms @ 60 Hz, 8.3 ms @ 120 Hz).
- **Dropped frames** (jank) and where they cluster.
- **Phase breakdown:** build (diff) · layout · paint/rasterize · composite.

Run on **device** for real numbers; the simulator is indicative only
([simulator.md](simulator.md)).

## Reading the breakdown

| Phase | Hot? Look at |
|---|---|
| Build/diff | Large trees rebuilt too often; memoize, split components ([../guides/state-management.md](../guides/state-management.md)) |
| Layout | Deep/complex stacks; over-flexing; oversized lists ([../guides/layout.md](../guides/layout.md)) |
| Paint | Expensive shadows/blur/custom canvas ([../api-reference/c/gfx.md](../api-reference/c/gfx.md)) |
| Composite | Too many layers/overlaps; large offscreen surfaces |

## JS / CPU profiler

```sh
zelto profile --js
```

Flame graph of Script execution to find hot functions. If app-loop work dominates a
frame, move it off the loop or into a **native C module**
([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)).

## Memory

```sh
zelto profile --memory
```

- JS heap (QuickJS), retained sizes, and growth over time (leak detection).
- Native allocations and GPU texture memory.
- Snapshot + diff to find what grew between two points.

Release subscriptions/timers in `useEffect` cleanups to avoid leaks
([../zelto-script/runtime.md](../zelto-script/runtime.md)).

## Animation performance

- Prefer `transform`/`opacity` (composited) over animating layout.
- Use `useAnimatedValue` for continuous/gesture motion so frames don't depend on the JS
  loop ([../guides/animation.md](../guides/animation.md)).
- Use `zelto inspect` repaint flashing to spot needless redraws
  ([debugging.md](debugging.md)).

## List performance

- Stable `key`, cheap `row`, estimated row heights
  ([../ui/components/list.md](../ui/components/list.md)).

## Startup

```sh
zelto profile --startup --device
```

Measures cold vs. warm start ([../platform/app-lifecycle.md](../platform/app-lifecycle.md)).
Defer heavy work until after first paint.

## CI budgets

`zelto profile --json` emits machine-readable metrics; assert budgets (e.g. p95 frame
time, startup ms) in CI to catch regressions.
