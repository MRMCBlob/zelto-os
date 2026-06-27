# Zelto Script API: `zelto/notifications`

Post and manage notifications. Requires the `notifications` permission. Guide:
[../../guides/notifications.md](../../guides/notifications.md).

## Post

```js
import { notify } from "zelto/notifications";

const id = await notify({
  title, body, channel,
  actions?: [{ id, title }],
  tapRoute?: "/path",        // deep link on tap
  ongoing?: false,           // non-dismissable progress
  id?: existingId,           // pass to update in place
});
```

## Channels

```js
import { notifications } from "zelto/notifications";

notifications.defineChannel({ id, name, importance });   // min|low|default|high
```

## Manage

```js
await notifications.cancel(id);
await notifications.cancelAll();
notifications.setBadge(count);          // app icon badge; 0 clears
```

## Handle actions / taps

```js
import { onNotificationAction } from "zelto/notifications";

onNotificationAction((e) => {
  // e = { notificationId, actionId }   (actionId null = body tapped)
});
```

Body taps with a `tapRoute` are also delivered to navigation deep links
([../../guides/navigation.md](../../guides/navigation.md)).
