# The photo library

Every image a user *creates* on this phone — a screenshot today, a camera still next —
lands in one shared library. This page is the contract: where it is, what a photo's
identity is, and what a second app may assume about it.

The implementation and the full reasoning live in `system/common/photos.h`; this is the
summary a caller needs.

## Where

```
<data>/media/photos/<id>.png     the full-size image
<data>/media/thumbs/<id>.png     a small sibling, made by the writer
```

`<data>` is the persistent root (`$ZELTO_DATA_DIR`, `/var/zelto` on device), and `media/`
is a **third root beside `apps/`** — deliberately not scoped to one app id the way
`z_path_documents()` is. That scoping is exactly what a library has to break: the
screenshot service and the camera both write, and Photos, the share sheet and the wallpaper
picker all read. Pictures kept in the camera's private directory would make "set as
wallpaper" a copy, and the library would be a directory one app happens to own.

`media/` is a **convention, not a permission boundary**. Any process that can call
libzelto can read it. Gating enumeration belongs to the broker, not to a path.

## Identity

A photo's identity is its **filename stem**, and nothing else:

```
1784900253821-00        13 digits of capture time in ms, then a 2-digit sequence
```

Fixed width is the load-bearing part — it makes byte order equal chronological order, so
"newest first" is a reverse sort of the directory listing. No `stat()`, no mtime (which a
backup restore would rewrite), no stored index. A second boot on the same data directory —
which is how this OS tests persistence without an emulator — sees the same library, in the
same order, with the same ids.

## The index is the directory

There is no database. P11 shipped prefs, files and SQLite, and the directory still wins:

- **SQLite** is scoped to one app's documents dir, and it would make the index authoritative
  while the pixels live elsewhere — so a crash between the write and the `INSERT` leaves a
  photo that exists and is invisible. Two sources of truth that can disagree, to order data
  that is already ordered on disk.
- **A brokered media service** turns every enumeration into a socket round-trip and grows a
  second filesystem model inside zsysd. The broker arbitrates (permissions, settings,
  intents); listing a directory needs no arbitration.
- **The directory** has one source of truth — the bytes. A photo exists iff its file does,
  deleting is `unlink`, and the state is inspectable with `ls`.

**The cost, stated:** there is nowhere to put metadata that is not in the pixels or the
name — no album, no favourite, no caption — and sorting is by capture time. When one of
those is wanted, the answer is a **sidecar file per photo** (same stem, different
extension), which keeps "a photo is its file" true.

## Thumbnails are a second file

The writer already holds the pixels, so it emits a small sibling; the grid loads that and
the viewer loads the original. Different paths, so different image-cache entries by
construction.

This is the **opposite** of what the wallpaper picker does, and both are right. The picker
shares one cache entry between its thumbnail and the full-screen wallpaper: about ten fixed
assets, where a downscaled decode would duplicate a bitmap that is about to be needed at
full size anyway. A photo library inverts every term — the count is unbounded and
user-driven, a grid shows dozens at once, and one 720×1440 frame is ~4 MB decoded.

The thumbnail's short edge is the **grid cell**, derived from the column count and the
spacing scale rather than chosen.

## Using it

```c
#include "common/photos.h"

char ids[ZELTO_PHOTOS_MAX][ZELTO_PHOTO_ID_MAX];
int n = zelto_photos_list(ids, ZELTO_PHOTOS_MAX);   // newest first

char path[ZELTO_PHOTO_PATH_MAX];
zelto_photo_display_path(ids[0], path, sizeof(path));   // thumb, or the full image
```

Writing needs an id and a buffer:

```c
char id[ZELTO_PHOTO_ID_MAX];
zelto_photo_new_id(id, sizeof(id));
zelto_photo_store(id, argb, w, h, stride, /*premultiplied=*/false);
```

`premultiplied` has no default because a wrong answer is invisible on an opaque image and
wrong only where alpha < 255 — it would ship looking correct. libzelto's canvas and decoded
bitmaps are premultiplied; a `wl_shm` buffer and a wlr-screencopy frame are straight.

`zelto_photo_delete(id)` removes the image **and** its thumbnail. A thumb outliving its
photo would show a picture the library says is gone.

## Screenshots

The compositor owns the chord (`Print`, or power+volume-down on a handset) and forks
`zelto-shot`, which copies the output through wlr-screencopy, stores it, and posts a
notification carrying the thumbnail.

**The lock guard is in the compositor**, next to the identical guard that stops a
backgrounded window being photographed under a lock screen. Only `zcomp` knows a modal
layer surface is holding the screen, and a check inside `zelto-shot` would be a client
applying a rule to itself — a claim, not a control.

What that does **not** cover: wlr-screencopy is still bindable by any client, so this stops
the *system* screenshot path rather than every possible capture. Closing that would also
stop `grim`, which is how the shot catalogue photographs the lock screen.

## Browsing, and what you can do with a photo

`zelto-photos` is the reader: a grid of thumbnails, and a full-screen viewer you
reach by tapping one. Back out of the viewer with the system gesture — it is a
`Navigator`, so the edge-swipe and Escape pop it like any other screen in the OS.

| Action | What it actually does |
|---|---|
| Swipe left / right | Steps to the next or previous photo, clamped at both ends. |
| Share | `z_share` with `mime = image/png` and the photo's **path** as the payload. |
| Wallpaper | Writes `sys.wallpaper` (P25). The launcher and lock screen repaint live. |
| Delete | Asks first, then unlinks the photo **and** its thumbnail. |

**Share carries a path, not bytes.** `z_share` moves text (binary items are
Planned), and a path is a working handle here precisely because the media root is
shared rather than app-scoped — the receiver can open it. A receiving app
declares `share_targets=image/png` in its manifest.

**Delete is the only irreversible thing in this app.** There is no trash: the
bytes are unlinked. So it is confirmed, and the confirmation is tested as a real
gate — one tap must *not* delete — rather than assumed to be one.

### Driving it without coordinates

`ZELTO_PHOTOS_VIEW=<index>` opens the viewer on a photo at boot (the
`ZELTO_SETTINGS_SCREEN` idiom), and `ZELTO_PHOTOS_ROOT` points the library at a
prepared directory. Controls are pressed by label:

```sh
ZELTO_TAP_LABEL="Delete,Delete Photo" ZELTO_TAP_APP=os.zelto.photos
```

`ZELTO_TAP_LABEL` takes a **comma-separated sequence**, because a control behind
a confirmation did not exist when the first tap was arranged and so cannot be
reached by any single hook — or by a coordinate.
