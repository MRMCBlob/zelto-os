# Zelto Script API: `zelto/sensors`

Motion sensors and location. Guides:
[../../system-apis/sensors.md](../../system-apis/sensors.md),
[../../system-apis/location.md](../../system-apis/location.md).

## Motion sensors

```js
import { sensors } from "zelto/sensors";

const unsub = sensors.accelerometer.subscribe((s) => {
  // s = { x, y, z, t }
});
unsub();

sensors.gyroscope.subscribe(fn);
sensors.magnetometer.subscribe(fn);
sensors.orientation.subscribe(fn);   // device attitude

sensors.accelerometer.rate = "ui";   // "fastest" | "game" | "ui" | "normal"
```

Most motion sensors need no permission; high-rate/raw access may. Test with simulator
sensor mocking ([../../getting-started/simulator.md](../../getting-started/simulator.md)).

## Location

Requires the `location` permission (`when-in-use` or `always`).

```js
import { location } from "zelto/sensors";

const pos = await location.current();         // { lat, lng, accuracy, t }
const unsub = location.watch((p) => …, { accuracy: "high", distanceFilter: 10 });
unsub();

await location.permission();                  // request/check; resolves to status
```

Continuous background location requires the `location` background capability and
`always` permission ([../../guides/background-tasks.md](../../guides/background-tasks.md)).

## Pedometer / activity (planned)

```js
sensors.pedometer.subscribe(fn);              // { steps, distance } — Planned
```
