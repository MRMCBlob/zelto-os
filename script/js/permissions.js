// The `zelto/permissions` module: asking the user, not the app.
//
// A sensitive capability (network, notifications, ...) is declared in the app's
// manifest and granted at runtime by the USER, through a consent dialog that
// belongs to the system (zsysd), not to the app. A script cannot draw it, fake
// it, or skip it.
//
// The grant is asynchronous because a human is in the loop, so `request` is a
// promise: `await` it and the SCRIPT suspends while the frame loop keeps running
// and the modal keeps drawing. That is the whole reason the capability modules
// (zelto/net, zelto/notifications) await a grant before they call anything —
// blocking on consent would freeze the very UI the user is answering.
import * as N from "zelto:native";

// "granted" | "denied" | "prompt" — a fast synchronous read of the cached
// decision. "prompt" means declared in the manifest but not yet decided.
export function status(name) {
  const s = N.permStatus(name);
  if (s === N.PERM_GRANTED) return "granted";
  if (s === N.PERM_DENIED) return "denied";
  return "prompt";
}

// Concurrent requests for the SAME permission must not each reach the broker: it
// allows only ONE consent dialog in flight, so a second native permRequest for a
// permission already being asked would be denied outright. The "request once,
// open many" shape hits this naturally — an app that opens the accelerometer AND
// the magnetometer to fuse orientation, before either grant has landed, fires two
// concurrent requests for `sensors`. We coalesce them: the first request for a
// name starts the native call and caches its promise; concurrent callers await
// that SAME promise; the entry is cleared once it settles, so a later, separate
// ask still prompts afresh (e.g. the user revisits a feature after denying).
const inFlight = new Map();

// Ask for `name`, resolving to true/false once the user answers. A decision that
// is already cached comes back immediately, with no dialog. Concurrent asks for
// the same name share one broker request (see above).
export function request(name) {
  const pending = inFlight.get(name);
  if (pending) return pending;
  // Promise.resolve flattens the native promise; .finally clears the cache on
  // settle (grant OR denial) without altering the resolved value.
  const p = Promise.resolve(N.permRequest(name)).finally(() => {
    inFlight.delete(name);
  });
  inFlight.set(name, p);
  return p;
}

// request(), but a denial is an ERROR rather than a false. This is what a
// capability call wants: `await ensure("network")` reads as a precondition, and a
// refusal rejects the caller's promise instead of quietly returning nothing.
// Goes through request() so it shares the same in-flight coalescing.
export async function ensure(name) {
  const granted = await request(name);
  if (!granted) {
    throw new Error(`permission denied: ${name}`);
  }
  return true;
}
