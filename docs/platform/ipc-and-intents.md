# IPC, Intents & Deep Links

How apps communicate: deep links (URL routes), app-to-app handoff, share targets, and
custom inter-app messages. The Android-equivalent of "intents," adapted to Zelto.

## Deep links

Apps register URL routes; the system delivers matching links to the app.

```js
import { Navigator } from "zelto/ui";

Navigator({
  root: HomeScreen,
  routes: {
    "/item/:id": (p) => [DetailScreen, { id: p.id }],
    "/settings": () => [SettingsScreen, {}],
  },
});
```

Declare the URL scheme/host your app handles in the manifest
([../packaging/manifest.md](../packaging/manifest.md)):

```toml
[links]
schemes = ["myapp"]
hosts = ["example.com"]      # https links to this host open the app
```

Incoming links — from notifications, other apps, or the web — route here.

## Opening links / handing off

```js
import { intents } from "zelto/platform";

await intents.openUrl("https://maps.example.com/?q=cafe");   // system picks a handler
await intents.openUrl("tel:+15551234567");
intents.onOpenUrl((url) => handle(url));                     // your incoming links
```

If a link targets an installed Android app, the APK bridge routes it there
([apk-interop.md](apk-interop.md)).

## Share targets

Receive content shared from other apps; see [../system-apis/share.md](../system-apis/share.md).

```toml
[capabilities]
share-targets = ["text/*", "image/*"]
```

```js
intents.onShareTarget((payload) => importShared(payload));
```

## App-to-app messaging

For structured requests between Zelto apps (e.g. "pick a file from app X"), use typed
intents brokered by `zsysd`:

```js
import { intents } from "zelto/platform";

// caller
const result = await intents.request("dev.example.picker", "pickFile", { types: ["pdf"] });

// provider (declares it can handle the action in its manifest)
intents.onRequest("pickFile", async (params, ctx) => {
  const file = await letUserPick(params.types);
  return file;     // returned to the caller
});
```

Providers declare handled actions in the manifest:

```toml
[[provides]]
action = "pickFile"
```

All cross-app calls go through the `zsysd` broker, which enforces permissions and app
identity ([permissions.md](permissions.md)).

## Background delivery

If the target app is suspended, the system launches/resumes it to handle the request or
link, subject to background rules
([../guides/background-tasks.md](../guides/background-tasks.md)).

## C API

```c
z_open_url(url);
z_on_open_url(app, cb, ud);
z_on_share_target(app, cb, ud);
```

See [../api-reference/c/platform.md](../api-reference/c/platform.md).

## See also

- [apk-interop.md](apk-interop.md) — Android interop specifics.
- [../guides/navigation.md](../guides/navigation.md) — routing links to screens.
