// The `zelto/intents` module: handing content to another app, without naming it.
//
// An intent is deliberately anonymous. `share` says "somebody take this text" and
// the system shows the chooser; `openUrl` says "whoever owns this scheme, here" —
// neither call knows, or can discover, which app answers. zsysd resolves the
// candidates from installed manifests, launches the target if it is not running,
// and delivers the payload.
//
// The receiving half is the mirror image: an app declares what it accepts in its
// manifest (`share_targets=text/plain`, `links=myapp`) and registers a handler.
// Register it at STARTUP — an app that was launched to handle a share must have
// its handler in place to receive the payload it was launched for (the runtime
// replays an intent that arrived first, so registration never races the delivery).
import * as N from "zelto:native";

function normalize(item) {
  if (typeof item === "string") {
    return { mime: "text/plain", text: item };
  }
  return {
    mime: item.mime || "text/plain",
    text: String(item.text ?? ""),
  };
}

// share("hello") / share([{ mime, text }, ...]) — presents the system share sheet.
export function share(items) {
  const list = Array.isArray(items) ? items : [items];
  return N.share(list.map(normalize));
}

export function shareText(text) {
  return share([{ mime: "text/plain", text: String(text) }]);
}

// Hand a URL to whichever app registered its scheme (a deep link). No-op if
// nothing handles it.
export function openUrl(url) {
  N.openUrl(String(url));
}

// This app was deep-linked: fn(url).
export function onOpenUrl(fn) {
  N.onOpenUrl(fn);
}

// Another app shared to us: fn([{ mime, text }, ...]).
export function onShareTarget(fn) {
  N.onShareTarget(fn);
}
