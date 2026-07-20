// android/widget.js — android.widget.Toast, translated onto the Zelto shade.
//
// A Toast is a fleeting message. Zelto has no free-floating overlay a guest can
// draw, so a Toast posts a low-importance system notification (which the shade
// shows as a brief heads-up banner) — the closest transient the platform gives a
// script app. Per the compat contract this must never crash: notifications are
// permission-gated, so if the grant is absent or the post is rejected the Toast
// quietly falls back to a console line instead of throwing (a Toast that failed to
// show is not an error an Android app is prepared to catch).
import * as notifications from "zelto/notifications";
import { warnUnsupported } from "android/compat";

export const LENGTH_SHORT = 0;
export const LENGTH_LONG = 1;

class Toast {
  constructor(text, duration) {
    this._text = text == null ? "" : String(text);
    this._duration = duration;
  }

  setText(text) {
    this._text = text == null ? "" : String(text);
    return this;
  }
  setDuration(duration) {
    this._duration = duration;
    return this;
  }

  // show() fires the notification and forgets it — the return is ignored by
  // Android callers. A denial/rejection degrades to a logged line.
  show() {
    notifications
      .post("", this._text, { importance: notifications.Importance.low })
      .catch(() => {
        warnUnsupported("Toast (notifications permission denied)");
        console.log(`[toast] ${this._text}`);
      });
  }

  cancel() {
    // Fire-and-forget banners self-dismiss; nothing to cancel.
  }
}

// Toast.makeText(context, text, duration) -> Toast (context is ignored — a Zelto
// notification is not tied to an Activity).
Toast.makeText = function (_context, text, duration) {
  return new Toast(text, duration);
};
Toast.LENGTH_SHORT = LENGTH_SHORT;
Toast.LENGTH_LONG = LENGTH_LONG;

export { Toast };
export default Toast;
