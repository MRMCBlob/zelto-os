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

---

## What is actually implemented (P53, C / libzelto)

The JavaScript surface above is the target design. What ships today is the C API
in `<zelto/ui.h>`, and it is shaped like the sensors route: request the grant,
open a stream at a rate, receive frames on the app loop.

```c
z_perm_request("camera", on_camera_perm, state);   // ask first
...
state->stream = z_camera_open(app, 15, on_frame, state);   // only on a grant

const char *key = z_camera_preview_key();          // NULL until frame 1
ZView pane = key ? Cover(Image(key)) : Rect(.color = Z_COLOR_SURFACE_2);

int w, h; int64_t seq;                             // the shutter
const uint32_t *px = z_camera_frame_pixels(&w, &h, &seq);
zelto_photo_store(id, px, w, h, (size_t)w * 4, /*premultiplied=*/false);
```

**Backgrounding stops the stream.** Not a power optimisation — the privacy rule
the sensors route established, applied where it matters most. The app does not
opt in and cannot opt out.

**The broker knows who is streaming.** `z_camera_open` tells zsysd, which
re-checks the grant (an app cannot start a stream by not asking) and records the
user. That record — not anything the app draws — is what a system in-use
indicator is built on.

### There is no camera in the simulator or in QEMU

The shipped frame source is **synthetic**: a colour-bar test signal generated
behind one function, `camera_source_fill()` in `sdk/src/camera.c`. A device port
replaces that function with a V4L2 or HAL read and nothing above it changes.

Read the header of `sdk/src/camera.c` for why (host passthrough, `vivid` and a
still loop were each rejected on measured grounds) and — more importantly — for
**what that does and does not verify**. In short: frames advancing, the preview
being current, a capture keeping the frame that was live, the background pause,
the permission gate and the write into the shared library are all tested; that a
physical sensor's pixels reach this API is not, and cannot be here.

The frames carry their own sequence number in the top-left pixel so a capture can
be **proven** to have kept the live frame rather than a stale buffer. An honest
fake also *looks* fake: the preview is a test pattern, not a photograph.
