# Sharing

Send content to other apps (native or Android) via the system share sheet, and receive
content shared to your app. API: `zelto/platform`. Related:
[../platform/ipc-and-intents.md](../platform/ipc-and-intents.md).

## Share out

```js
import { share } from "zelto/platform";

await share({
  text: "Check this out",
  url: "https://example.com",
  files: [fileHandle],     // optional attachments
});
```

The system presents a share sheet listing eligible targets — including installed Android
apps surfaced through the APK bridge ([../platform/apk-interop.md](../platform/apk-interop.md)).

## Receive shares (be a share target)

Declare what content types your app accepts:

```toml
[capabilities]
share-targets = ["text/*", "image/*"]
```

Handle incoming shares:

```js
import { intents } from "zelto/platform";

intents.onShareTarget((payload) => {
  // payload = { type, text?, url?, files? }
  importShared(payload);
});
```

Your app appears in other apps' share sheets for the declared types, and launches (or
resumes) to handle the payload.

## Files

Shared files arrive as handles readable via `zelto/storage`
([../guides/storage.md](../guides/storage.md)); copy what you need into app storage, since
the handle may be temporary.

## See also

- [../platform/ipc-and-intents.md](../platform/ipc-and-intents.md) — deep links & intents.
- [../platform/apk-interop.md](../platform/apk-interop.md) — sharing with Android apps.
