# Media

Play and record audio, play video, and pick images. API reference:
[../api-reference/script/media.md](../api-reference/script/media.md).

## Audio playback

```js
import { Audio } from "zelto/media";

const player = await Audio.load("assets/song.mp3");   // asset or remote URL
player.play();
player.pause();
player.seek(30);          // seconds
player.volume = 0.8;      // 0..1
player.onEnd(() => next());
player.release();         // free when done
```

Background playback requires the `audio` background capability
([../guides/background-tasks.md](../guides/background-tasks.md)) and shows media controls
in the System UI.

## Video

Video is a view:

```js
import { Video } from "zelto/ui";

Video({ source: url, controls: true, autoplay: false, loop: false })
  .frame({ width: "fill" })
  .aspectRatio(16/9);
```

Drive it imperatively with a ref (`play`, `pause`, `seek`) via `useVideo()`.

## Audio recording

Requires the `microphone` permission.

```js
import { Recorder } from "zelto/media";

const rec = await Recorder.start({ format: "m4a" });   // "m4a" | "wav"
// ...
const file = await rec.stop();      // → file handle in app storage
```

## Image picker

```js
import { picker } from "zelto/media";

const images = await picker.images({ multiple: true });   // no broad gallery permission
const photo  = await picker.camera();                      // camera permission
```

The picker returns handles for the items the user explicitly chose.

## Now-playing integration

When you declare background `audio`, set the now-playing metadata so the lock screen and
shade show track info and controls:

```js
import { nowPlaying } from "zelto/media";
nowPlaying.set({ title, artist, artwork, duration });
nowPlaying.onCommand((cmd) => { /* "play"|"pause"|"next"|"prev" */ });
```

## See also

- [camera.md](camera.md) · [../guides/storage.md](../guides/storage.md)
