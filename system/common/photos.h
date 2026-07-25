// THE PHOTO LIBRARY — the shared data layer every writer and every reader of
// user-created images agrees on.
//
// ---------------------------------------------------------------------------
// WHY A LIBRARY AND NOT A DIRECTORY (the decision this header exists to record)
// ---------------------------------------------------------------------------
// Until P53 nothing in this OS could make an image, so nothing needed a place to
// put one. The moment two things can — the screenshot service and the camera —
// the question stops being "where does this app save its files" and becomes
// "what is the identity of a photo". The test of the answer is not the writer;
// it is whether a SECOND, UNRELATED app can enumerate the result. That test is
// concrete rather than hypothetical: the P25 wallpaper picker should be able to
// offer your own pictures, and if it cannot then this was never a library, only
// a directory that Photos happens to own.
//
// So the pieces are:
//
//   WHERE      <data>/media/photos          full-size PNGs
//              <data>/media/thumbs           one small sibling per photo
//   IDENTITY   the FILENAME STEM. Nothing else.
//   INDEX      the directory itself.
//
// ---------------------------------------------------------------------------
// WHY THE DIRECTORY IS THE INDEX (and not SQLite, and not a zsysd service)
// ---------------------------------------------------------------------------
// P11 shipped all three storage primitives, so all three were available and the
// choice had to be made on merit rather than availability.
//
//   SQLite. z_db_open is scoped to ONE app's documents dir, which is exactly the
//     scoping a library has to break. Two processes writing one SQLite file
//     across a shared path is a locking design, and it would make the index the
//     authority while the pixels live somewhere else — so a crash between the
//     write and the INSERT leaves a photo that exists and is invisible, and a
//     failed delete leaves a row pointing at nothing. Two sources of truth that
//     can disagree, to index data that is already ordered on disk.
//
//   A zsysd-brokered media service. Every enumeration becomes a socket
//     round-trip, the broker grows a second filesystem model, and the grid — a
//     surface that rebuilds its whole tree every frame — pays for it. zsysd
//     brokers things that need ARBITRATION (a permission, a setting, an intent).
//     Listing a directory needs none.
//
//   THE DIRECTORY. There is one source of truth, it is the bytes; a photo exists
//     if and only if its file does. Deleting is unlink. Nothing can drift out of
//     sync with itself, and the state is inspectable with `ls`, which matters for
//     a project whose recurring bug is a number quietly wrong everywhere at once.
//
// THE COST, STATED. There is nowhere to put metadata that is not in the pixels
// or the name — no album, no favourite, no caption, and sorting is by capture
// time because that is what the name encodes. When one of those is wanted, the
// answer is a SIDECAR file per photo (same stem, different extension), which
// keeps the "a photo is its file" invariant, not a central index that can
// disagree with the directory. Recorded here so the next phase does not
// rediscover the trade-off as a surprise.
//
// ---------------------------------------------------------------------------
// THE IDENTITY SURVIVES A REBOOT BECAUSE IT IS THE FILENAME
// ---------------------------------------------------------------------------
// "<epoch_ms>-<seq>" — 13 fixed-width digits of capture time, then a 2-digit
// sequence that only moves when two captures land in the same millisecond.
// FIXED WIDTH IS THE POINT: it makes byte order equal chronological order, so
// "newest first" is a reverse sort of the directory listing and needs no stat(),
// no mtime (which a copy or a backup restore would rewrite) and no stored index.
// A second boot on the same ZELTO_DATA_DIR — which is how this OS tests
// persistence without an emulator — sees exactly the same library, in the same
// order, with the same ids.
//
// ---------------------------------------------------------------------------
// THUMBNAILS ARE A SECOND FILE, AND P25 DELIBERATELY DID THE OPPOSITE
// ---------------------------------------------------------------------------
// The wallpaper picker SHARES one image-cache entry between its thumbnail and
// the full-screen wallpaper, and that was the right call there and is recorded
// as deliberate: ~10 fixed assets, and a downscaled decode would have duplicated
// a bitmap that was about to be needed at full size anyway. A photo library
// inverts every term. The count is unbounded and user-driven, the grid wants
// dozens at once, and one 720x1440 frame is ~4 MB decoded — thirty of them is
// 120 MB against image.c's 64-entry cache, which would thrash and would still be
// holding full frames to draw 216-unit tiles. So the WRITER, which already has
// the pixels in hand, emits a small sibling; the grid loads that and the viewer
// loads the original. They are different paths, so they are different cache
// entries by construction rather than by policy.
//
// The thumb's short edge is the GRID CELL, derived below — not a round number.
#ifndef ZELTO_PHOTOS_H
#define ZELTO_PHOTOS_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include <zelto/ui.h>

#define ZELTO_PHOTO_ID_MAX 24        // "0123456789012-00" + slack
#define ZELTO_PHOTO_PATH_MAX 320
// An upper bound on what one enumeration returns. The grid scrolls, so this is a
// ceiling on the library a build can show at once rather than on the library.
#define ZELTO_PHOTOS_MAX 512

// The app id the library belongs to — the poster of a screenshot notification,
// the tap route target, and the owner of the Photos tile.
#define ZELTO_PHOTOS_APP_ID "os.zelto.photos"

// --- the browse grid, declared here because the WRITER needs it too ---------
// The cell size is an EXPRESSION over the column count and the scale's own
// steps, never a literal — P52 made the wallpaper picker's cell derived for this
// reason and copying its NUMBER would have re-introduced exactly what it fixed.
#define ZELTO_PHOTOS_COLS 3
#define ZELTO_PHOTOS_MARGIN ((float)Z_SPACE_L)
#define ZELTO_PHOTOS_GAP    ((float)Z_SPACE_XS)

// One cell of the grid, given the width available to the grid itself.
static inline float zelto_photos_cell(float grid_w) {
    float inner = grid_w - 2.0f * ZELTO_PHOTOS_MARGIN -
                  (float)(ZELTO_PHOTOS_COLS - 1) * ZELTO_PHOTOS_GAP;
    float cell = inner / (float)ZELTO_PHOTOS_COLS;
    return cell > 1.0f ? cell : 1.0f;
}

// The thumbnail's SHORT edge, in pixels. A cell is square and a thumb is
// aspect-FILLed into it, so covering the cell means the short edge reaches it —
// and one logical unit is one physical pixel on this geometry (the dossier's
// unit rule), so no oversampling factor is owed. 390pt is the design width Z_PT's
// own ratio was derived from, which is why it appears here as points rather than
// as the number 720.
#define ZELTO_PHOTO_THUMB_EDGE ((int)(zelto_photos_cell((float)Z_PT(390)) + 0.5f))

// --- paths -----------------------------------------------------------------
// Both directories are created on first ask, like every other storage path in
// the system. The returned pointer is a per-call static buffer (the wallpaper.h
// idiom): copy it if you need to keep it.
static inline const char *zelto_photos_subdir(const char *sub) {
    static char buf[ZELTO_PHOTO_PATH_MAX];
    const char *env = getenv("ZELTO_PHOTOS_ROOT");
    if (env && env[0]) {
        snprintf(buf, sizeof(buf), "%s/%s", env, sub);
    } else {
        char *p = z_path_media(sub);
        if (!p) {
            buf[0] = '\0';
            return buf;
        }
        snprintf(buf, sizeof(buf), "%s", p);
        free(p);
    }
    z_mkdir_p(buf);
    return buf;
}

static inline const char *zelto_photos_dir(void) {
    return zelto_photos_subdir("photos");
}
static inline const char *zelto_thumbs_dir(void) {
    return zelto_photos_subdir("thumbs");
}

static inline void zelto_photo_path(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.png", zelto_photos_dir(), id ? id : "");
}
static inline void zelto_thumb_path(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.png", zelto_thumbs_dir(), id ? id : "");
}

// --- identity --------------------------------------------------------------

// Mint an id for a capture happening NOW. The sequence advances only on a
// same-millisecond collision with a file that already exists, so an id is stable
// the instant it is chosen and two writers racing cannot pick the same one
// (open(O_EXCL) by the caller closes the remaining window; at human capture
// rates it never opens).
static inline bool zelto_photo_new_id(char *out, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    for (int seq = 0; seq < 100; seq++) {
        char id[ZELTO_PHOTO_ID_MAX];
        snprintf(id, sizeof(id), "%013lld-%02d", ms, seq);
        char path[ZELTO_PHOTO_PATH_MAX];
        zelto_photo_path(id, path, sizeof(path));
        if (access(path, F_OK) != 0) {
            snprintf(out, cap, "%s", id);
            return true;
        }
    }
    return false;
}

// --- enumeration -----------------------------------------------------------

// True if `name` is a photo file this library owns: "<stem>.png", and not the
// ".part" temp a write in progress leaves (z_image_write_png renames into
// place, so a .part is by definition not yet a photo).
static inline bool zelto_photo_is_file(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".png") == 0 && name[0] != '.';
}

// List the library NEWEST FIRST into `ids` (each ZELTO_PHOTO_ID_MAX). Returns
// the count. Newest-first is a reverse lexicographic sort and nothing else,
// because the id is fixed-width time — see the note at the top.
static inline int zelto_photos_list(char ids[][ZELTO_PHOTO_ID_MAX], int cap) {
    const char *dir = zelto_photos_dir();
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while (n < cap && (de = readdir(d))) {
        if (!zelto_photo_is_file(de->d_name)) {
            continue;
        }
        size_t len = strlen(de->d_name) - 4;    // drop ".png"
        if (len == 0 || len >= ZELTO_PHOTO_ID_MAX) {
            continue;
        }
        memcpy(ids[n], de->d_name, len);
        ids[n][len] = '\0';
        n++;
    }
    closedir(d);
    // Insertion sort, DESCENDING. n is a screenful to a few hundred and this
    // runs on a build, not a frame.
    for (int i = 1; i < n; i++) {
        char tmp[ZELTO_PHOTO_ID_MAX];
        snprintf(tmp, sizeof(tmp), "%s", ids[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(ids[j], tmp) < 0) {
            snprintf(ids[j + 1], ZELTO_PHOTO_ID_MAX, "%s", ids[j]);
            j--;
        }
        snprintf(ids[j + 1], ZELTO_PHOTO_ID_MAX, "%s", tmp);
    }
    return n;
}

static inline int zelto_photos_count(void) {
    const char *dir = zelto_photos_dir();
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (zelto_photo_is_file(de->d_name)) {
            n++;
        }
    }
    closedir(d);
    return n;
}

// Remove a photo AND its thumbnail. The thumb is derived data, so a photo whose
// thumb outlived it would show a picture the library says is gone — the delete
// has to be one operation or it is not a delete.
static inline bool zelto_photo_delete(const char *id) {
    if (!id || !id[0]) {
        return false;
    }
    char p[ZELTO_PHOTO_PATH_MAX], t[ZELTO_PHOTO_PATH_MAX];
    zelto_photo_path(id, p, sizeof(p));
    zelto_thumb_path(id, t, sizeof(t));
    bool ok = (remove(p) == 0);
    remove(t);            // best effort: a missing thumb is not a failed delete
    return ok;
}

// Write one photo into the library: the full-size PNG and its thumbnail, from
// one buffer the caller already holds. `id` must come from zelto_photo_new_id.
// Returns false if the FULL image did not land — a library entry without a thumb
// is degraded (the grid falls back), a thumb without an image is a ghost.
static inline bool zelto_photo_store(const char *id, const void *argb, int w,
                                     int h, size_t stride, bool premultiplied) {
    if (!id || !id[0] || !argb || w <= 0 || h <= 0) {
        return false;
    }
    char full[ZELTO_PHOTO_PATH_MAX];
    zelto_photo_path(id, full, sizeof(full));
    if (!z_image_write_png(full, argb, w, h, stride, premultiplied)) {
        return false;
    }

    int shortest = w < h ? w : h;
    int edge = ZELTO_PHOTO_THUMB_EDGE;
    if (shortest > edge) {
        // Preserve the aspect ratio: scale so the SHORT edge lands on the cell.
        int tw = (w == shortest) ? edge : (int)((long)w * edge / shortest);
        int th = (h == shortest) ? edge : (int)((long)h * edge / shortest);
        uint32_t *small = z_image_box_scale(argb, w, h, stride, tw, th);
        if (small) {
            char thumb[ZELTO_PHOTO_PATH_MAX];
            zelto_thumb_path(id, thumb, sizeof(thumb));
            z_image_write_png(thumb, small, tw, th, (size_t)tw * 4,
                              premultiplied);
            free(small);
        }
    } else {
        // Already smaller than a cell: the photo IS its own thumbnail. Copying
        // it would double the bytes to say nothing.
        char thumb[ZELTO_PHOTO_PATH_MAX];
        zelto_thumb_path(id, thumb, sizeof(thumb));
        z_image_write_png(thumb, argb, w, h, stride, premultiplied);
    }
    return true;
}

// The path the grid should draw for `id`: the thumbnail if it exists, else the
// full image. The fallback is what keeps a thumb-write failure a performance
// problem rather than a blank tile.
static inline void zelto_photo_display_path(const char *id, char *out,
                                            size_t cap) {
    char t[ZELTO_PHOTO_PATH_MAX];
    zelto_thumb_path(id, t, sizeof(t));
    if (access(t, R_OK) == 0) {
        snprintf(out, cap, "%s", t);
        return;
    }
    zelto_photo_path(id, out, cap);
}

#endif  // ZELTO_PHOTOS_H
