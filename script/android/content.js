// android/content.js — Context: the andemu entry point Android apps reach the
// system through. Context.getSystemService(name) dispatches to the mapped Android
// manager (translated onto Zelto APIs); an unmapped service returns a graceful
// stub so `getSystemService("nope").anything()` never crashes.
import { SensorManager } from "android/hardware";
import { LocationManager, FusedLocationProviderClient } from "android/location";
import { BatteryManager } from "android/os";
import { ConnectivityManager, WifiManager } from "android/net";
import { SharedPreferences } from "android/prefs";
import { stub, warnUnsupported } from "android/compat";
import * as intents from "zelto/intents";

// android.content.Intent — the anonymous request an Activity fires. Only the two
// actions Zelto can actually fulfil are wired: ACTION_SEND -> the share sheet
// (zelto/intents.share) and ACTION_VIEW of a URI -> a deep link
// (zelto/intents.openUrl). Any other action is a benign no-op, per the contract.
export class Intent {
  constructor(action, data) {
    this.action = action || null;
    this.data = data || null;     // a URI string (ACTION_VIEW)
    this.type = null;             // a MIME type (ACTION_SEND)
    this.extras = {};
  }
  setAction(action) { this.action = action; return this; }
  setType(type) { this.type = type; return this; }
  setData(uri) { this.data = uri == null ? null : String(uri); return this; }
  putExtra(key, value) { this.extras[key] = value; return this; }
  getStringExtra(key) {
    const v = this.extras[key];
    return v == null ? null : String(v);
  }
}
Intent.ACTION_SEND = "android.intent.action.SEND";
Intent.ACTION_VIEW = "android.intent.action.VIEW";
Intent.EXTRA_TEXT = "android.intent.extra.TEXT";
Intent.EXTRA_SUBJECT = "android.intent.extra.SUBJECT";
// createChooser wraps a SEND intent in the system share sheet; Zelto's share IS
// the chooser, so we pass the inner intent straight through.
Intent.createChooser = function (target, _title) {
  return target;
};

export const Context = {
  // The service-name constants Android code passes to getSystemService.
  LOCATION_SERVICE: "location",
  SENSOR_SERVICE: "sensor",
  BATTERY_SERVICE: "batterymanager",
  CONNECTIVITY_SERVICE: "connectivity",
  WIFI_SERVICE: "wifi",
  MODE_PRIVATE: 0,   // the only SharedPreferences mode Zelto has (app-scoped store)

  getSystemService(name) {
    switch (name) {
      case "location":
        return new LocationManager();
      case "sensor":
        return new SensorManager();
      case "batterymanager":
        return new BatteryManager();
      case "connectivity":
        return new ConnectivityManager();
      case "wifi":
        return new WifiManager();
      default:
        warnUnsupported(`service ${String(name)}`);
        return stub(`service:${String(name)}`);
    }
  },

  // Google Play services convenience (LocationServices.getFusedLocationProviderClient).
  getFusedLocationProviderClient() {
    return new FusedLocationProviderClient();
  },

  // getSharedPreferences(name, mode) -> SharedPreferences over zelto/storage. mode
  // (MODE_PRIVATE etc.) is irrelevant on Zelto — the store is already app-private.
  getSharedPreferences(name, _mode) {
    return new SharedPreferences(name);
  },

  // startActivity(intent) — fire the intent. ACTION_SEND hands its EXTRA_TEXT to
  // the share sheet; ACTION_VIEW opens its data URI as a deep link. Anything else
  // is unsupported and does nothing (no crash), as the contract requires.
  startActivity(intent) {
    if (!intent || !intent.action) {
      warnUnsupported("startActivity(no action)");
      return;
    }
    if (intent.action === Intent.ACTION_SEND) {
      const text = intent.getStringExtra(Intent.EXTRA_TEXT);
      if (text != null) intents.shareText(text);
      return;
    }
    if (intent.action === Intent.ACTION_VIEW && intent.data) {
      intents.openUrl(intent.data);
      return;
    }
    warnUnsupported(`startActivity(${String(intent.action)})`);
  },
};

export default Context;
