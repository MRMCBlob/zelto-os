# Sensors

Access motion sensors: accelerometer, gyroscope, magnetometer, and fused orientation.
API reference: [../api-reference/script/sensors.md](../api-reference/script/sensors.md).

```js
import { sensors } from "zelto/sensors";

const unsub = sensors.accelerometer.subscribe(({ x, y, z, t }) => {
  setTilt(x);
});
// later:
unsub();
```

## Available sensors

| Sensor | Data | Permission |
|---|---|---|
| `accelerometer` | `{ x, y, z, t }` m/s² | none (high-rate may) |
| `gyroscope` | `{ x, y, z, t }` rad/s | none |
| `magnetometer` | `{ x, y, z, t }` µT | none |
| `orientation` | fused attitude (roll/pitch/yaw) | none |
| `pedometer` | `{ steps, distance }` | activity (Planned) |

## Sampling rate

```js
sensors.gyroscope.rate = "game";   // "fastest" | "game" | "ui" | "normal"
```

Higher rates cost battery; pick the slowest rate that works.

## Lifecycle

Always unsubscribe when the view unmounts or the app pauses:

```js
useEffect(() => {
  const unsub = sensors.accelerometer.subscribe(onSample);
  return () => unsub();
}, []);
```

Subscriptions are suspended automatically while the app is backgrounded (unless a
relevant background capability is declared,
[../guides/background-tasks.md](../guides/background-tasks.md)).

## Availability

Sensor availability depends on the device and its postmarketOS port. Check before use:

```js
if (sensors.gyroscope.available) { … }
```

## Testing

Mock sensor input in the simulator
([../getting-started/simulator.md](../getting-started/simulator.md)).
