# Zelto Script: andemu (Android compatibility)

**andemu** lets an app written against the Android APIs — `Context.getSystemService`,
`SensorManager`, `LocationManager`, `SharedPreferences`, `Toast`, `Intent` — run on
Zelto unchanged. It is not an Android runtime: there is no Dalvik, no `Activity`
stack, no Java. It is a set of JavaScript modules (`android/*`) that present the
*shapes* Android code expects and **translate every call into the `zelto/*` system
APIs** underneath ([runtime.md](runtime.md)). The app renders with the Zelto
toolkit, because that is the device it is running on.

## Status

| Available | Not yet |
|---|---|
| `Context` + `getSystemService`, `SensorManager` (+ `getRotationMatrix`/`getOrientation`), `LocationManager` / `FusedLocationProviderClient`, `BatteryManager`, `Connectivity`/`WifiManager`, `SharedPreferences`, `Toast`, `Intent` + `startActivity`, `Handler`/`Looper`, the runtime-permission flow | Views/`Activity`/`Fragment` lifecycle, `RecyclerView`, `ContentProvider`, `BroadcastReceiver`, Java stdlib, anything with no Zelto capability behind it |

Values that come from hardware (sensors, location, battery) are **simulated** in the
current build — see [../system-apis/sensors.md](../system-apis/sensors.md).

## Running an andemu app

An andemu app is an ordinary [Zelto Script](runtime.md) app launched in Android
mode. Its manifest declares `runtime=andemu`, and the installer synthesises the
`--android` flag onto its `exec=`:

```ini
# zelto.toml
id=com.example.app
runtime=andemu
permissions=sensors,location,notifications
```

`--android` sets `globalThis.__ANDEMU__`, which only tunes diagnostics (see the
graceful contract below). The `android/*` modules resolve for **any** script app,
so a Zelto-native app can import one deliberately; `runtime=andemu` is what makes a
whole app Android-shaped and declares its intent.

Permissions are the Zelto broker's. An Android permission string maps to a Zelto
permission (`ACCESS_FINE_LOCATION` → `location`, `BODY_SENSORS` → `sensors`,
`POST_NOTIFICATIONS` → `notifications`), so `requestPermissions` raises the *same*
consent dialog a native app gets ([../platform/permissions.md](../platform/permissions.md)).
Declare the Zelto names in the manifest.

## The graceful contract

The rule that makes andemu useful: **an Android app must never crash reaching for
something Zelto does not map.** So `android/compat.js` guarantees:

- an unmapped `getSystemService(name)` returns a benign **stub** — every method is a
  no-op returning `null`, so `getSystemService("nfc").anything()` runs harmlessly;
- an absent sensor makes `getDefaultSensor(type)` return `null` (Android apps
  null-check it), never a throw;
- a denied or unmapped permission comes back **denied**, not as an exception;
- location/sensor accessors return the Android-idiomatic empty (`null` `Location`,
  empty list) rather than rejecting.

Under `--android` an unsupported call logs one line — `andemu: unsupported …` — and
returns its default. Nothing throws, and no promise a listener will not catch is
rejected.

## What maps to what

| Android | Zelto | Notes |
|---|---|---|
| `Context.getSystemService(SENSOR_SERVICE)` → `SensorManager` | `zelto/sensors` | `SENSOR_DELAY_*` → Hz (capped at 60); `SensorEventListener.onSensorChanged`/`onAccuracyChanged` both fire |
| `SensorManager.getRotationMatrix` / `getOrientation` | pure math | the stock algorithms, run on the caller's arrays — no device access |
| `LocationManager` / `FusedLocationProviderClient` | `zelto/sensors` location | `getLastKnownLocation` is synchronous and returns `null` when denied; `requestLocationUpdates` streams fixes |
| `BatteryManager` | `zelto/settings` (`sys.battery_*`) | `getIntProperty(CAPACITY)`, `isCharging()` |
| `ConnectivityManager` / `WifiManager` | `zelto/settings` (`sys.wifi`/`sys.airplane`) | `getActiveNetworkInfo().isConnected()` etc. |
| `Context.getSharedPreferences(name, mode)` | `zelto/storage` | file name becomes a key prefix; `edit()`/`putX`/`apply()`/`commit()` batch writes |
| `Toast.makeText(ctx, text, dur).show()` | `zelto/notifications` | a low-importance banner; degrades to a log line if `notifications` is denied |
| `Intent` + `Context.startActivity` | `zelto/intents` | `ACTION_SEND` → the share sheet, `ACTION_VIEW` of a URI → a deep link; other actions are a no-op |
| `Handler` / `Looper.postDelayed` | `setTimeout` | the guest already runs one loop, so `postDelayed` **is** `setTimeout`; no threads |
| `ActivityCompat.requestPermissions` / `checkSelfPermission` | `zelto/permissions` | the same consent dialog and cached decisions a native app uses |

Because the broker allows only one consent dialog in flight at a time, request the
permissions you need up front (`requestPermissions([...])`) before firing calls that
prompt — the same discipline a well-behaved Android app already follows.

## Example

```js
import Context, { Intent } from "android/content";
import { Sensor, SensorManager, SENSOR_DELAY_UI } from "android/hardware";
import Toast from "android/widget";
import * as pm from "android/pm";

await pm.requestPermissions(["android.permission.ACCESS_FINE_LOCATION",
                             "android.permission.BODY_SENSORS"]);

const prefs = Context.getSharedPreferences("prefs", Context.MODE_PRIVATE);
prefs.edit().putInt("runs", prefs.getInt("runs", 0) + 1).apply();

const sm = Context.getSystemService(Context.SENSOR_SERVICE);
sm.registerListener({
  onSensorChanged: (e) => { /* e.values, e.accuracy, e.timestamp */ },
  onAccuracyChanged: (_s, acc) => console.log("accuracy", acc),
}, sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER), SENSOR_DELAY_UI);

Toast.makeText(null, "ready", Toast.LENGTH_SHORT).show();
Context.startActivity(new Intent(Intent.ACTION_VIEW, "https://example.com"));
```

A complete, runnable app lives at `samples/andemu-demo/` — it exercises every
mapping above and the graceful contract in one screen.

## When to reach for it

andemu is a **compatibility bridge**, not the native way to build for Zelto. A new
app should use the `zelto/*` APIs directly ([stdlib.md](stdlib.md)) — they are
smaller and idiomatic. Reach for andemu to port an existing Android app, or to reuse
Android-shaped code, with the least change.
