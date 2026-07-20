// andemu Demo — an "Android" app that only ever calls android.* APIs.
//
// The whole point of the andemu route: this file is written the way an Android
// developer writes one — Context.getSystemService, SensorManager.registerListener,
// LocationManager.getLastKnownLocation, SharedPreferences, Toast, Intent,
// Handler.postDelayed — and every one of those calls is translated by the
// android/* modules into the Zelto system APIs underneath (zelto/sensors,
// zelto/storage, zelto/notifications, zelto/intents, zelto/settings, the zsysd
// permission broker). It renders with the Zelto toolkit, because that is the device
// it runs on, but it never touches a zelto/* system module directly.
//
// It also proves the properties that matter: a stream requested at
// SENSOR_DELAY_GAME arrives at a real (≤60 Hz) rate; accelerometer + magnetometer
// fuse through the stock getRotationMatrix/getOrientation math; SharedPreferences
// survives a relaunch; and an app that reaches for something unsupported keeps
// running instead of crashing — the graceful compat contract.
import { useState, useEffect, useRef } from "zelto";
import { VStack, HStack, Text, Spacer, Scroll, Color, Font, Weight } from "zelto/ui";

import Context, { Intent } from "android/content";
import { Sensor, SensorManager, SENSOR_DELAY_GAME, SENSOR_DELAY_UI } from "android/hardware";
import { Handler } from "android/os";
import Toast from "android/widget";
import * as pm from "android/pm";

function row(label, value) {
  return HStack({ align: "center" }, [
    Text(label).font(Font.caption).color(Color.textMuted),
    Spacer(),
    Text(value).font(Font.body).color(Color.text),
  ]);
}

function card(title, rows) {
  return VStack({ spacing: 10, padding: 16 }, [
    Text(title).font(Font.footnote).color(Color.textFaint),
    ...rows,
  ])
    .bg(Color.surface2)
    .radius(16);
}

const DEG = 180 / Math.PI;

export default function App() {
  const [gyro, setGyro] = useState([0, 0, 0]);
  const [rate, setRate] = useState(0);
  const [orient, setOrient] = useState(null); // [azimuth, pitch, roll] degrees
  const [acc, setAcc] = useState("—");         // onAccuracyChanged
  const [loc, setLoc] = useState(null);
  const [battery, setBattery] = useState(-1);
  const [net, setNet] = useState("?");
  const [launches, setLaunches] = useState(0); // SharedPreferences
  const [delayed, setDelayed] = useState("pending…"); // Handler.postDelayed
  const [shared, setShared] = useState("—");   // Intent + startActivity
  const [compat, setCompat] = useState("checking…");

  const nRef = useRef(0);
  const t0 = useRef(0);
  const gravity = useRef([0, 0, 0]);   // latest accelerometer sample
  const geomag = useRef([0, 0, 0]);    // latest magnetometer sample

  useEffect(() => {
    // Android-style boot: request the runtime permissions, then wire managers.
    async function boot() {
      // --- SharedPreferences: a launch counter that survives relaunch ---
      const prefs = Context.getSharedPreferences("andemu", Context.MODE_PRIVATE);
      const n = prefs.getInt("launch_count", 0) + 1;
      prefs.edit().putInt("launch_count", n).apply();
      setLaunches(n);

      // Request the sensor/location grants first. The broker allows only one
      // consent dialog at a time, so we resolve these before firing anything else
      // that prompts (the Toast below asks for `notifications`).
      await pm.requestPermissions([
        "android.permission.ACCESS_FINE_LOCATION",
        "android.permission.BODY_SENSORS",
      ]);

      // --- Toast: a fleeting banner (translated to a system notification) ---
      Toast.makeText(null, `andemu launch #${n}`, Toast.LENGTH_SHORT).show();

      const sm = Context.getSystemService(Context.SENSOR_SERVICE);

      // --- SensorManager: stream the gyroscope at "GAME" rate ---
      const gyroSensor = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
      t0.current = Date.now();
      sm.registerListener(
        (ev) => {
          nRef.current += 1;
          setGyro(ev.values);
          const secs = (Date.now() - t0.current) / 1000;
          if (secs > 0.25) setRate(Math.round(nRef.current / secs));
        },
        gyroSensor,
        SENSOR_DELAY_GAME
      );

      // --- Sensor fusion: accelerometer + magnetometer -> orientation, plus
      //     onAccuracyChanged (a full SensorEventListener object) ---
      const accelSensor = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
      const magSensor = sm.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD);
      const R = new Array(9).fill(0);
      const out = new Array(3).fill(0);
      const recompute = () => {
        if (SensorManager.getRotationMatrix(R, null, gravity.current, geomag.current)) {
          SensorManager.getOrientation(R, out);
          setOrient(out.map((v) => Math.round(v * DEG)));
        }
      };
      sm.registerListener(
        {
          onSensorChanged: (ev) => { gravity.current = ev.values; recompute(); },
          onAccuracyChanged: (_sensor, accuracy) => setAcc(`level ${accuracy}`),
        },
        accelSensor,
        SENSOR_DELAY_UI
      );
      sm.registerListener(
        (ev) => { geomag.current = ev.values; recompute(); },
        magSensor,
        SENSOR_DELAY_UI
      );

      // --- LocationManager: updates + a synchronous last-known fix ---
      const lm = Context.getSystemService(Context.LOCATION_SERVICE);
      lm.requestLocationUpdates("gps", 1000, 0, (l) => {
        if (l) setLoc({ lat: l.getLatitude(), lng: l.getLongitude(), acc: l.getAccuracy() });
      });
      const last = lm.getLastKnownLocation("gps");
      if (last) setLoc({ lat: last.getLatitude(), lng: last.getLongitude(), acc: last.getAccuracy() });

      // --- BatteryManager + ConnectivityManager ---
      const bm = Context.getSystemService(Context.BATTERY_SERVICE);
      setBattery(bm.capacity);
      const cm = Context.getSystemService(Context.CONNECTIVITY_SERVICE);
      setNet(cm.getActiveNetworkInfo().isConnected() ? "connected" : "offline");

      // --- Handler.postDelayed: run something on the loop half a second later ---
      const handler = new Handler();
      handler.postDelayed(() => setDelayed("ran after 500ms"), 500);

      // --- Intent + startActivity: an ACTION_VIEW deep link (no chooser overlay,
      //     so it is safe to fire on boot). ACTION_SEND would raise the share sheet.
      const view = new Intent(Intent.ACTION_VIEW, "andemu://opened");
      Context.startActivity(view);
      setShared("ACTION_VIEW sent");

      // --- Graceful contract: unsupported must NOT crash ---
      const nfc = Context.getSystemService("nfc"); // unmapped -> benign stub
      const unknown = sm.getDefaultSensor("teleporter"); // absent -> null
      const stubbed = nfc.doAnything(); // no-op -> null (no throw)
      setCompat(`service=${stubbed}  sensor=${unknown}`); // "service=null  sensor=null"
    }
    boot();
  }, []);

  const gyroText = gyro.map((v) => v.toFixed(2)).join("   ");
  const orientText = orient
    ? `az ${orient[0]}°   pitch ${orient[1]}°   roll ${orient[2]}°`
    : "—";
  const locText = loc
    ? `${loc.lat.toFixed(4)}, ${loc.lng.toFixed(4)}  ±${loc.acc}m`
    : "—";

  return Scroll(
    VStack({ spacing: 14, padding: 20 }, [
      Text("andemu — Android APIs")
        .font(Font.title)
        .weight(Weight.bold)
        .color(Color.text),
      Text("android.* calls, translated to Zelto system APIs")
        .font(Font.subhead)
        .color(Color.textMuted),

      card("SensorManager · gyroscope", [
        row("values", gyroText),
        row("rate", `${rate} Hz  (SENSOR_DELAY_GAME, cap 60)`),
      ]),
      card("Sensor fusion · getOrientation", [
        row("orientation", orientText),
        row("accuracy", acc),
      ]),
      card("LocationManager", [row("fix", locText)]),
      card("BatteryManager · ConnectivityManager", [
        row("battery", battery < 0 ? "—" : `${battery}%`),
        row("network", net),
      ]),
      card("SharedPreferences · Handler · Intent", [
        row("launch #", String(launches)),
        row("postDelayed", delayed),
        row("startActivity", shared),
      ]),
      card("Graceful compat", [row("unsupported →", compat)]),
    ])
  ).bg(Color.bg);
}
