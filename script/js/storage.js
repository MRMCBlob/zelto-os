// The `zelto/storage` module: the app's private key/value store.
//
// Backed by libzelto's prefs store (z_prefs_*), i.e. an app-scoped file on the
// persistent partition — it survives reboots and is invisible to other apps.
// Values are strings on the wire; get/set JSON for anything richer.
//
// Files and SQLite (z_file_*, z_db_*) are exposed to C today; the script
// bindings for them land with the rest of the system APIs.
import * as N from "zelto:native";

// The stored string, or `fallback` (default null) when the key is unset.
export function get(key, fallback = null) {
  const v = N.prefsGet(key);
  return v === null ? fallback : v;
}

export function set(key, value) {
  return N.prefsSet(key, String(value));
}

export function remove(key) {
  return N.prefsRemove(key);
}

// Numbers, with a NaN-safe fallback.
export function getNumber(key, fallback = 0) {
  const v = N.prefsGet(key);
  if (v === null) return fallback;
  const n = Number(v);
  return Number.isNaN(n) ? fallback : n;
}

export function setNumber(key, value) {
  return N.prefsSet(key, String(value));
}

// Objects/arrays, via JSON. A corrupt value reads as the fallback rather than
// throwing — a store is not a trusted parser.
export function getJSON(key, fallback = null) {
  const v = N.prefsGet(key);
  if (v === null) return fallback;
  try {
    return JSON.parse(v);
  } catch (e) {
    return fallback;
  }
}

export function setJSON(key, value) {
  return N.prefsSet(key, JSON.stringify(value));
}
