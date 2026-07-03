// Shared app-icon helpers for the System UI (launcher + recents).
//
// Every launchable app declares a 512x512 icon in its manifest (`icon=` — a PNG
// or an SVG). When an app has no icon, or its icon fails to load, the UI falls
// back to one shared placeholder. This header is the single source of truth for
// that placeholder path (so the launcher and Recents agree) and a tiny helper to
// resolve an app_id back to its manifest `icon=` for surfaces that only have the
// app_id (Recents, which lists foreign-toplevel windows, not manifests).
//
// The on-device placeholder lives at ZELTO_PLACEHOLDER_DEFAULT; the desktop
// simulator overrides it with $ZELTO_PLACEHOLDER_ICON pointing into the in-repo
// resources/ tree (matching the ZELTO_RECENTS_BIN idiom in meta/run-sim.sh).
#ifndef ZELTO_APP_ICONS_H
#define ZELTO_APP_ICONS_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The baked-in placeholder icon (a 512x512 PNG shipped by build-initramfs.sh).
#define ZELTO_PLACEHOLDER_DEFAULT "/usr/share/zelto/app-icons/Placeholder.png"

// Manifest directories the launcher/zsysd scan (mirrors launcher main.c).
#define ZELTO_MANIFEST_DIR "/usr/share/zelto/apps"

// Resolve the placeholder icon path: the $ZELTO_PLACEHOLDER_ICON override (used
// by the simulator to point at the in-repo asset) or the on-device default.
static inline const char *zelto_placeholder_icon(void) {
    const char *env = getenv("ZELTO_PLACEHOLDER_ICON");
    return (env && env[0]) ? env : ZELTO_PLACEHOLDER_DEFAULT;
}

// Copy `src` into `out` (size `cap`), truncating safely.
static inline void zelto_icon_copy(char *out, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

// Scan one manifest dir for a .app whose `id=` field equals `app_id`; on a match
// copy its `icon=` value into `out` and return true. NB: manifests are named by
// exec basename (e.g. zelto-cards.app), NOT by app_id, so we must read each and
// match the id= field — the app_id here is the xdg app_id a window reports.
static inline bool zelto_icon_scan_dir(const char *dir, const char *app_id,
                                       char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(d))) {
        size_t nl = strlen(de->d_name);
        if (nl < 5 || strcmp(de->d_name + nl - 4, ".app") != 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char line[512], id[128] = "", icon[384] = "";
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "id=", 3) == 0) {
                zelto_icon_copy(id, sizeof(id), line + 3);
            } else if (strncmp(line, "icon=", 5) == 0) {
                zelto_icon_copy(icon, sizeof(icon), line + 5);
            }
        }
        fclose(f);
        if (icon[0] && strcmp(id, app_id) == 0) {
            zelto_icon_copy(out, cap, icon);
            found = true;
        }
    }
    closedir(d);
    return found;
}

// Resolve a running window's `app_id` to its manifest `icon=` (into `out`).
// Looks in the baked-in manifest dir and, if set, $ZELTO_DATA_DIR/apps/manifests
// (runtime-installed packages). Returns true if an icon path was found. Used by
// Recents, which has only the window app_id, not the launcher's parsed table.
static inline bool zelto_icon_for_app_id(const char *app_id, char *out,
                                         size_t cap) {
    if (!app_id || !app_id[0]) {
        return false;
    }
    if (zelto_icon_scan_dir(ZELTO_MANIFEST_DIR, app_id, out, cap)) {
        return true;
    }
    const char *data = getenv("ZELTO_DATA_DIR");
    if (data && data[0]) {
        char rt[384];
        snprintf(rt, sizeof(rt), "%s/apps/manifests", data);
        return zelto_icon_scan_dir(rt, app_id, out, cap);
    }
    return false;
}

// Scan one manifest dir for the .app whose `id=` equals `app_id`, copying its
// `name=` (the human display name) into `out`. Mirrors zelto_icon_scan_dir.
static inline bool zelto_name_scan_dir(const char *dir, const char *app_id,
                                       char *out, size_t cap) {
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    struct dirent *de;
    bool found = false;
    while (!found && (de = readdir(d))) {
        size_t nl = strlen(de->d_name);
        if (nl < 5 || strcmp(de->d_name + nl - 4, ".app") != 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char line[512], id[128] = "", name[128] = "";
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (strncmp(line, "id=", 3) == 0) {
                zelto_icon_copy(id, sizeof(id), line + 3);
            } else if (strncmp(line, "name=", 5) == 0) {
                zelto_icon_copy(name, sizeof(name), line + 5);
            }
        }
        fclose(f);
        if (name[0] && strcmp(id, app_id) == 0) {
            zelto_icon_copy(out, cap, name);
            found = true;
        }
    }
    closedir(d);
    return found;
}

// Resolve a running window's `app_id` to its manifest `name=` (the human display
// name, e.g. "Fetch") into `out`. A foreign-toplevel window otherwise reports its
// xdg title, which for a libzelto app is the body-function symbol (e.g.
// "fetch_body") — useless in a task switcher. Returns true on a match.
static inline bool zelto_name_for_app_id(const char *app_id, char *out,
                                         size_t cap) {
    if (!app_id || !app_id[0]) {
        return false;
    }
    if (zelto_name_scan_dir(ZELTO_MANIFEST_DIR, app_id, out, cap)) {
        return true;
    }
    const char *data = getenv("ZELTO_DATA_DIR");
    if (data && data[0]) {
        char rt[384];
        snprintf(rt, sizeof(rt), "%s/apps/manifests", data);
        return zelto_name_scan_dir(rt, app_id, out, cap);
    }
    return false;
}

#endif  // ZELTO_APP_ICONS_H
