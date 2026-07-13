// The `zelto/notifications` module: posting to the system shade.
//
// A notification is not a window the app draws — it is a message handed to the
// zsysd broker, which routes it to the shade (the heads-up banner) and routes any
// tap back. So it outlives the app: a notification posted and then backgrounded
// still shows, and tapping its action button LAUNCHES the app if it is gone.
// That is why `onAction` must be registered at startup rather than when you post
// — an app started to handle its own action would otherwise miss the event it was
// started for (the runtime replays a queued action on registration).
//
// Gated by the `notifications` permission (`permissions=notifications` in the
// manifest); the first post awaits the system consent dialog.
import * as N from "zelto:native";
import { ensure } from "zelto/permissions";

// How prominently the shade presents a channel's notifications.
export const Importance = {
  min: N.IMPORTANCE_MIN,
  low: N.IMPORTANCE_LOW,
  default: N.IMPORTANCE_DEFAULT,
  high: N.IMPORTANCE_HIGH,
};

// Register a channel (an importance grouping the user can tune per-app).
export function defineChannel(id, name, importance = Importance.default) {
  N.notifyChannel(id, name, importance);
}

// post(title, body, { channel, tapRoute, actionId, actionTitle }) -> Promise<id>
//
// `tapRoute` is a deep link opened when the BODY is tapped; an action button
// instead routes back to this app's onAction handler. Resolves with the id the
// broker assigned (pass it to cancel()).
export async function post(title, body, options = {}) {
  await ensure("notifications");

  const id = N.notifyPost(String(title), String(body), options);
  if (id < 0) {
    throw new Error("the system rejected the notification");
  }
  return id;
}

export function cancel(id) {
  N.notifyCancel(id);
}

// The app-icon badge count (0 clears it).
export function setBadge(count) {
  N.notifyBadge(count);
}

// Called when the user taps one of this app's notification action buttons:
// `fn({ id, action })`. `action` is null when the notification BODY was tapped
// (its tapRoute, if any, has already been routed). Register once, at startup.
export function onAction(fn) {
  N.notifyOnAction(fn);
}
