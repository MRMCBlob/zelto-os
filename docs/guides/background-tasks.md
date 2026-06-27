# Background Tasks

Zelto limits background execution to preserve battery. Apps get short finish-up windows,
scheduled jobs, and a few declared long-running modes — modeled on iOS-style background
execution rather than unrestricted services.

> Long-running modes must be declared under `[capabilities] background = [...]` in
> `zelto.toml` ([../packaging/manifest.md](../packaging/manifest.md)).

## The model

When your app leaves the foreground it is **suspended** soon after. To do work in the
background you must use one of:

1. A **finish task** — a short window to complete in-flight work.
2. A **scheduled job** — periodic/maintenance work the system runs when convenient.
3. A **background mode** — declared, ongoing work (audio, location, downloads).

## Finish tasks

Ask for a brief extension to complete something (e.g. flush a save) when backgrounded:

```js
import { beginFinishTask } from "zelto/platform";

const task = beginFinishTask("save-draft");
try {
  await saveDraft();
} finally {
  task.end();
}
```

The window is short (seconds). Don't rely on it for long work.

## Scheduled jobs

Register work to run periodically; the system batches it for efficiency:

```js
import { scheduler } from "zelto/platform";

scheduler.register("refresh-feed", {
  minInterval: "30m",
  requiresNetwork: true,
  requiresCharging: false,
}, async () => {
  await refreshFeed();
});
```

Intervals are advisory minimums; the system decides actual timing based on power and
usage. Jobs must finish quickly.

## Background modes

For genuinely continuous work, declare a capability and use its API:

| Mode | Use | API |
|---|---|---|
| `audio` | Playback continues in background | [../system-apis/media.md](../system-apis/media.md) |
| `location` | Continuous location updates | [../system-apis/location.md](../system-apis/location.md) |
| `downloads` | System-managed transfers | `zelto/net` `download` |

```toml
[capabilities]
background = ["audio"]
```

Undeclared modes are denied; declared modes still show user-visible indicators.

## Push-style wakeups

Server-initiated wakeups are delivered as notifications; tapping routes via deep links
([notifications.md](notifications.md)). Silent server-triggered background refresh is
scheduled through the job scheduler above, not arbitrary push execution.

## Lifecycle interaction

Background hooks pair with the lifecycle states in
[../platform/app-lifecycle.md](../platform/app-lifecycle.md). Save state on `pause`;
don't assume you'll run again before being killed.

## C API

```c
ZFinishTask *t = z_finish_task_begin("save-draft");
/* ... */
z_finish_task_end(t);

z_scheduler_register("refresh-feed", &opts, refresh_cb);
```

See [../api-reference/c/platform.md](../api-reference/c/platform.md).

## Next

- [../platform/app-lifecycle.md](../platform/app-lifecycle.md)
- [notifications.md](notifications.md)
