# Zelto Script API: `zelto/media`

Audio/video playback, recording, and the image/photo picker. Guides:
[../../system-apis/media.md](../../system-apis/media.md),
[../../system-apis/camera.md](../../system-apis/camera.md).

## Audio playback

```js
import { Audio } from "zelto/media";
const player = await Audio.load("assets/song.mp3");   // or a remote URL
player.play(); player.pause(); player.seek(30);
player.onEnd(fn);
player.volume = 0.8;
player.release();
```

Background audio requires the `audio` background capability
([../../guides/background-tasks.md](../../guides/background-tasks.md)).

## Video

```js
import { Video } from "zelto/ui";   // video is a view
Video({ source: url, controls: true, autoplay: false }).frame({ width: "fill" }).aspectRatio(16/9);
```

## Recording (audio)

Requires the `microphone` permission.

```js
import { Recorder } from "zelto/media";
const rec = await Recorder.start({ format: "m4a" });
// ...
const file = await rec.stop();        // → file handle in app storage
```

## Image / photo picker

```js
import { picker } from "zelto/media";
const images = await picker.images({ multiple: true });   // user selects; no broad permission
const photo  = await picker.camera();                      // capture (camera permission)
```

The picker returns handles without granting blanket gallery access
([../../platform/permissions.md](../../platform/permissions.md)).

## Camera capture

For a live camera preview/capture surface, see
[../../system-apis/camera.md](../../system-apis/camera.md).
