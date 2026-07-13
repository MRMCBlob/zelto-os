// The `zelto/net` module: HTTP, shaped like fetch.
//
// The request is driven by libzelto's asynchronous state machine, which is parked
// in the app loop — the socket is pumped between frames, so nothing here blocks
// the UI. `await` suspends your script, not the renderer.
//
// Gated by the `network` permission: the first call awaits the system consent
// dialog, and a denial REJECTS rather than silently doing nothing. Declare it in
// the manifest (`permissions=network`) or the request is refused outright.
//
// The transport is plain HTTP/1.x to a numeric IPv4 host (DNS and TLS are the
// platform's next step, not this module's) — see docs/guides/networking.md.
import * as N from "zelto:native";
import { ensure } from "zelto/permissions";

function headerKey(headers, name) {
  return Object.keys(headers).find((k) => k.toLowerCase() === name);
}

// fetch(url, { method, headers, body }) -> Promise<Response>
//
// A non-string `body` is sent as JSON (and typed as such unless you said
// otherwise). A transport failure rejects; an HTTP error status resolves with
// ok === false — the same split fetch() makes on the web, and the useful one: a
// 404 is an answer, a dead socket is not.
export async function fetch(url, options = {}) {
  await ensure("network");

  const method = (options.method || "GET").toUpperCase();
  const headers = { ...(options.headers || {}) };
  let body = options.body ?? null;

  if (body !== null && typeof body !== "string") {
    body = JSON.stringify(body);
    if (!headerKey(headers, "content-type")) {
      headers["content-type"] = "application/json";
    }
  }

  const res = await N.netSend(method, url, headers, body);
  return {
    status: res.status,
    ok: res.ok,
    headers: res.headers,          // lower-cased keys
    text: () => res.body,
    json: () => JSON.parse(res.body),
  };
}

export function get(url, options = {}) {
  return fetch(url, { ...options, method: "GET" });
}

export function post(url, body, options = {}) {
  return fetch(url, { ...options, method: "POST", body });
}
