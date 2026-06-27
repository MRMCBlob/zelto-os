# API Conventions

Rules shared by both the C (`libzelto`) and Zelto Script APIs: naming, errors, async, and
memory. Read this once; individual reference pages assume it.

## Naming

| Surface | Convention | Example |
|---|---|---|
| Zelto Script components | PascalCase | `VStack`, `TextField` |
| Zelto Script functions | camelCase | `useState`, `fetch` |
| Zelto Script modules | `zelto/<area>` | `zelto/ui`, `zelto/net` |
| C types | `Z`-prefixed PascalCase | `ZView`, `ZApp`, `ZDatabase` |
| C functions | `z_` snake_case | `z_invalidate`, `z_db_open` |
| C macros (views) | PascalCase | `VStack`, `Button` |
| C enums | `Z_` SCREAMING | `Z_ALIGN_CENTER` |

## Errors

**Zelto Script** throws exceptions; use `try/catch` or `.catch` on promises:

```js
try { await fetch(url); } catch (e) { console.error(e.message); }
```

**C** functions return a status or `NULL`/`false` on failure and set the last error:

```c
ZDatabase *db = z_db_open("app.db");
if (!db) { fprintf(stderr, "%s\n", z_last_error()); }
```

Functions that can fail are documented with their failure mode. Native modules surface C
errors to Script with `z_throw(ctx, "message")`
([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)).

## Async

- **Zelto Script:** async APIs return promises; use `async/await`. Callbacks resume on
  the app (UI) loop thread.
- **C:** async APIs take a completion callback invoked on the app loop. Never block the
  app loop; offload heavy work to a thread and post results with `z_post`.

Long-running work off the UI thread must marshal back before touching UI or Script state.

## Threading

- The **app loop** owns UI and Script state. Build view trees and call `setState` /
  `z_invalidate` only on it.
- Worker threads are allowed for compute; communicate via `z_post` (C) or message
  channels (Script).
- `useAnimatedValue` updates run on the render thread and don't require the app loop
  ([../guides/animation.md](../guides/animation.md)).

## Memory (C)

- View macros allocate from a per-frame arena; you don't free views.
- Long-lived objects (`ZDatabase`, `ZAnimated`, retained `ZValue`) are reference-counted:
  `z_retain` / `z_release`.
- Strings/bytes crossing the Script↔C boundary are copied or retained by the runtime;
  don't keep raw pointers past a call without retaining.

## Permissions

APIs that touch private data or hardware require a declared permission and a runtime
grant. Each such API documents the permission it needs; the call rejects/throws if denied
([../platform/permissions.md](../platform/permissions.md)).

## Versioning & stability

- APIs marked **Planned** are not yet available in the current phase
  ([../README.md](../README.md#roadmap)).
- The manifest's `id`, `.zap` layout, and permission names are stable contracts; changes
  are versioned ([../packaging/manifest.md](../packaging/manifest.md)).

## See also

- [c/ui.md](c/ui.md) · [c/system.md](c/system.md) · [c/platform.md](c/platform.md) · [c/gfx.md](c/gfx.md)
- [script/zelto-core.md](script/zelto-core.md) and the other `script/` modules.
