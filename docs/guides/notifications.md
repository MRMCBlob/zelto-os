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

## Next

- [background-tasks.md](background-tasks.md)
- [../platform/ipc-and-intents.md](../platform/ipc-and-intents.md)
