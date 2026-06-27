# Zelto Script API: `zelto/platform`

Lifecycle, permissions, intents/sharing, clipboard, haptics, and device info. Guides:
[../../platform/](../../platform/).

## Lifecycle

```js
import { app } from "zelto";
app.onLifecycle((ev) => {
  // ev: "launch" | "resume" | "pause" | "stop" | "terminate"
});
```

See [../../platform/app-lifecycle.md](../../platform/app-lifecycle.md).

## Permissions

```js
import { permissions } from "zelto/platform";

await permissions.status(name);     // "granted" | "denied" | "prompt"
await permissions.request(name);    // shows the system prompt → status
```

Names: `network`, `notifications`, `camera`, `microphone`, `location`, `contacts`, …
([../../platform/permissions.md](../../platform/permissions.md)).

## Intents, deep links & sharing

```js
import { intents, share } from "zelto/platform";

intents.onOpenUrl((url) => { /* incoming deep link or app handoff */ });
await intents.openUrl(url);                  // hand off to system / another app
await share({ text, url, files });           // present the share sheet
intents.onShareTarget((payload) => { … });   // receive shared content
```

See [../../platform/ipc-and-intents.md](../../platform/ipc-and-intents.md) and
[../../platform/apk-interop.md](../../platform/apk-interop.md).

## Clipboard

```js
import { clipboard } from "zelto/platform";
await clipboard.setText(str);
await clipboard.getText();
```

## Haptics

```js
import { haptics } from "zelto/platform";
haptics.impact("light");      // light | medium | heavy
haptics.notify("success");    // success | warning | error
```

## Biometrics

```js
import { biometrics } from "zelto/platform";
const ok = await biometrics.authenticate("Unlock notes");
```

See [../../system-apis/biometrics.md](../../system-apis/biometrics.md).

## Device info

```js
import { device } from "zelto/platform";
device.model; device.osVersion;
device.screen;        // { width, height, scale }
device.safeArea;      // { top, bottom, leading, trailing }
device.battery();     // 0..1
```
