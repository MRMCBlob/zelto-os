// android/hardware.js — the SensorManager surface of the andemu route.
//
// android.hardware.SensorManager / Sensor / SensorEvent, translated onto the
// Zelto sensor streams (zelto/sensors). Android's SENSOR_DELAY_* map onto the
// Zelto rate presets (clamped to 60 Hz — Android's FASTEST is "device-dependent",
// so the cap is compliant). A sensor the device does not have makes
// getDefaultSensor return null (Android apps null-check it), never a throw.
import * as sensors from "zelto/sensors";
import { warnUnsupported } from "android/compat";

// Android Sensor.TYPE_* -> Zelto sensor type name.
export const Sensor = {
  TYPE_ACCELEROMETER: "accelerometer",
  TYPE_GYROSCOPE: "gyroscope",
  TYPE_MAGNETIC_FIELD: "magnetometer",
  TYPE_ORIENTATION: "orientation",
  TYPE_GRAVITY: "gravity",
  TYPE_LINEAR_ACCELERATION: "linear_acceleration",
  TYPE_ROTATION_VECTOR: "rotation_vector",
  TYPE_LIGHT: "light",
  TYPE_PROXIMITY: "proximity",
  TYPE_PRESSURE: "pressure",
  TYPE_STEP_COUNTER: "step_counter",
};

// SENSOR_DELAY_* — exported as Hz (what registerListener forwards to Zelto).
export const SENSOR_DELAY_NORMAL = sensors.Rate.normal;
export const SENSOR_DELAY_UI = sensors.Rate.ui;
export const SENSOR_DELAY_GAME = sensors.Rate.game;
export const SENSOR_DELAY_FASTEST = sensors.Rate.fastest;

// SensorEvent.accuracy levels.
export const SENSOR_STATUS_ACCURACY_HIGH = 3;

function delayToRate(delay) {
  if (typeof delay === "number") {
    if (delay > 0 && delay <= 60) return delay; // an Hz preset (our constants)
    if (delay >= 1000) {
      // a microsecond period, as Android also accepts
      return Math.min(60, Math.max(1, Math.round(1e6 / delay)));
    }
  }
  return sensors.Rate.normal;
}

class SensorObj {
  constructor(type) {
    this._type = type;
  }
  getType() {
    return this._type;
  }
  getName() {
    return this._type;
  }
  getMaximumRange() {
    return 0;
  }
}

export class SensorManager {
  constructor() {
    // listener -> [stop, ...]. Android lets one SensorEventListener register for
    // several sensors at once; each registration owns its own Zelto stream, so we
    // keep a LIST of unsubscribe fns per listener. (Keying a single stop by
    // listener would drop the earlier stream's handle on the second register, and
    // unregisterListener would then leak it — a live sensor after the app thinks
    // it stopped, defeating the when-in-use pause.)
    this._streams = new Map();
  }

  // getDefaultSensor(type) — type is a Sensor.TYPE_* value (a Zelto name). Returns
  // null for a sensor the device lacks, exactly as Android does.
  getDefaultSensor(type) {
    if (typeof type !== "string" || !sensors.has(type)) {
      warnUnsupported(`sensor ${String(type)}`);
      return null;
    }
    return new SensorObj(type);
  }

  getSensorList(_type) {
    const seen = {};
    const list = [];
    for (const t of Object.values(Sensor)) {
      if (!seen[t] && sensors.has(t)) {
        seen[t] = true;
        list.push(new SensorObj(t));
      }
    }
    return list;
  }

  // registerListener(listener, sensor, delay) -> boolean (Android returns whether
  // the sensor was registered). `listener` is a function or a
  // SensorEventListener ({ onSensorChanged, onAccuracyChanged }).
  registerListener(listener, sensor, delay) {
    if (!sensor || !listener) return false;
    const type = sensor.getType();
    const rate = delayToRate(delay);
    let lastAcc = -1;   // so the first sample delivers an onAccuracyChanged
    const stop = sensors.open(
      type,
      (ev) => {
        const se = {
          sensor,
          accuracy: ev.accuracy,
          timestamp: ev.t * 1e6, // Android SensorEvent.timestamp is nanoseconds
          values: ev.values,
        };
        // Android fires onAccuracyChanged whenever a sensor's reported accuracy
        // changes (and once up front) — surface that on a SensorEventListener.
        if (
          listener &&
          typeof listener !== "function" &&
          listener.onAccuracyChanged &&
          ev.accuracy !== lastAcc
        ) {
          lastAcc = ev.accuracy;
          listener.onAccuracyChanged(sensor, ev.accuracy);
        }
        if (typeof listener === "function") listener(se);
        else if (listener.onSensorChanged) listener.onSensorChanged(se);
      },
      { rate }
    );
    const existing = this._streams.get(listener);
    if (existing) existing.push(stop);
    else this._streams.set(listener, [stop]);
    return true;
  }

  unregisterListener(listener) {
    if (listener) {
      const stops = this._streams.get(listener);
      if (stops) {
        for (const stop of stops) stop();
        this._streams.delete(listener);
      }
      return;
    }
    // No listener: unregister everything (the Android overload).
    for (const stops of this._streams.values())
      for (const stop of stops) stop();
    this._streams.clear();
  }
}

// --- static orientation helpers (SensorManager.getRotationMatrix / getOrientation)
//
// These are pure math on the caller's arrays — no device access — so they are the
// stock Android algorithms verbatim, letting an app fuse accelerometer +
// magnetometer into a rotation matrix and then azimuth/pitch/roll exactly as it
// would on Android.

// getRotationMatrix(R, I, gravity, geomagnetic) -> boolean (false in free-fall or
// with no usable magnetic field, as Android returns). R (and optional inclination
// matrix I) may be length 9 or 16; they are written in place.
SensorManager.getRotationMatrix = function (R, I, gravity, geomagnetic) {
  let Ax = gravity[0], Ay = gravity[1], Az = gravity[2];
  const normsqA = Ax * Ax + Ay * Ay + Az * Az;
  const g = 9.81;
  const freeFallGravitySquared = 0.01 * g * g;
  if (normsqA < freeFallGravitySquared) return false; // device is in free fall

  const Ex = geomagnetic[0], Ey = geomagnetic[1], Ez = geomagnetic[2];
  let Hx = Ey * Az - Ez * Ay;
  let Hy = Ez * Ax - Ex * Az;
  let Hz = Ex * Ay - Ey * Ax;
  const normH = Math.sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
  if (normH < 0.1) return false; // gravity and magnetic field are parallel
  const invH = 1.0 / normH;
  Hx *= invH; Hy *= invH; Hz *= invH;
  const invA = 1.0 / Math.sqrt(Ax * Ax + Ay * Ay + Az * Az);
  Ax *= invA; Ay *= invA; Az *= invA;
  const Mx = Ay * Hz - Az * Hy;
  const My = Az * Hx - Ax * Hz;
  const Mz = Ax * Hy - Ay * Hx;

  if (R) {
    if (R.length === 9) {
      R[0] = Hx; R[1] = Hy; R[2] = Hz;
      R[3] = Mx; R[4] = My; R[5] = Mz;
      R[6] = Ax; R[7] = Ay; R[8] = Az;
    } else if (R.length === 16) {
      R[0] = Hx; R[1] = Hy; R[2] = Hz; R[3] = 0;
      R[4] = Mx; R[5] = My; R[6] = Mz; R[7] = 0;
      R[8] = Ax; R[9] = Ay; R[10] = Az; R[11] = 0;
      R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 1;
    }
  }
  if (I) {
    const invE = 1.0 / Math.sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
    const c = (Ex * Mx + Ey * My + Ez * Mz) * invE;
    const s = (Ex * Ax + Ey * Ay + Ez * Az) * invE;
    if (I.length === 9) {
      I[0] = 1; I[1] = 0; I[2] = 0;
      I[3] = 0; I[4] = c; I[5] = s;
      I[6] = 0; I[7] = -s; I[8] = c;
    } else if (I.length === 16) {
      I[0] = 1; I[1] = 0; I[2] = 0;
      I[4] = 0; I[5] = c; I[6] = s;
      I[8] = 0; I[9] = -s; I[10] = c;
      I[3] = I[7] = I[11] = I[12] = I[13] = I[14] = 0;
      I[15] = 1;
    }
  }
  return true;
};

// getOrientation(R, values) -> values = [azimuth, pitch, roll] in radians.
SensorManager.getOrientation = function (R, values) {
  if (R.length === 9) {
    values[0] = Math.atan2(R[1], R[4]);
    values[1] = Math.asin(-R[7]);
    values[2] = Math.atan2(-R[6], R[8]);
  } else {
    values[0] = Math.atan2(R[1], R[5]);
    values[1] = Math.asin(-R[9]);
    values[2] = Math.atan2(-R[8], R[10]);
  }
  return values;
};
