// Shared wallpaper helpers for the System UI (launcher + lock + settings).
//
// Full-screen home/lock backgrounds live as PNGs in one directory. The ACTIVE
// wallpaper is a single absolute path stored in the zsysd settings broker under
// the key sys.wallpaper (persisted to /var/zelto, fanned out to observers) — so a
// pick in Settings updates the home + lock screen live and survives a reboot,
// exactly like the other sys.* toggles. This header is the single source of truth
// for the wallpaper directory, the broker key, and the resolve helpers so those
// three surfaces agree.
//
// The on-device directory is ZELTO_WALLPAPER_DIR_DEFAULT; the desktop simulator
// overrides it with $ZELTO_WALLPAPER_DIR pointing into the in-repo resources/
// tree (matching the ZELTO_PLACEHOLDER_ICON idiom in meta/run-sim.sh).
#ifndef ZELTO_WALLPAPER_H
#define ZELTO_WALLPAPER_H

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zelto/ui.h>

// The baked-in wallpaper directory (PNGs installed by build-initramfs.sh).
#define ZELTO_WALLPAPER_DIR_DEFAULT "/usr/share/zelto/wallpaper"
// The brokered settings key holding the active wallpaper's absolute path.
#define ZELTO_WALLPAPER_KEY "sys.wallpaper"
// Upper bound on wallpapers we enumerate (the picker builds one thumbnail each).
#define ZELTO_WALLPAPER_MAX 24
#define ZELTO_WALLPAPER_PATH_MAX 256

// The wallpaper directory: $ZELTO_WALLPAPER_DIR (the simulator override) or the
// on-device default.
static inline const char *zelto_wallpaper_dir(void) {
    const char *env = getenv("ZELTO_WALLPAPER_DIR");
    return (env && env[0]) ? env : ZELTO_WALLPAPER_DIR_DEFAULT;
}

// True if `name` ends in ".png" (case-insensitive).
static inline bool zelto_wallpaper_is_png(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".png") == 0;
}

// List the wallpaper directory into `paths` (each ZELTO_WALLPAPER_PATH_MAX), as
// absolute paths sorted by name for a deterministic grid (the headless harness
// reads fixed thumbnail coordinates). Returns the count (0 if the dir is absent).
static inline int zelto_wallpaper_list(
    char paths[][ZELTO_WALLPAPER_PATH_MAX], int cap) {
    const char *dir = zelto_wallpaper_dir();
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while (n < cap && (de = readdir(d))) {
        if (!zelto_wallpaper_is_png(de->d_name)) {
            continue;
        }
        snprintf(paths[n], ZELTO_WALLPAPER_PATH_MAX, "%s/%s", dir, de->d_name);
        n++;
    }
    closedir(d);
    // Insertion sort (n is tiny — a handful of files).
    for (int i = 1; i < n; i++) {
        char tmp[ZELTO_WALLPAPER_PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", paths[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(paths[j], tmp) > 0) {
            snprintf(paths[j + 1], ZELTO_WALLPAPER_PATH_MAX, "%s", paths[j]);
            j--;
        }
        snprintf(paths[j + 1], ZELTO_WALLPAPER_PATH_MAX, "%s", tmp);
    }
    return n;
}

// Copy the first (sorted) wallpaper in the directory into `out`. Returns false if
// the directory holds none. This is the seed value the launcher writes to the
// broker the first time (so the home/lock screen has a wallpaper out of the box).
static inline bool zelto_wallpaper_default(char *out, size_t cap) {
    char paths[ZELTO_WALLPAPER_MAX][ZELTO_WALLPAPER_PATH_MAX];
    int n = zelto_wallpaper_list(paths, ZELTO_WALLPAPER_MAX);
    if (n == 0) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    snprintf(out, cap, "%s", paths[0]);
    return true;
}

// Resolve the ACTIVE wallpaper into `out`: the broker's sys.wallpaper if it is set
// AND decodes, else the directory default. Returns true only when `out` holds a
// path that loads (so a caller can fall back to a drawn background otherwise).
// Read-only: it never writes the broker — seeding the default is the launcher's
// job (one owner), done once on first build.
static inline bool zelto_wallpaper_active(char *out, size_t cap) {
    const char *cur = z_setting_get_str(ZELTO_WALLPAPER_KEY, "");
    if (cur && cur[0] && z_image_loads(cur)) {
        snprintf(out, cap, "%s", cur);
        return true;
    }
    if (zelto_wallpaper_default(out, cap)) {
        return z_image_loads(out);
    }
    return false;
}

// --- THE INK THAT GOES ON THE WALLPAPER (P54) -------------------------------
//
// THE WALLPAPER IS A PHOTOGRAPH AND DOES NOT CHANGE WHEN THE APPEARANCE DOES.
// That is the whole of the problem, and it is the one place in this phase where
// a token table cannot answer: the status bar's clock, the home grid's captions,
// the home indicator and the lock screen's clock sit on the picture, not on a
// surface. Take their ink from the palette and a light appearance turns every
// one of them dark over a dark photograph.
//
// DERIVED, NOT DECLARED. Three ways to solve this were on the table:
//
//   ALWAYS SCRIM       cheapest, and it dulls every wallpaper the user chose. It
//                      also answers a question nobody asked: the picture is the
//                      one thing on this screen the user picked.
//   A FLAG IN THE ASSET  a "this one is dark" bit per wallpaper. Correct until
//                      somebody adds a wallpaper and forgets, and there is no
//                      way to notice — the flag and the pixels cannot disagree
//                      loudly.
//   MEASURE THE PIXELS which is what this does. The decode is already cached by
//                      path (P24), so the mean is one pass over pixels the
//                      process had anyway, and the answer cannot drift from the
//                      picture because it IS the picture.
//
// AND IT IS MEASURED PER REGION, which the measurement decided rather than
// taste. Over the ten shipped wallpapers, a WHOLE-IMAGE mean chooses a different
// ink from the band's own mean on FOUR of them; the worst spread is 0.364, on a
// wallpaper that reads 0.173 under the status bar, 0.373 behind the captions and
// 0.010 under the home indicator. One number for the whole picture is the wrong
// statistic for a surface that occupies part of it.
//
// THE FRACTIONS ARE SCREEN FRACTIONS, NOT IMAGE FRACTIONS, and getting that
// wrong is what the first version of this did. The wallpaper is drawn with
// Cover() — aspect-FILL with a centre crop — so a 946x2048 picture on a 720x1440
// screen is scaled to 720x1558 and loses 59 units off the top and the bottom.
// Asking the IMAGE for its top 5.6% therefore asks about a strip that is not on
// the screen at all: measured, the file's top band reads 0.169 (so: light ink)
// while the pixels actually under the status bar read 0.396 (so: dark). The
// clock came out washed out, and it came out that way in a frame, not in a test.
//
// So these helpers take the ZApp, ask the image its intrinsic size, and put the
// requested rectangle through the SAME aspect-fill transform Cover uses. This is
// the surface-versus-screen confusion the probe note in CLAUDE.md warns about,
// wearing a different hat.
//
// WHAT IT COSTS: the process has to decode the wallpaper. The launcher and the
// lock screen already did; the status bar and the home indicator did not, and
// now pay one decode at startup and on a wallpaper change (cached thereafter).
// That is the price of the choice being derived, and it is paid once per boot
// rather than per frame.
//
// WHAT IT DOES NOT DO: a mean cannot see a bright object inside a dark region.
// The caption text-shadow (Z_K_TEXT's text_shadow in sdk/src/render.c) and the
// launcher's bottom scrim still cover that variance — this picks the ink, they
// carry what is left.

// MEMOISED, and it has to be. The decode is cached but the SCAN is not, and a
// caller asks from inside body(): the launcher's grid_ink() is reached once per
// CELL, so sixteen times a build, at 40Hz — and each scan touches ~94k pixels of
// a 946x2048 wallpaper. That is tens of millions of pixel reads a second to
// answer a question whose inputs change when the user picks a new wallpaper.
//
// The key is the path and the rectangle, so the entries are per CALL SITE (each
// asks about the same patch every time) and a wallpaper change misses on the
// path. Four entries covers the four surfaces that ask; past that it overwrites
// the oldest, which for a fifth caller means a rescan per build rather than a
// wrong answer.
#define ZELTO_WP_LUMA_MEMO 4

typedef struct ZeltoWpLumaMemo {
    char path[ZELTO_WALLPAPER_PATH_MAX];
    float x0, y0, x1, y1;
    float luma;
    bool used;
} ZeltoWpLumaMemo;

// The mean luminance of the active wallpaper under a rectangle of the SCREEN
// (fractions of the screen's width and height), or -1 when there is no wallpaper
// (the launcher's drawn gradient, which is dark).
static inline float zelto_wallpaper_luma(ZApp *app, float x0, float y0,
                                         float x1, float y1) {
    char path[ZELTO_WALLPAPER_PATH_MAX];
    if (!zelto_wallpaper_active(path, sizeof(path))) {
        return -1.0f;
    }
    static ZeltoWpLumaMemo memo[ZELTO_WP_LUMA_MEMO];
    static int memo_next;
    for (int i = 0; i < ZELTO_WP_LUMA_MEMO; i++) {
        if (memo[i].used && memo[i].x0 == x0 && memo[i].y0 == y0 &&
            memo[i].x1 == x1 && memo[i].y1 == y1 &&
            strcmp(memo[i].path, path) == 0) {
            return memo[i].luma;
        }
    }
    float fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    int iw = 0, ih = 0;
    float sw = (float)z_screen_width(app), sh = (float)z_screen_height(app);
    if (z_image_intrinsic(path, &iw, &ih) && iw > 0 && ih > 0 && sw >= 1.0f &&
        sh >= 1.0f) {
        // Cover(): scale so the screen is fully covered, centre the overflow.
        float scale = sw / (float)iw;
        float other = sh / (float)ih;
        if (other > scale) {
            scale = other;
        }
        float dw = (float)iw * scale, dh = (float)ih * scale;
        float ox = (sw - dw) * 0.5f, oy = (sh - dh) * 0.5f;  // <= 0 if cropped
        // Screen fraction -> screen unit -> displayed unit -> image fraction.
        fx0 = (x0 * sw - ox) / dw;
        fx1 = (x1 * sw - ox) / dw;
        fy0 = (y0 * sh - oy) / dh;
        fy1 = (y1 * sh - oy) / dh;
    }
    float l = z_image_region_luma(path, fx0, fy0, fx1, fy1);

    int slot = memo_next % ZELTO_WP_LUMA_MEMO;
    memo_next++;
    snprintf(memo[slot].path, sizeof(memo[slot].path), "%s", path);
    memo[slot].x0 = x0;
    memo[slot].y0 = y0;
    memo[slot].x1 = x1;
    memo[slot].y1 = y1;
    memo[slot].luma = l;
    memo[slot].used = true;
    return l;
}

// The ink to draw on the wallpaper under a rectangle of the SCREEN. Goes through
// z_on_fill, which is the OS's one rule for "what ink belongs on this colour" —
// so the ink over a photograph is chosen by the same computation as the ink on a
// semantic fill, and the two cannot drift apart. A region with no wallpaper
// behind it is treated as dark, because the drawn fallback gradient is
// (#161d38 to #05070e).
static inline ZColor zelto_wallpaper_ink(ZApp *app, float x0, float y0,
                                         float x1, float y1) {
    float l = zelto_wallpaper_luma(app, x0, y0, x1, y1);
    if (l < 0.0f) {
        l = 0.02f;
    }
    // z_on_fill takes a COLOUR, so the measured luminance becomes the grey with
    // that luminance. Inverting the sRGB transfer keeps the grey the one whose
    // contrast maths matches the band it stands for.
    float c = l <= 0.0031308f ? l * 12.92f
                              : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    int v = (int)(c * 255.0f + 0.5f);
    if (v < 0) { v = 0; }
    if (v > 255) { v = 255; }
    return z_on_fill(z_rgba((uint8_t)v, (uint8_t)v, (uint8_t)v, 0xff));
}

#endif  // ZELTO_WALLPAPER_H
