// The `zelto/sensors` module: device sensors + location, shaped as the docs
// promise (docs/system-apis/location.md).
//
// A stream (location.watch, sensors.gyroscope, ...) is driven by the zsysd sensor
// source over libzelto's async socket, parked in the app loop — samples arrive
// between frames, nothing blocks the UI. Location needs the `location` permission
// and the other sensors need `sensors`; a stream awaits its grant first, so a
// denial rejects rather than silently doing nothing (the zelto/net idiom).
//
// Refresh rate is Hz, clamped by the platform to [1, 60] (60 is display-aligned;
// higher is wasted on a software-rendered device). The named presets mirror the
// Android SensorManager delays, which is what the andemu bridge maps onto.
import * as N from "zelto:native";
import { ensure, status } from "zelto/permissions";

// Subscribe once the grant is held. The broker only allows ONE consent request in
// flight at a time, so a "request once, open many" app (the common shape — open
// the accelerometer AND the magnetometer to fuse orientation) must not fire a
// fresh consent per stream. When the capability is already granted we subscribe
// straight away; only an undecided/denied grant awaits the dialog. That mirrors a
// native app, which calls z_perm_request once and then opens every stream.
function whenGranted(perm, start) {
  if (status(perm) === "granted") {
    start();
    return;
  }
  ensure(perm).then(start).catch(() => {});
}

export const Rate = {
  min: 1,
  normal: 5,
  ui: 16,
  game: 50,
  fastest: 60,
  max: 60,
};

// --- location --------------------------------------------------------------

export const location = {
  // "granted" | "denied" | "prompt"
  permission() {
    const s = N.permStatus("location");
    if (s === N.PERM_GRANTED) return "granted";
    if (s === N.PERM_DENIED) return "denied";
    return "prompt";
  },

  // A single fix: { lat, lng, accuracy, altitude, speed, bearing, t } (null if
  // denied / no fix). Awaits the grant first.
  async current() {
    await ensure("location");
    return N.locationGet();
  },

  // Synchronous last fix — no await, no prompt. Returns the fix if the grant is
  // already held, else null (the broker gates it). This is what Android's
  // synchronous getLastKnownLocation maps onto.
  lastKnown() {
    return N.locationGet();
  },

  // Continuous updates. `opts.rate` is Hz (default 1). Returns an unsubscribe fn.
  // A denied grant throws (await it) rather than starting a dead stream.
  watch(cb, opts = {}) {
    const rate = opts.rate ?? Rate.min;
    let handle = 0;
    let stopped = false;
    whenGranted("location", () => {
      if (stopped) return;
      handle = N.locationWatch(rate, (p) => { if (p) cb(p); });
    });
    return () => {
      stopped = true;
      if (handle) N.locationStop(handle);
    };
  },
};

// --- sensors ---------------------------------------------------------------

// Whether the device exposes a sensor type ("gyroscope", "accelerometer", ...).
export function has(type) {
  return N.sensorPresent(type);
}

// Open a sensor stream. `cb` receives { type, values:[...], accuracy, t } per
// sample. `opts.rate` is Hz (default 5). Returns an unsubscribe fn. Awaits the
// `sensors` grant; a denial simply never delivers.
export function open(type, cb, opts = {}) {
  const rate = opts.rate ?? Rate.normal;
  let handle = 0;
  let stopped = false;
  whenGranted("sensors", () => {
    if (stopped) return;
    handle = N.sensorOpen(type, rate, (ev) => cb(ev));
  });
  return () => {
    stopped = true;
    if (handle) N.sensorClose(handle);
  };
}

// Named helpers for the common sensors — each is `open` bound to a type.
export const accelerometer = (cb, opts) => open("accelerometer", cb, opts);
export const gyroscope = (cb, opts) => open("gyroscope", cb, opts);
export const magnetometer = (cb, opts) => open("magnetometer", cb, opts);
export const orientation = (cb, opts) => open("orientation", cb, opts);
export const gravity = (cb, opts) => open("gravity", cb, opts);
export const light = (cb, opts) => open("light", cb, opts);
export const proximity = (cb, opts) => open("proximity", cb, opts);
export const pressure = (cb, opts) => open("pressure", cb, opts);
export const stepCounter = (cb, opts) => open("step_counter", cb, opts);
