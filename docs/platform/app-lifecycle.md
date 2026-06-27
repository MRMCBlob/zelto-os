# App Lifecycle

A Zelto app moves through a small set of states as the user opens, switches, and closes
it. Respond to transitions to save state, pause work, and resume cleanly.

## States

```
        launch
          │
          ▼
      ┌────────┐  resume   ┌─────────┐
      │ Active │◄──────────│ Inactive│
      │ (fg)   │──────────►│ (paused)│
      └────────┘  pause    └─────────┘
                                │ stop
                                ▼
                           ┌─────────┐  terminate
                           │ Stopped │───────────► (process killed)
                           │ (bg)    │
                           └─────────┘
```

| State | Meaning |
|---|---|
| **Active** | Foreground, receiving input. |
| **Inactive** | Transient (e.g. during a switch, a system prompt over the app). |
| **Stopped** | Backgrounded; soon suspended. May be terminated without further notice. |

## Handling transitions

```js
import { app } from "zelto";

app.onLifecycle((ev) => {
  switch (ev) {
    case "launch":   init(); break;
    case "resume":   refresh(); break;       // back to foreground
    case "pause":    saveDraft(); break;     // leaving foreground — persist now
    case "stop":     releaseResources(); break;
    case "terminate":/* last chance, best-effort */ break;
  }
});
```

In **C**: `z_on_lifecycle(app, cb, ud)` with `Z_LC_*` events
([../api-reference/c/platform.md](../api-reference/c/platform.md)).

## Rules of thumb

- **Save on `pause`.** Treat every `pause` as "might not run again." Persist user data and
  navigation state.
- **Release on `stop`.** Drop sensors, camera, timers, and large buffers; re-acquire on
  `resume`.
- **Don't assume `terminate` fires.** The system may kill a stopped app silently to
  reclaim memory.

## Background work

Anything beyond a brief finish window after `pause` needs an explicit mechanism —
finish tasks, scheduled jobs, or a declared background mode. See
[../guides/background-tasks.md](../guides/background-tasks.md).

## Restoring state

Persist enough on `pause` to restore the screen on next `launch`:

```js
app.onLifecycle((ev) => {
  if (ev === "pause") prefs.set("lastRoute", currentRoute());
});
// on launch, read it and navigate
```

Combine with deep-link routing ([ipc-and-intents.md](ipc-and-intents.md)) so the app can
open directly to a screen.

## Cold vs. warm start

- **Cold start:** process spawned fresh → `launch`. Keep launch work minimal; defer heavy
  loads until after first paint.
- **Warm resume:** process still alive → `resume`. Fast; just refresh stale data.

Measure both with [../tooling/profiling.md](../tooling/profiling.md).
