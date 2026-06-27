# Android (APK) Interop

Zelto runs Android apps in a container (Waydroid) and bridges them into the shell. This
page covers what app developers can do with installed Android apps and the limits of the
integration. Background: [../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md).

## What the APK bridge provides

- **Unified launcher** — installed APKs appear next to native apps.
- **Themed windows** — Android windows get Zelto decoration/animation as surfaces in
  `zcomp`; the app's *internal* UI stays Android.
- **Notification forwarding** — Android notifications appear in the Zelto shade.
- **Permission mapping** — Android runtime permissions surface through the Zelto prompt
  model ([permissions.md](permissions.md)).
- **Link & share routing** — deep links and shares can target Android apps.

## Launching an Android app

```js
import { intents } from "zelto/platform";

// By package (Android application id):
await intents.openApp("com.example.android", { /* extras */ });

// Or via a URL the Android app handles:
await intents.openUrl("https://maps.example.com/?q=cafe");
```

The system resolves the best handler (native or Android) and the bridge starts the
Android activity if needed.

## Sharing to / from Android apps

- **Share to Android:** native `share(...)` lists eligible Android targets
  ([../system-apis/share.md](../system-apis/share.md)).
- **Receive from Android:** declare `share-targets` and Android apps can share into your
  native app ([ipc-and-intents.md](ipc-and-intents.md)).

## Managing installed APKs (tooling)

During development:

```sh
zelto apk install app.apk
zelto apk list
zelto apk remove com.example.android
```

See [../getting-started/run-on-device.md](../getting-started/run-on-device.md).

## Limits (be honest with users)

- **You cannot restyle an Android app's internal UI** into the Zelto design language —
  only its window framing is themed.
- **Hardware passthrough** (camera/sensors/GPS) to Android apps depends on the device
  port and HAL bridges.
- **Google services** require microG/GApps in the container; some apps with Play
  Integrity checks may not run.
- **Performance** carries container overhead vs. a stock Android device.
- **Direct programmatic calls** into arbitrary Android APIs from native code are **not**
  exposed; interop is via launch/links/share/notifications, not Binder calls from your
  app.

## When to use native vs. APK

Build **native** Zelto apps for the best look, performance, and integration. Lean on APKs
for apps that only exist on Android, and integrate them through the bridge surfaces above.

## See also

- [../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md)
- [../contributing/services-and-ipc.md](../contributing/services-and-ipc.md) — the bridge internals.
