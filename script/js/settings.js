// The `zelto/settings` module: the system's shared, live settings store.
//
// zsysd owns one source of truth for the system toggles (Wi-Fi, mute, brightness,
// Reduce Motion, ...), persists it across reboots, and pushes every change to
// every observer. So a value flipped in the Settings app updates a script that is
// watching it, live, with no polling and no reboot — and a value a script writes
// shows up in the shade's quick-settings chip the same way.
//
// The observer fires on EVERY change, including the script's own writes. Apply
// them idempotently (set the same state twice = no-op) and nothing loops.
//
// Reads are a fast synchronous round-trip; writes persist and broadcast.
import * as N from "zelto:native";

// The system keys worth naming. Any `sys.*` key can be read; these are the ones
// with meaning across the OS.
export const Keys = {
  wifi: "sys.wifi",
  mute: "sys.mute",
  airplane: "sys.airplane",
  brightness: "sys.brightness",
  volume: "sys.volume",
  reduceMotion: "sys.reduce_motion",
  wallpaper: "sys.wallpaper",
  batteryPercent: "sys.battery_pct",
  batteryCharging: "sys.battery_charging",
};

export function get(key, fallback = null) {
  const v = N.settingGet(key);
  return v === null ? fallback : v;
}

export function getInt(key, fallback = 0) {
  return N.settingGetInt(key, fallback);
}

// Booleans are stored as "0"/"1" on the wire.
export function getBool(key, fallback = false) {
  const v = N.settingGet(key);
  if (v === null) return fallback;
  return v === "1" || v === "true";
}

export function set(key, value) {
  const wire = typeof value === "boolean" ? (value ? "1" : "0") : String(value);
  N.settingSet(key, wire);
}

// observe((key, value) => ...): one observer per app; call stop() to drop it.
export function observe(fn) {
  N.settingsObserve(fn);
}

export function stop() {
  N.settingsObserve(null);
}
