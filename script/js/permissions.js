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

// Ask for `name`, resolving to true/false once the user answers. A decision that
// is already cached comes back immediately, with no dialog.
export function request(name) {
  return N.permRequest(name);
}

// request(), but a denial is an ERROR rather than a false. This is what a
// capability call wants: `await ensure("network")` reads as a precondition, and a
// refusal rejects the caller's promise instead of quietly returning nothing.
export async function ensure(name) {
  const granted = await N.permRequest(name);
  if (!granted) {
    throw new Error(`permission denied: ${name}`);
  }
  return true;
}
