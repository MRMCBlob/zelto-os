# Notifications

Apps post notifications to the Zelto notification shade. Notifications support channels,
actions, and updates, and require the `notifications` permission.

> Declare `notifications = true` in `zelto.toml`; the user grants it at runtime
> ([../platform/permissions.md](../platform/permissions.md)).

## Post a notification

```js
import { notify } from "zelto/notifications";

await notify({
  title: "Build finished",
  body: "hello-1.0.0.zap is ready",
  channel: "builds",
});
```

## Channels

Channels group notifications so users can tune importance per type. Register them at
startup:

```js
import { notifications } from "zelto/notifications";

notifications.defineChannel({
  id: "builds",
  name: "Build results",
  importance: "default",   // min | low | default | high
});
```

## Actions

Add buttons; handle taps via the app's intent handler:

```js
await notify({
  title: "New message",
  body: text,
  channel: "messages",
  actions: [
    { id: "reply", title: "Reply" },
    { id: "mark-read", title: "Mark read" },
  ],
  tapRoute: `/chat/${chatId}`,    // deep link when the body is tapped
});
```

Handle the action / tap:

```js
import { onNotificationAction } from "zelto/notifications";

onNotificationAction((e) => {
  if (e.actionId === "mark-read") markRead(e.notificationId);
});
```

`tapRoute` is delivered through navigation deep links
([navigation.md](navigation.md), [../platform/ipc-and-intents.md](../platform/ipc-and-intents.md)).

## Updating & removing

```js
const id = await notify({ title: "Downloading…", body: "0%", channel: "downloads", ongoing: true });
await notify({ id, body: "60%" });       // update in place
await notifications.cancel(id);          // remove
```

`ongoing: true` marks a non-dismissable progress notification (clear it yourself).

## Badges

```js
notifications.setBadge(3);   // app icon badge count
notifications.setBadge(0);   // clear
```

## APK notifications

Android app notifications are forwarded into the same shade by the APK bridge. Your app
doesn't manage those; see [../platform/apk-interop.md](../platform/apk-interop.md).

## C API

```c
ZNotification *n = z_notify_new("Build finished", "ready");
z_notify_set_channel(n, "builds");
z_notify_post(n);
```

See [../api-reference/c/system.md](../api-reference/c/system.md).

## Displaying notifications: the sink contract

Everything above is the POSTING side. The other side — the surfaces that actually
draw a notification — is a separate contract, and it is the one you need if you
are writing System UI rather than an app.

A **sink** is a surface that has called `z_notify_subscribe()`. zsysd records it
by its persistent control fd and pushes every posted notification to it as
`notify_show` (and `notify_hide` when it is cancelled or expires).

**Sinks are a SET, not a single registration.** More than one surface may
subscribe, and each one receives every notification. This matters because Zelto
has at least two surfaces that draw the same notification at the same time: the
heads-up banner (`system/shade`) and the lock screen (`system/lock`), which
between them cover "you are using the phone" and "the phone is on the table".

That was not always true, and the way it failed is worth knowing, because
nothing about it looks like a bug at the call site. The sink used to be one fd —
literally "the shade sink" — so subscribing was LAST-WINS. `/init` starts the
lock screen after the shade, so the lock screen's subscription silently replaced
the shade's, and the heads-up banner simply stopped existing. No error, no log:
one surface quietly took the other's notifications.

Consequences for anything that subscribes:

- **Subscribing does not displace anyone.** Add a surface freely.
- **Expect to be one of several.** Do not assume you are the only one showing a
  given notification, and do not treat receiving one as ownership of it.
- **Any sink may report an action tap**, and zsysd routes it to the posting app's
  mailbox. A notification acted on from the lock screen and the same one acted on
  from the banner are indistinguishable to the poster, which is the intent.
- **Subscribing twice on one fd is a no-op**, so a surface that re-subscribes
  after a rebuild does not receive doubles.
- **A sink is dropped automatically when its connection closes.** A surface that
  exits needs no teardown call.

The settings-observer set (`z_settings_observe`) works the same way and for the
same reason; the two fan-outs mirror each other in `system/zsysd/main.c`.

## Next

- [background-tasks.md](background-tasks.md)
- [../platform/ipc-and-intents.md](../platform/ipc-and-intents.md)
