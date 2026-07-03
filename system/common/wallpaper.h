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

#endif  // ZELTO_WALLPAPER_H
