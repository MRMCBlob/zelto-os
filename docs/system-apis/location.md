# Location

Get the device's location, once or continuously. Requires the `location` permission.
API reference: [../api-reference/script/sensors.md](../api-reference/script/sensors.md).

> `location = "when-in-use"` or `"always"` in `zelto.toml`
> ([../packaging/manifest.md](../packaging/manifest.md)).

## One-shot

```js
import { location } from "zelto/sensors";

const pos = await location.current();    // { lat, lng, accuracy, altitude?, speed?, t }
```

## Continuous

```js
const unsub = location.watch((p) => setPos(p), {
  accuracy: "high",       // "high" | "balanced" | "low"
  distanceFilter: 10,     // metres of movement before an update
});
// later:
unsub();
```

## Permission flow

```js
const status = await location.permission();   // "granted" | "denied" | "prompt"
if (status !== "granted") {
  await location.permission();                // triggers the system prompt
}
```

`when-in-use` allows location only while the app is foreground. `always` (plus the
`location` background capability) is required for background tracking
([../guides/background-tasks.md](../guides/background-tasks.md)).

## Accuracy vs. battery

Higher accuracy uses GPS and more power; `balanced`/`low` use network/cell positioning.
Use the lowest accuracy your feature needs, and stop watching when done.

## Availability

GPS availability depends on the device port and HAL bridge
([../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md)). Test with
`zelto simulator set location 52.52,13.40`
([../getting-started/simulator.md](../getting-started/simulator.md)).

## Privacy

Location is sensitive: request it in context (when the user invokes a location feature),
explain why, and show the system's in-use indicator while active.
