// android/location.js — the LocationManager / FusedLocationProviderClient surface
// of the andemu route, translated onto zelto/sensors location.
//
// getLastKnownLocation is synchronous and returns null when the `location` grant
// is absent (the Android-idiomatic empty, not a throw). requestLocationUpdates
// streams fixes; the Android minTime (ms) becomes a Zelto rate (Hz), clamped.
import * as sensors from "zelto/sensors";
import * as pm from "android/pm";

// A Zelto fix { lat,lng,accuracy,altitude,speed,bearing,t } -> an android.location
// .Location (getters + plain fields, both shapes Android code uses).
function toLocation(p) {
  if (!p) return null;
  return {
    getLatitude: () => p.lat,
    getLongitude: () => p.lng,
    getAccuracy: () => p.accuracy,
    getAltitude: () => p.altitude,
    getSpeed: () => p.speed,
    getBearing: () => p.bearing,
    getTime: () => p.t,
    latitude: p.lat,
    longitude: p.lng,
    accuracy: p.accuracy,
    altitude: p.altitude,
    speed: p.speed,
    bearing: p.bearing,
    time: p.t,
  };
}

function minTimeToRate(minTimeMs) {
  if (typeof minTimeMs === "number" && minTimeMs > 0) {
    return Math.min(60, Math.max(1, Math.round(1000 / minTimeMs)));
  }
  return 1;
}

function deliver(listener, loc) {
  if (typeof listener === "function") listener(loc);
  else if (listener && listener.onLocationChanged) listener.onLocationChanged(loc);
}

export class LocationManager {
  constructor() {
    this._unsubs = new Map();
    this._last = null;
  }

  // Synchronous last fix, or null when denied / unavailable.
  getLastKnownLocation(_provider) {
    if (
      pm.checkSelfPermission("ACCESS_FINE_LOCATION") !== pm.PERMISSION_GRANTED &&
      pm.checkSelfPermission("ACCESS_COARSE_LOCATION") !== pm.PERMISSION_GRANTED
    ) {
      return null;
    }
    const fix = toLocation(sensors.location.lastKnown());
    return fix || this._last;
  }

  requestLocationUpdates(provider, minTimeMs, minDistance, listener) {
    if (!listener) return;
    const rate = minTimeToRate(minTimeMs);
    const unsub = sensors.location.watch(
      (p) => {
        const loc = toLocation(p);
        this._last = loc;
        deliver(listener, loc);
      },
      { rate }
    );
    this._unsubs.set(listener, unsub);
  }

  removeUpdates(listener) {
    const unsub = this._unsubs.get(listener);
    if (unsub) {
      unsub();
      this._unsubs.delete(listener);
    }
  }
}

// The Google Play services fused client, over the same source.
export class FusedLocationProviderClient {
  constructor() {
    this._unsub = null;
  }

  // getLastLocation() -> a Task exposing addOnSuccessListener(cb).
  getLastLocation() {
    const loc = toLocation(sensors.location.lastKnown());
    const task = {
      addOnSuccessListener(cb) {
        cb(loc);
        return task;
      },
      addOnFailureListener() {
        return task;
      },
      isSuccessful: () => true,
      getResult: () => loc,
    };
    return task;
  }

  requestLocationUpdates(request, callback) {
    const rate = request && request.rate ? request.rate : 1;
    this._unsub = sensors.location.watch(
      (p) => {
        const loc = toLocation(p);
        const result = {
          getLastLocation: () => loc,
          getLocations: () => [loc],
        };
        if (typeof callback === "function") callback(result);
        else if (callback && callback.onLocationResult) {
          callback.onLocationResult(result);
        }
      },
      { rate }
    );
  }

  removeLocationUpdates() {
    if (this._unsub) {
      this._unsub();
      this._unsub = null;
    }
  }
}
