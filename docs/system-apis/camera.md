# Camera

Capture photos/video, or show a live preview. Requires the `camera` permission (and
`microphone` for video with audio).

> Declare `camera = true` in `zelto.toml` ([../packaging/manifest.md](../packaging/manifest.md));
> the user grants it at runtime ([../platform/permissions.md](../platform/permissions.md)).

## Quick capture (picker)

For a one-off photo without managing a camera session, use the picker — it needs no
broad gallery permission:

```js
import { picker } from "zelto/media";
const photo = await picker.camera();     // → file handle, or null if cancelled
```

## Live preview view

```js
import { CameraView } from "zelto/ui";

CameraView({
  facing: "back",          // "back" | "front"
  ref: cam,
}).frame({ width: "fill" }).aspectRatio(3/4);
```

Control via the ref:

```js
const cam = useCamera();
const photo = await cam.capture({ flash: "auto" });   // → file handle
await cam.startRecording();
const video = await cam.stopRecording();
await cam.setFacing("front");
```

## Options

| Option / arg | Values | Notes |
|---|---|---|
| `facing` | `back` / `front` | Default `back` |
| `capture({ flash })` | `off`/`on`/`auto` | Still capture |
| `capture({ quality })` | `0..1` | JPEG quality |
| recording | — | Requires `microphone` for audio |

## Availability & device notes

Camera support depends on the device port and the Waydroid/HAL camera bridge; check
availability and degrade gracefully:

```js
if (!CameraView.available) return Text("Camera unavailable");
```

See [../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md) for hardware
passthrough caveats.

## Output

Captured media is written to app storage and returned as a handle you can read, upload,
or display ([../guides/storage.md](../guides/storage.md)).
