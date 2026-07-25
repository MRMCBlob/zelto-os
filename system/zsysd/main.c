// zsysd — the Zelto system-service broker.
//
// The single authority for runtime permissions (and, later, lifecycle, packages
// and intents). It speaks a dead-simple, newline-delimited JSON-ish protocol
// over a SOCK_STREAM unix socket at $XDG_RUNTIME_DIR/zsysd.sock — no D-Bus, the
// framing is hand-rolled. Requests:
//
//     {"op":"perm_status","app_id":"os.zelto.cards","perm":"camera"}
//     {"op":"perm_request","app_id":"os.zelto.cards","perm":"camera"}
//
// Reply (one line):  {"status":"granted|denied|prompt"}
//
// Policy:
//   - A permission the app does not DECLARE in its manifest is auto-denied (no
//     prompt). Declarations come from /usr/share/zelto/apps/<id>.app, extended
//     with a `permissions=cam,net,...` line (and an `id=` line to key by app_id).
//   - A cached grant (granted/denied) is returned immediately.
//   - Otherwise perm_status answers "prompt"; perm_request shows the System-UI
//     consent dialog (a separate overlay layer-shell helper, /usr/bin/zelto-
//     consent), records the user's choice, and returns it.
//
// Grants live in memory only (a table keyed by app_id+perm), seeded empty.
// Single-threaded poll() loop; the prompt path blocks on the consent child,
// which is intentional — the request is modal, the requesting app waits async.
// See docs/contributing/services-and-ipc.md + docs/platform/permissions.md.
// (_GNU_SOURCE for strtok_r/etc. comes from the project-wide build args.)
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common/exec_cmd.h"

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define CONSENT_BIN "/usr/bin/zelto-consent"
#define CHOOSER_BIN "/usr/bin/zelto-chooser"
#define MAX_GRANTS 128
#define MAX_MANIFESTS 64
#define MAX_CLIENTS 32
#define MAX_PENDING 16
#define MAX_NOTIFS 32
#define MAX_SETTINGS 64
#define REQ_MAX 512

// --- power source (zsysd's 5th duty, P23) ----------------------------------
// The battery + charging state surface through the SAME settings keys (sys.
// battery_pct 0..100, sys.battery_charging 0/1) so the status bar reads them via
// the existing settings_subscribe fan-out — no new protocol. A slow drain timer
// (fake source) makes the level driveable without hardware; a real sysfs read
// backs the same keys where a battery exists (QEMU/host swap later). At/below the
// threshold a one-shot low-battery notification is posted (reuse of the notify
// store) and brightness is nudged down.
#define BATT_TICK_MS 5000        // default drain/refresh cadence
#define LOW_BATT_THRESHOLD 20    // % at/below which we warn once

// --- manifest table (per-app declarations) --------------------------------
// Beyond permissions, manifests now declare the intent handlers an app
// provides: `share_targets=` (MIME globs it accepts shared content for) and
// `links=` (URL schemes it opens). `exec=` is the binary zsysd launches when an
// intent targets an app that is not currently running.
typedef struct Manifest {
    char id[96];
    char perms[256];          // CSV of declared permission names
    char share_targets[256];  // CSV of accepted MIME globs ("text/plain,image/*")
    char links[128];          // CSV of handled URL schemes ("zelto,myapp")
    char exec[256];           // launch command (path + args) for launch-if-needed
    bool no_snapshot;         // `no_snapshot=1`: never photograph this window
} Manifest;
static Manifest g_manifests[MAX_MANIFESTS];
static int g_n_manifests;

// --- intents mailbox + pending queue --------------------------------------
// Each registered control connection is an app's mailbox: app_id -> client fd.
// A delivered intent for an app with no live mailbox is queued here and flushed
// when that app_id registers (after zsysd launches it).
typedef struct Pending {
    bool used;
    char app_id[96];
    char kind[16];    // "share" | "open_url" | "notify_action"
    char mime[64];
    char url[256];
    char payload[256];   // share payload, or (for notify_action) the action id
    int64_t notif_id;    // notify_action only: the notification id
} Pending;
static Pending g_pending[MAX_PENDING];

// app_id registered on each client slot (empty until it sends {"op":"register"}).
static char g_client_app[MAX_CLIENTS][96];
static int g_client_fd[MAX_CLIENTS];   // mirror of clients[] for mailbox lookup

// The listening socket, and the state that lets the daemon keep serving while a
// consent dialog is up (see show_consent). g_in_consent is true only while a
// dialog is on screen; a perm-gated request that arrives in that window is
// stashed in g_deferred and replayed once the dialog closes, so two apps asking
// at once queue up instead of stacking two dialogs.
#define MAX_DEFERRED 16
static int g_lfd = -1;
static bool g_in_consent;
typedef struct Deferred {
    int slot;
    char line[REQ_MAX];
} Deferred;
static Deferred g_deferred[MAX_DEFERRED];
static int g_n_deferred;

static void serve_once(int timeout_ms);

// --- grant store (the cached decisions) -----------------------------------
typedef struct Grant {
    char app_id[96];
    char perm[32];
    bool granted;
    bool used;
} Grant;
static Grant g_grants[MAX_GRANTS];

// --- notification store + sinks --------------------------------------------
// zsysd's third duty: a notification store + router. A perm-gated notify_post
// assigns a monotonic global id, stores the notification, and pushes a
// notify_show to every subscribed SINK. A sink is a libzelto surface that sent
// notify_subscribe, recorded by its ctrl fd. A sink reports a body tap
// (notify_tap -> drop the banner; it routed the deep link itself via z_open_url)
// or an action tap (notify_action -> route to the poster's mailbox, then drop).
// Cancel removes a notification by id. The store is in-memory only (lost on
// reboot); no history/grouping (heads-up only).
//
// WHY A SET AND NOT LAST-WINS. This was one fd — "the shade sink" — because for
// P10 the shade was the only surface that displayed notifications. The lock
// screen shows them too (P41), and it is a SEPARATE process, so last-wins made
// the two surfaces silently exclusive: whichever subscribed later took the sink
// and the other never saw another notification again. Since init starts the lock
// last, that would have been the shade — the heads-up banner, gone. A set fans
// out to both, exactly like the settings-subscriber set below, and a drop of a
// notification reaches every surface showing it. Any sink may report a tap; the
// notification is dropped once and the hide is fanned out to all of them.
typedef struct Notification {
    bool used;
    int64_t id;
    char app_id[96];
    char title[128];
    char body[192];
    char channel[64];
    char tap_route[256];
    char action_id[64];
    char action_title[64];
    // An IMAGE the notification is ABOUT — the thumbnail of the thing that just
    // happened, not the poster's icon (that is resolved from app_id, as it
    // always was). A screenshot notification showing a grey camera glyph tells
    // you an event occurred; one showing the picture tells you WHICH. An
    // absolute path into the photo library: the broker moves the string and
    // never opens the file, so it stays a router rather than growing a second
    // image pipeline.
    char image[256];
} Notification;
static Notification g_notifs[MAX_NOTIFS];
static int64_t g_next_notif_id = 1;
static int g_sink_fds[MAX_CLIENTS];   // ctrl fds of the subscribed sinks
static int g_n_sinks;

// Register / drop a notification sink. Subscribing twice on one fd is a no-op, so
// a surface that re-subscribes after a rebuild does not get double pushes.
static void sink_subscribe_fd(int fd) {
    for (int i = 0; i < g_n_sinks; i++) {
        if (g_sink_fds[i] == fd) {
            return;
        }
    }
    if (g_n_sinks < MAX_CLIENTS) {
        g_sink_fds[g_n_sinks++] = fd;
    }
}
static void sink_unsubscribe_fd(int fd) {
    for (int i = 0; i < g_n_sinks; i++) {
        if (g_sink_fds[i] == fd) {
            g_sink_fds[i] = g_sink_fds[--g_n_sinks];
            return;
        }
    }
}

// --- settings store (zsysd's 4th duty) -------------------------------------
// A single source of truth for system toggles. Values are strings (bools as
// "0"/"1"); keys are namespaced under `sys.` (sys.wifi / sys.mute / sys.bright /
// sys.airplane / sys.brightness). Unlike the notification store (in-memory, lost
// on reboot) this is PERSISTED: every settings_set writes the whole table
// through to $ZELTO_DATA_DIR/settings.conf (TAB-separated key\tvalue, fsync'd)
// like the libzelto storage client, so a flipped toggle survives a reboot.
//
// Three ops over the same line protocol:
//   settings_get        (key)        -> {"value":"<v>"} on the same conn (fast,
//                                        in-memory; never blocks, so no consent
//                                        round-trip and the client can read it
//                                        synchronously like perm_status).
//   settings_set        (key, value) -> update + persist + broadcast to every
//                                        subscriber as settings_changed.
//   settings_subscribe  ()           -> register this ctrl fd to receive
//                                        settings_changed pushes (like the notify
//                                        sink, but a SET of fds — the shade AND
//                                        the Settings app observe at once).
// The setter is broadcast to as well (it is just another subscriber); clients
// apply changes idempotently, so a setter that also observes neither loops nor
// double-applies.
typedef struct Setting {
    bool used;
    char key[64];
    char value[160];
} Setting;
static Setting g_settings[MAX_SETTINGS];

// Subscriber ctrl fds (the persistent connections that asked for settings_changed
// pushes). A SET, not last-wins: several apps observe simultaneously. Cleared per
// fd on its disconnect in the read loop (mirroring the notification sink set).
static int g_settings_subs[MAX_CLIENTS];
static int g_n_settings_subs;

// --- power source state ----------------------------------------------------
static bool g_batt_active;      // is a battery source driven at all?
static bool g_fake_battery;     // fake drain (sim/harness) vs real sysfs read
static int g_batt_tick_ms = BATT_TICK_MS;
static int64_t g_next_batt_ms;  // monotonic ms of the next tick
static bool g_low_warned;       // one-shot latch for the low-battery notification

// Forward declarations: the notification store (defined early) publishes a live
// count through the settings store (defined later), and vice versa.
static bool settings_apply(const char *key, const char *value);
static void publish_notif_count(void);

// Strip a trailing CR/LF in place.
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

// Extract a quoted string value for "key" out of a JSON-ish line. Our own
// protocol only ever sends quoted string values, so this is enough (no nesting,
// no escapes). Returns false if the key is absent.
static bool json_get(const char *buf, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// Is `item` one of the comma-separated entries in `csv`?
static bool csv_contains(const char *csv, const char *item) {
    size_t il = strlen(item);
    const char *p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = (size_t)(p - start);
        while (len > 0 && start[len - 1] == ' ') {
            len--;
        }
        if (len == il && strncmp(start, item, il) == 0) {
            return true;
        }
    }
    return false;
}

// Scan one manifest dir, appending each <id>.app to g_manifests.
static void scan_manifest_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) && g_n_manifests < MAX_MANIFESTS) {
        size_t ln = strlen(de->d_name);
        if (ln < 5 || strcmp(de->d_name + ln - 4, ".app") != 0) {
            continue;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        Manifest m = {0};
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            chomp(line);
            if (line[0] == '#') {
                continue;
            }
            char *eq = strchr(line, '=');
            if (!eq) {
                continue;
            }
            *eq = '\0';
            const char *k = line, *v = eq + 1;
            if (strcmp(k, "id") == 0) {
                snprintf(m.id, sizeof(m.id), "%s", v);
            } else if (strcmp(k, "permissions") == 0) {
                snprintf(m.perms, sizeof(m.perms), "%s", v);
            } else if (strcmp(k, "share_targets") == 0) {
                snprintf(m.share_targets, sizeof(m.share_targets), "%s", v);
            } else if (strcmp(k, "links") == 0) {
                snprintf(m.links, sizeof(m.links), "%s", v);
            } else if (strcmp(k, "exec") == 0) {
                snprintf(m.exec, sizeof(m.exec), "%s", v);
            } else if (strcmp(k, "no_snapshot") == 0) {
                m.no_snapshot = atoi(v) != 0;
            }
        }
        fclose(f);
        if (m.id[0]) {
            g_manifests[g_n_manifests++] = m;
        }
    }
    closedir(d);
}

// (Re)build g_manifests from BOTH the baked-in dir and the runtime-installed dir
// ($ZELTO_DATA_DIR/apps/manifests, where zelto-install writes a verified .zap's
// manifest, P13). Called at startup and on a {"op":"reload"} from the installer.
static void load_manifests(void) {
    g_n_manifests = 0;
    scan_manifest_dir(MANIFEST_DIR);
    const char *data = getenv("ZELTO_DATA_DIR");
    if (data && data[0]) {
        char runtime_dir[256];
        snprintf(runtime_dir, sizeof(runtime_dir), "%s/apps/manifests", data);
        scan_manifest_dir(runtime_dir);
    }
}

static bool manifest_declares(const char *app_id, const char *perm) {
    for (int i = 0; i < g_n_manifests; i++) {
        if (strcmp(g_manifests[i].id, app_id) == 0) {
            return csv_contains(g_manifests[i].perms, perm);
        }
    }
    return false;
}

static Grant *grant_find(const char *app_id, const char *perm) {
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (g_grants[i].used && strcmp(g_grants[i].app_id, app_id) == 0 &&
            strcmp(g_grants[i].perm, perm) == 0) {
            return &g_grants[i];
        }
    }
    return NULL;
}

static void grant_set(const char *app_id, const char *perm, bool granted) {
    Grant *g = grant_find(app_id, perm);
    if (!g) {
        for (int i = 0; i < MAX_GRANTS; i++) {
            if (!g_grants[i].used) {
                g = &g_grants[i];
                break;
            }
        }
    }
    if (!g) {
        return;
    }
    g->used = true;
    snprintf(g->app_id, sizeof(g->app_id), "%s", app_id);
    snprintf(g->perm, sizeof(g->perm), "%s", perm);
    g->granted = granted;
}

// Show the System-UI consent dialog and wait for the user's answer. The dialog is
// a separate overlay layer-shell helper; it exits 0 for Allow, 1 for Deny
// (anything else — e.g. exec failure — counts as Deny).
//
// The wait CANNOT be a plain waitpid: the dialog is itself a libzelto app, and
// every libzelto app makes a synchronous settings_get to this very daemon while
// starting up (Reduce Motion, app.c). Blocking here would leave the broker unable
// to answer it — the child waits on us, we wait on the child, and the permission
// prompt hangs forever, taking every other app's settings call down with it.
//
// So we keep SERVING while the dialog is up: poll the socket as usual and reap
// the child with WNOHANG. The daemon stays responsive (the dialog can start, the
// shade keeps updating), and the requesting client simply gets its reply later —
// it is parked on its own fd waiting, which is exactly what it expects.
static bool show_consent(const char *app_id, const char *perm) {
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // The desktop sim runs apps from the host build tree, not the image, so
        // the consent helper isn't at CONSENT_BIN there; ZELTO_CONSENT_BIN points
        // at the build-host binary (the ZELTO_RECENTS_BIN idiom the nav bar uses).
        const char *bin = getenv("ZELTO_CONSENT_BIN");
        if (!bin || !bin[0]) {
            bin = CONSENT_BIN;
        }
        execl(bin, "zelto-consent", app_id, perm, (char *)NULL);
        _exit(2);
    }

    // Re-entrancy guard: while this dialog is up, another app's perm_request (or
    // a notify_post, which is perm-gated too) must not fork a SECOND dialog on
    // top of it. Such a line is set aside and replayed once this one closes, so
    // the second app gets a real prompt rather than a spurious denial.
    g_in_consent = true;

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            break;
        }
        if (r < 0 && errno != EINTR) {
            status = 0;
            break;
        }
        serve_once(50);   // keep the broker alive for the dialog we just forked
    }

    g_in_consent = false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Resolve a request to a status word. For perm_request, this may prompt and
// record a new grant; for perm_status it only reports (declared -> "prompt").
static const char *decide(const char *op, const char *app_id,
                          const char *perm) {
    Grant *g = grant_find(app_id, perm);
    if (g) {
        return g->granted ? "granted" : "denied";   // cached fast path
    }
    if (!manifest_declares(app_id, perm)) {
        return "denied";   // undeclared: auto-deny, never prompt
    }
    if (strcmp(op, "perm_request") != 0) {
        return "prompt";   // perm_status: declared but no decision yet
    }
    bool allow = show_consent(app_id, perm);
    grant_set(app_id, perm, allow);
    return allow ? "granted" : "denied";
}

// --- app-to-app intents ----------------------------------------------------
// zsysd resolves a sender's z_share/z_open_url to a handler app from the
// manifests, shows the System-UI chooser when there's a choice, then delivers
// the payload to the target over its mailbox (launching it first if it isn't
// running). This is the second broker duty after permissions; see
// docs/platform/ipc-and-intents.md + docs/contributing/services-and-ipc.md.

static Manifest *manifest_by_id(const char *app_id) {
    for (int i = 0; i < g_n_manifests; i++) {
        if (strcmp(g_manifests[i].id, app_id) == 0) {
            return &g_manifests[i];
        }
    }
    return NULL;
}

// Does a single share-target pattern match a MIME type? Exact, or a "type/*"
// glob (e.g. "text/*" matches "text/plain").
static bool mime_glob_match(const char *pattern, const char *mime) {
    size_t pl = strlen(pattern);
    if (pl >= 2 && pattern[pl - 1] == '*' && pattern[pl - 2] == '/') {
        return strncmp(pattern, mime, pl - 1) == 0;   // compare "text/"
    }
    return strcmp(pattern, mime) == 0;
}

// Does this app accept `mime` (any entry in its share_targets CSV matches)?
static bool share_target_matches(const Manifest *m, const char *mime) {
    const char *p = m->share_targets;
    while (*p) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = (size_t)(p - start);
        while (len > 0 && start[len - 1] == ' ') {
            len--;
        }
        if (len > 0) {
            char pat[64];
            if (len >= sizeof(pat)) {
                len = sizeof(pat) - 1;
            }
            memcpy(pat, start, len);
            pat[len] = '\0';
            if (mime_glob_match(pat, mime)) {
                return true;
            }
        }
    }
    return false;
}

// Copy the scheme of a URL ("zelto://note/42" -> "zelto") into out.
static void scheme_of(const char *url, char *out, size_t n) {
    size_t i = 0;
    while (url[i] && url[i] != ':' && i + 1 < n) {
        out[i] = url[i];
        i++;
    }
    out[i] = '\0';
}

// Find the live mailbox fd registered for an app_id (-1 if not running).
static int mailbox_fd(const char *app_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_fd[i] >= 0 && strcmp(g_client_app[i], app_id) == 0) {
            return g_client_fd[i];
        }
    }
    return -1;
}

static void send_deliver_share(int fd, const char *mime, const char *payload) {
    char msg[512];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"deliver\",\"kind\":\"share\",\"mime\":\"%s\","
                     "\"payload\":\"%s\"}\n",
                     mime, payload);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
}

static void send_deliver_url(int fd, const char *url) {
    char msg[384];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"deliver\",\"kind\":\"open_url\",\"url\":\"%s\"}\n",
                     url);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
}

// Push a tapped notification action to the poster's mailbox; libzelto fires its
// z_on_notification_action. id travels as a quoted string (our json_get only
// parses quoted values).
static void send_deliver_action(int fd, int64_t id, const char *action) {
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"deliver\",\"kind\":\"notify_action\","
                     "\"id\":\"%lld\",\"action\":\"%s\"}\n",
                     (long long)id, action ? action : "");
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
}

// Launch an app by its manifest exec= (the same fork/setsid/exec path the
// launcher uses), so an intent can reach an app that isn't running yet.
static void launch_app(const char *app_id) {
    Manifest *m = manifest_by_id(app_id);
    if (!m || !m->exec[0]) {
        fprintf(stderr, "[zsysd] cannot launch %s: no exec= in manifest\n",
                app_id);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        char cmd[256];
        z_exec_cmd(m->exec, cmd, sizeof(cmd));
        _exit(127);
    }
}

// Queue an intent for an app that has no live mailbox yet; flushed on register.
static void enqueue_pending(const char *app_id, const char *kind,
                            const char *mime, const char *url,
                            const char *payload) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used) {
            g_pending[i].used = true;
            snprintf(g_pending[i].app_id, sizeof(g_pending[i].app_id), "%s",
                     app_id);
            snprintf(g_pending[i].kind, sizeof(g_pending[i].kind), "%s", kind);
            snprintf(g_pending[i].mime, sizeof(g_pending[i].mime), "%s",
                     mime ? mime : "");
            snprintf(g_pending[i].url, sizeof(g_pending[i].url), "%s",
                     url ? url : "");
            snprintf(g_pending[i].payload, sizeof(g_pending[i].payload), "%s",
                     payload ? payload : "");
            return;
        }
    }
    fprintf(stderr, "[zsysd] pending-intent queue full; dropping for %s\n",
            app_id);
}

// Flush every queued intent for an app that just registered its mailbox.
static void flush_pending(const char *app_id, int fd) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used || strcmp(g_pending[i].app_id, app_id) != 0) {
            continue;
        }
        if (strcmp(g_pending[i].kind, "open_url") == 0) {
            send_deliver_url(fd, g_pending[i].url);
        } else if (strcmp(g_pending[i].kind, "notify_action") == 0) {
            send_deliver_action(fd, g_pending[i].notif_id, g_pending[i].payload);
        } else {
            send_deliver_share(fd, g_pending[i].mime, g_pending[i].payload);
        }
        fprintf(stderr, "[zsysd] flushed pending %s -> %s\n", g_pending[i].kind,
                app_id);
        g_pending[i].used = false;
    }
}

// Deliver to a chosen target: push now if it has a mailbox, else launch it and
// queue the intent (flushed when it registers). Bringing it to the foreground
// is the compositor's job — a freshly mapped toplevel activates on map (P7).
static void deliver_or_queue(const char *app_id, const char *kind,
                             const char *mime, const char *url,
                             const char *payload) {
    int fd = mailbox_fd(app_id);
    if (fd >= 0) {
        if (strcmp(kind, "open_url") == 0) {
            send_deliver_url(fd, url);
        } else {
            send_deliver_share(fd, mime, payload);
        }
        fprintf(stderr, "[zsysd] delivered %s -> %s (running)\n", kind, app_id);
    } else {
        fprintf(stderr, "[zsysd] launching %s for %s intent\n", app_id, kind);
        launch_app(app_id);
        enqueue_pending(app_id, kind, mime, url, payload);
    }
}

// Show the System-UI chooser over the candidate app_ids and block until the
// user picks one. Mirrors show_consent's fork/exec + exit-code IPC: the chooser
// exits with the 1-based index of the pick (0 = cancel / exec failure).
//
// `mime` / `payload` describe WHAT is being shared, so the sheet can preview it
// (both may be ""). They go through the ENVIRONMENT of the forked child, not
// argv: argv is the candidate list and its indices ARE the IPC — the exit code
// means "argv[N]" — so prepending anything to it would introduce an offset both
// sides must agree on, and a disagreement delivers the share to the wrong app.
static int run_chooser(char ids[][96], int n, const char *mime,
                       const char *payload) {
    char *argv[2 + MAX_MANIFESTS];
    argv[0] = (char *)"zelto-chooser";
    for (int i = 0; i < n; i++) {
        argv[1 + i] = ids[i];
    }
    argv[1 + n] = NULL;
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        setenv("ZELTO_SHARE_MIME", mime ? mime : "", 1);
        setenv("ZELTO_SHARE_PAYLOAD", payload ? payload : "", 1);
        execv(CHOOSER_BIN, argv);
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}

// Resolve a sender's intent_resolve request: match candidate handlers, run the
// chooser if there's a choice, and deliver the payload to the pick.
static void handle_intent_resolve(const char *line) {
    char action[16] = {0};
    json_get(line, "action", action, sizeof(action));

    char cands[MAX_MANIFESTS][96];
    int n = 0;

    if (strcmp(action, "share") == 0) {
        char mime[64] = {0}, payload[256] = {0};
        json_get(line, "mime", mime, sizeof(mime));
        json_get(line, "payload", payload, sizeof(payload));
        for (int i = 0; i < g_n_manifests && n < MAX_MANIFESTS; i++) {
            if (share_target_matches(&g_manifests[i], mime)) {
                snprintf(cands[n++], 96, "%s", g_manifests[i].id);
            }
        }
        fprintf(stderr, "[zsysd] intent_resolve share mime=%s -> %d candidate(s)\n",
                mime, n);
        if (n == 0) {
            return;
        }
        // Share always shows the sheet (even a single candidate) — that chooser
        // is the user's confirmation of where the content goes.
        int pick = run_chooser(cands, n, mime, payload);
        if (pick < 1 || pick > n) {
            fprintf(stderr, "[zsysd] share cancelled (pick=%d)\n", pick);
            return;
        }
        deliver_or_queue(cands[pick - 1], "share", mime, "", payload);
    } else if (strcmp(action, "open_url") == 0) {
        char url[256] = {0}, scheme[32] = {0};
        json_get(line, "url", url, sizeof(url));
        scheme_of(url, scheme, sizeof(scheme));
        for (int i = 0; i < g_n_manifests && n < MAX_MANIFESTS; i++) {
            if (csv_contains(g_manifests[i].links, scheme)) {
                snprintf(cands[n++], 96, "%s", g_manifests[i].id);
            }
        }
        fprintf(stderr,
                "[zsysd] intent_resolve open_url scheme=%s -> %d candidate(s)\n",
                scheme, n);
        if (n == 0) {
            return;
        }
        // A single handler skips the chooser (resolve directly by scheme).
        int idx = 1;
        if (n > 1) {
            // A deep link has no payload to preview — the URL is the subject.
            idx = run_chooser(cands, n, "", url);
            if (idx < 1 || idx > n) {
                fprintf(stderr, "[zsysd] open_url cancelled (pick=%d)\n", idx);
                return;
            }
        }
        deliver_or_queue(cands[idx - 1], "open_url", "", url, "");
    }
}

// --- notifications ---------------------------------------------------------

// Push a stored notification to the shade as a notify_show (id as a quoted
// string). Sent only when a sink is subscribed.
static void send_notify_show(int fd, const Notification *n) {
    char msg[1024];
    int m = snprintf(
        msg, sizeof(msg),
        "{\"op\":\"notify_show\",\"id\":\"%lld\",\"app_id\":\"%s\","
        "\"title\":\"%s\",\"body\":\"%s\",\"tap_route\":\"%s\","
        "\"action_id\":\"%s\",\"action_title\":\"%s\",\"image\":\"%s\"}\n",
        (long long)n->id, n->app_id, n->title, n->body, n->tap_route,
        n->action_id, n->action_title, n->image);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
}

static void send_notify_hide(int fd, int64_t id) {
    char msg[64];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"notify_hide\",\"id\":\"%lld\"}\n",
                     (long long)id);
    if (m > 0 && m < (int)sizeof(msg)) {
        ssize_t w = write(fd, msg, (size_t)m);
        (void)w;
    }
}

static Notification *notif_find(int64_t id) {
    for (int i = 0; i < MAX_NOTIFS; i++) {
        if (g_notifs[i].used && g_notifs[i].id == id) {
            return &g_notifs[i];
        }
    }
    return NULL;
}

// Remove a notification from the store and hide it on every sink.
static void notif_drop(int64_t id) {
    Notification *n = notif_find(id);
    if (!n) {
        return;
    }
    n->used = false;
    for (int i = 0; i < g_n_sinks; i++) {
        send_notify_hide(g_sink_fds[i], id);
    }
    publish_notif_count();
}

// notify_post: perm-gate (the consent prompt blocks here exactly like a
// perm_request — the posting app waits synchronously on its transient conn),
// assign a global id, store it, reply {"id":"N"} on this connection, and push a
// notify_show to every sink. A denial replies {"id":"-1"}.
static void handle_notify_post(int fd, const char *line) {
    char app_id[96] = {0}, title[128] = {0}, body[192] = {0}, channel[64] = {0},
         tap_route[256] = {0}, action_id[64] = {0}, action_title[64] = {0},
         image[256] = {0};
    json_get(line, "app_id", app_id, sizeof(app_id));
    json_get(line, "title", title, sizeof(title));
    json_get(line, "body", body, sizeof(body));
    json_get(line, "channel", channel, sizeof(channel));
    json_get(line, "tap_route", tap_route, sizeof(tap_route));
    json_get(line, "action_id", action_id, sizeof(action_id));
    json_get(line, "action_title", action_title, sizeof(action_title));
    json_get(line, "image", image, sizeof(image));

    const char *st = app_id[0] ? decide("perm_request", app_id, "notifications")
                               : "denied";
    if (strcmp(st, "granted") != 0) {
        fprintf(stderr, "[zsysd] notify_post app=%s -> %s (no banner)\n",
                app_id[0] ? app_id : "?", st);
        const char *deny = "{\"id\":\"-1\"}\n";
        ssize_t w = write(fd, deny, strlen(deny));
        (void)w;
        return;
    }

    Notification *n = NULL;
    for (int i = 0; i < MAX_NOTIFS; i++) {
        if (!g_notifs[i].used) {
            n = &g_notifs[i];
            break;
        }
    }
    if (!n) {
        fprintf(stderr, "[zsysd] notify store full; dropping post from %s\n",
                app_id);
        const char *deny = "{\"id\":\"-1\"}\n";
        ssize_t w = write(fd, deny, strlen(deny));
        (void)w;
        return;
    }
    n->used = true;
    n->id = g_next_notif_id++;
    snprintf(n->app_id, sizeof(n->app_id), "%s", app_id);
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->body, sizeof(n->body), "%s", body);
    snprintf(n->channel, sizeof(n->channel), "%s", channel);
    snprintf(n->tap_route, sizeof(n->tap_route), "%s", tap_route);
    snprintf(n->action_id, sizeof(n->action_id), "%s", action_id);
    snprintf(n->action_title, sizeof(n->action_title), "%s", action_title);
    snprintf(n->image, sizeof(n->image), "%s", image);

    char reply[64];
    int m = snprintf(reply, sizeof(reply), "{\"id\":\"%lld\"}\n",
                     (long long)n->id);
    if (m > 0) {
        ssize_t w = write(fd, reply, (size_t)m);
        (void)w;
    }
    fprintf(stderr, "[zsysd] notify_post app=%s id=%lld -> %d sink(s)\n", app_id,
            (long long)n->id, g_n_sinks);
    for (int i = 0; i < g_n_sinks; i++) {
        send_notify_show(g_sink_fds[i], n);
    }
    publish_notif_count();
}

// An action tapped on the shade: route it to the poster's mailbox (launch-if-
// needed + queue, the same machinery deep links use), then drop the banner.
static void route_notify_action(int64_t id, const char *action) {
    Notification *n = notif_find(id);
    if (!n) {
        return;
    }
    char app_id[96];
    snprintf(app_id, sizeof(app_id), "%s", n->app_id);
    int mfd = mailbox_fd(app_id);
    if (mfd >= 0) {
        send_deliver_action(mfd, id, action);
        fprintf(stderr, "[zsysd] notify_action id=%lld -> %s (running)\n",
                (long long)id, app_id);
    } else {
        fprintf(stderr, "[zsysd] launching %s for notify_action\n", app_id);
        launch_app(app_id);
        for (int i = 0; i < MAX_PENDING; i++) {
            if (!g_pending[i].used) {
                g_pending[i].used = true;
                snprintf(g_pending[i].app_id, sizeof(g_pending[i].app_id), "%s",
                         app_id);
                snprintf(g_pending[i].kind, sizeof(g_pending[i].kind),
                         "notify_action");
                snprintf(g_pending[i].payload, sizeof(g_pending[i].payload),
                         "%s", action ? action : "");
                g_pending[i].notif_id = id;
                break;
            }
        }
    }
    notif_drop(id);
}

// --- settings ---------------------------------------------------------------

// Build the persistence path ($ZELTO_DATA_DIR/settings.conf, falling back to
// /var/zelto like init's mountpoint) into `out`.
static void settings_path(char *out, size_t n) {
    const char *data = getenv("ZELTO_DATA_DIR");
    if (!data || !data[0]) {
        data = "/var/zelto";
    }
    snprintf(out, n, "%s/settings.conf", data);
}

static Setting *setting_find(const char *key) {
    for (int i = 0; i < MAX_SETTINGS; i++) {
        if (g_settings[i].used && strcmp(g_settings[i].key, key) == 0) {
            return &g_settings[i];
        }
    }
    return NULL;
}

// The stored value for a key, or "" when unset (the client supplies the default).
static const char *setting_value(const char *key) {
    Setting *s = setting_find(key);
    return s ? s->value : "";
}

// Update the in-memory table (insert or overwrite). Returns false if the table
// is full and the key is new.
static bool setting_put(const char *key, const char *value) {
    Setting *s = setting_find(key);
    if (!s) {
        for (int i = 0; i < MAX_SETTINGS; i++) {
            if (!g_settings[i].used) {
                s = &g_settings[i];
                break;
            }
        }
    }
    if (!s) {
        return false;
    }
    s->used = true;
    snprintf(s->key, sizeof(s->key), "%s", key);
    snprintf(s->value, sizeof(s->value), "%s", value);
    return true;
}

// Read the persisted settings file into the table at startup (best-effort: a
// missing file just means an empty table — clients fall back to their defaults).
static void settings_load(void) {
    char path[300];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        char *tab = strchr(line, '\t');
        if (!tab) {
            continue;
        }
        *tab = '\0';
        if (line[0]) {
            setting_put(line, tab + 1);
        }
    }
    fclose(f);
    fprintf(stderr, "[zsysd] settings loaded from %s\n", path);
}

// Rewrite the whole settings file (TAB-separated key\tvalue) and fsync so the
// change reaches the host image (the guest flush -> virtio-blk chain) and
// survives a reboot — settings are durable, unlike the notification store.
static void settings_persist(void) {
    char path[300];
    settings_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[zsysd] settings: cannot write %s\n", path);
        return;
    }
    for (int i = 0; i < MAX_SETTINGS; i++) {
        if (g_settings[i].used) {
            fprintf(f, "%s\t%s\n", g_settings[i].key, g_settings[i].value);
        }
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

// Register a ctrl fd as a settings observer (deduped; a SET so the shade and the
// Settings app can both be subscribed at once).
static void settings_subscribe_fd(int fd) {
    for (int i = 0; i < g_n_settings_subs; i++) {
        if (g_settings_subs[i] == fd) {
            return;
        }
    }
    if (g_n_settings_subs < MAX_CLIENTS) {
        g_settings_subs[g_n_settings_subs++] = fd;
    }
}

// Drop a ctrl fd from the observer set (on its disconnect).
static void settings_unsubscribe_fd(int fd) {
    for (int i = 0; i < g_n_settings_subs; i++) {
        if (g_settings_subs[i] == fd) {
            g_settings_subs[i] = g_settings_subs[--g_n_settings_subs];
            return;
        }
    }
}

// Push a settings_changed to every subscriber (fan-out; the setter is included).
static void settings_broadcast(const char *key, const char *value) {
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "{\"op\":\"settings_changed\",\"key\":\"%s\","
                     "\"value\":\"%s\"}\n",
                     key, value);
    if (m <= 0 || m >= (int)sizeof(msg)) {
        return;
    }
    for (int i = 0; i < g_n_settings_subs; i++) {
        ssize_t w = write(g_settings_subs[i], msg, (size_t)m);
        (void)w;
    }
}

// Set a key, persist + broadcast — but only when the value actually changed
// (idempotent: an unchanged write is a no-op, no disk churn, no fan-out). Both
// the settings_set op and the battery tick funnel through here. Returns whether
// anything changed.
static bool settings_apply(const char *key, const char *value) {
    Setting *cur = setting_find(key);
    if (cur && strcmp(cur->value, value) == 0) {
        return false;
    }
    if (!setting_put(key, value)) {
        return false;
    }
    settings_persist();
    settings_broadcast(key, value);
    return true;
}

// Publish the number of live (stored) notifications as the brokered setting
// sys.notif_count, so the home-screen notifications widget reads it through the
// settings fan-out it already observes. The launcher can't become a second
// notify sink (the sink is last-subscriber-wins — it would steal the shade's),
// so routing the count through the settings store reuses one observer instead of
// a new protocol. Called on every store change (post / drop / system post); the
// store is in-memory (lost on reboot), so main() republishes 0 at startup to
// clear any count persisted on a prior boot. settings_apply is idempotent, so an
// unchanged count is a no-op (no disk churn, no fan-out).
static void publish_notif_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_NOTIFS; i++) {
        if (g_notifs[i].used) {
            n++;
        }
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", n);
    settings_apply("sys.notif_count", buf);
}

// --- power source (battery) ------------------------------------------------

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Best-effort real battery read: first /sys/class/power_supply/* entry exposing
// a `capacity` file. Returns false when no battery exists (the common case on
// qemu-virt / a headless host), so the caller leaves the keys untouched.
static bool read_sysfs_battery(int *pct_out, bool *charging_out) {
    const char *base = "/sys/class/power_supply";
    DIR *d = opendir(base);
    if (!d) {
        return false;
    }
    bool found = false;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "%s/%s/capacity", base, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        int pct = -1;
        if (fscanf(f, "%d", &pct) == 1 && pct >= 0) {
            *pct_out = pct > 100 ? 100 : pct;
            *charging_out = false;
            snprintf(path, sizeof(path), "%s/%s/status", base, de->d_name);
            FILE *sf = fopen(path, "r");
            if (sf) {
                char st[32] = {0};
                if (fgets(st, sizeof(st), sf)) {
                    *charging_out = strncmp(st, "Charging", 8) == 0 ||
                                    strncmp(st, "Full", 4) == 0;
                }
                fclose(sf);
            }
            found = true;
        }
        fclose(f);
        if (found) {
            break;
        }
    }
    closedir(d);
    return found;
}

// Store + push a notification straight from the OS (no app, no perm gate — the
// system is the poster). Reuses the notify store + sinks. Used for the
// low-battery warning.
static void post_system_notification(const char *title, const char *body) {
    Notification *n = NULL;
    for (int i = 0; i < MAX_NOTIFS; i++) {
        if (!g_notifs[i].used) {
            n = &g_notifs[i];
            break;
        }
    }
    if (!n) {
        return;
    }
    n->used = true;
    n->id = g_next_notif_id++;
    // Clear every field this poster does not set. The slot is REUSED from a
    // fixed pool, so anything left over is the previous occupant's — a system
    // notification inheriting a stale image path would show a picture from an
    // unrelated event, which is worse than showing none.
    n->image[0] = '\0';
    snprintf(n->app_id, sizeof(n->app_id), "os.zelto.system");
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->body, sizeof(n->body), "%s", body);
    n->channel[0] = n->tap_route[0] = '\0';
    n->action_id[0] = n->action_title[0] = '\0';
    fprintf(stderr, "[zsysd] system notification id=%lld: %s\n",
            (long long)n->id, title);
    for (int i = 0; i < g_n_sinks; i++) {
        send_notify_show(g_sink_fds[i], n);
    }
    publish_notif_count();
}

// Post the low-battery warning once when the level crosses at/below the
// threshold on battery power, and nudge brightness down to conserve. The latch
// resets once charging or back above the threshold, so a later dip warns again.
static void check_low_battery(int pct, bool charging) {
    if (!charging && pct > 0 && pct <= LOW_BATT_THRESHOLD) {
        if (!g_low_warned) {
            g_low_warned = true;
            char body[96];
            snprintf(body, sizeof(body),
                     "Battery at %d%%. Plug in soon.", pct);
            post_system_notification("Battery low", body);
            // Only dim if brightness is set and comfortably high (don't surprise
            // a user who left it low, and skip when unset — client default 3).
            if (atoi(setting_value("sys.brightness")) >= 3) {
                settings_apply("sys.brightness", "2");
            }
        }
    } else {
        g_low_warned = false;
    }
}

// One battery tick: advance the fake drain (or read the real source), publish
// the two keys through settings_apply (fan-out to the bar), and check the low
// threshold. Fake: drain 1%/tick on battery, charge 2%/tick when charging.
static void battery_tick(void) {
    int pct = atoi(setting_value("sys.battery_pct"));
    bool charging = atoi(setting_value("sys.battery_charging")) != 0;
    if (g_fake_battery) {
        if (charging) {
            pct += 2;
            if (pct > 100) {
                pct = 100;
            }
        } else {
            pct -= 1;
            if (pct < 0) {
                pct = 0;
            }
        }
    } else {
        int rp;
        bool rc;
        if (!read_sysfs_battery(&rp, &rc)) {
            return;   // no real source this tick; leave keys as-is
        }
        pct = rp;
        charging = rc;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", pct);
    settings_apply("sys.battery_pct", buf);
    settings_apply("sys.battery_charging", charging ? "1" : "0");
    // Logged (P45) because a power SOURCE is otherwise invisible: settings_apply
    // fans out internally and does not go through the RPC path that prints
    // settings_set, so the only evidence the battery service was running at all
    // was a glyph in the status bar. That is exactly what the VOLUME harness
    // tried to photograph, at coordinates, before it rotted.
    fprintf(stderr, "[zsysd] battery %d%% charging=%d\n", pct, charging ? 1 : 0);
    fflush(stderr);
    check_low_battery(pct, charging);
}

// Compute the poll() timeout (ms) so the loop wakes for the next battery tick;
// -1 (block forever) when no battery source is driven.
static int battery_poll_timeout(void) {
    if (!g_batt_active) {
        return -1;
    }
    int64_t rem = g_next_batt_ms - now_ms();
    return rem < 0 ? 0 : (int)rem;
}

// Decide whether a battery source is driven and seed the keys. Fake source is
// gated by $ZELTO_FAKE_BATTERY (set in the sim/harness); otherwise a real sysfs
// battery, if present, backs the same keys. Seeds only unset keys so a two-boot
// resumes the persisted level rather than jumping back to full.
static void battery_init(void) {
    const char *fake = getenv("ZELTO_FAKE_BATTERY");
    g_fake_battery = fake && fake[0] && strcmp(fake, "0") != 0;
    const char *tk = getenv("ZELTO_BATTERY_TICK_MS");
    if (tk && atoi(tk) > 0) {
        g_batt_tick_ms = atoi(tk);
    }
    int rp;
    bool rc;
    bool real = read_sysfs_battery(&rp, &rc);
    g_batt_active = g_fake_battery || real;
    if (g_fake_battery) {
        if (!setting_find("sys.battery_pct")) {
            const char *start = getenv("ZELTO_BATTERY_START");
            settings_apply("sys.battery_pct", start && start[0] ? start : "100");
        }
        if (!setting_find("sys.battery_charging")) {
            settings_apply("sys.battery_charging", "0");
        }
    } else if (real) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", rp);
        settings_apply("sys.battery_pct", buf);
        settings_apply("sys.battery_charging", rc ? "1" : "0");
    }
    g_next_batt_ms = now_ms() + g_batt_tick_ms;
    fprintf(stderr, "[zsysd] battery source: %s (tick %dms, active=%d)\n",
            g_fake_battery ? "fake-drain" : (real ? "sysfs" : "none"),
            g_batt_tick_ms, g_batt_active);
}

// --- sensor + location source (zsysd's 6th duty, P38) ----------------------
// The device sensors (accelerometer, gyroscope, magnetometer, ...) and the GPS
// are streamed to apps that subscribe over a normal client connection: each
// subscription is keyed by that connection's fd, carries a sensor type and a
// refresh rate (clamped to [1,60] Hz — 60 is display-aligned, and the sensible
// cap for a software-rendered sim), and a per-subscription deadline. sensor_tick
// pushes one sensor_sample / location_update line per subscription when its
// deadline passes, exactly like the battery tick funnels through settings_apply,
// but addressed to a single subscriber fd rather than fanned out.
//
// The values are SIM-scriptable: a real device port fills them from a HAL, but
// here they come from ZELTO_SIM_* env (e.g. ZELTO_SIM_LOCATION="52.52,13.40",
// ZELTO_SIM_ORIENTATION="az,pitch,roll"), so the harness can `set location`
// deterministically. Streaming a sensor is gated by the `sensors` grant and
// location by the `location` grant — the same cached decisions the permission
// broker already owns (decide()/grant_find), so no new consent path.
//
// All numeric fields travel as QUOTED strings ("v0":"0.010000"), because the
// hand-rolled json_get on both ends only parses quoted string values; the client
// re-parses them with strtod/atoi (the same way ids travel as quoted strings).
#define MAX_SENSOR_SUBS 32
#define SENSOR_RATE_MIN 1
#define SENSOR_RATE_MAX 60

typedef struct SensorSub {
    bool used;
    int fd;                // subscriber connection
    char app_id[96];
    int kind;              // 0 = sensor, 1 = location
    char type[24];         // sensor wire name (kind 0 only)
    int rate_hz;           // clamped [1,60]
    int64_t next_ms;       // monotonic ms of this subscription's next sample
} SensorSub;
static SensorSub g_sensor_subs[MAX_SENSOR_SUBS];

static float env_f(const char *name, float dflt) {
    const char *s = getenv(name);
    return (s && s[0]) ? (float)atof(s) : dflt;
}

// Parse a "x,y,z" env (or its default) into three floats (missing -> 0).
static void env_vec3(const char *name, const char *dflt, float out[3]) {
    const char *s = getenv(name);
    if (!s || !s[0]) {
        s = dflt;
    }
    out[0] = out[1] = out[2] = 0.0f;
    sscanf(s, "%f,%f,%f", &out[0], &out[1], &out[2]);
}

// Synthesize one sensor reading for `type` at time `t` (ms). Writes up to three
// values into v[] and returns the value count (0 = unknown/absent sensor). The
// values are static per env (deterministic for screenshots); an app proves a
// stream's rate by counting samples over time, not by watching a value wobble.
static int sensor_synth(const char *type, int64_t t, float v[3], int *accuracy) {
    *accuracy = 3;   // SENSOR_STATUS_ACCURACY_HIGH
    v[0] = v[1] = v[2] = 0.0f;
    if (strcmp(type, "accelerometer") == 0 || strcmp(type, "gravity") == 0) {
        env_vec3("ZELTO_SIM_ACCEL", "0,0,9.81", v);
        return 3;
    }
    if (strcmp(type, "linear_acceleration") == 0) {
        return 3;   // at rest: no linear acceleration
    }
    if (strcmp(type, "gyroscope") == 0) {
        env_vec3("ZELTO_SIM_GYRO", "0,0,0", v);
        return 3;
    }
    if (strcmp(type, "magnetometer") == 0) {
        env_vec3("ZELTO_SIM_MAG", "0,-30,-40", v);
        return 3;
    }
    if (strcmp(type, "orientation") == 0 || strcmp(type, "rotation_vector") == 0) {
        env_vec3("ZELTO_SIM_ORIENTATION", "0,0,0", v);
        return 3;
    }
    if (strcmp(type, "light") == 0) {
        v[0] = env_f("ZELTO_SIM_LIGHT", 300.0f);
        return 1;
    }
    if (strcmp(type, "proximity") == 0) {
        v[0] = env_f("ZELTO_SIM_PROXIMITY", 5.0f);
        return 1;
    }
    if (strcmp(type, "pressure") == 0) {
        v[0] = env_f("ZELTO_SIM_PRESSURE", 1013.25f);
        return 1;
    }
    if (strcmp(type, "step_counter") == 0) {
        v[0] = env_f("ZELTO_SIM_STEPS", 0.0f) + (float)(t / 1000);   // ~1 step/s
        return 1;
    }
    return 0;   // unknown sensor
}

// Fill a location reading from ZELTO_SIM_LOCATION (+ optional altitude/speed/
// bearing). Returns false if no simulated location is configured.
static bool location_synth(double *lat, double *lng, float *acc, float *alt,
                           float *speed, float *bearing) {
    const char *s = getenv("ZELTO_SIM_LOCATION");
    if (!s || !s[0]) {
        s = "52.5200,13.4050";   // a default fix so location demos have data
    }
    double la = 0, ln = 0;
    if (sscanf(s, "%lf,%lf", &la, &ln) < 2) {
        return false;
    }
    *lat = la;
    *lng = ln;
    *acc = env_f("ZELTO_SIM_LOC_ACCURACY", 12.0f);
    *alt = env_f("ZELTO_SIM_ALTITUDE", 34.0f);
    *speed = env_f("ZELTO_SIM_SPEED", 0.0f);
    *bearing = env_f("ZELTO_SIM_BEARING", 0.0f);
    return true;
}

static int clamp_rate(int hz) {
    if (hz < SENSOR_RATE_MIN) {
        return SENSOR_RATE_MIN;
    }
    if (hz > SENSOR_RATE_MAX) {
        return SENSOR_RATE_MAX;
    }
    return hz;
}

// Register (or re-arm) a subscription on this fd. kind 0 keys on fd+type; kind 1
// (location) keys on fd. Re-subscribing updates the rate in place.
static void sensor_sub_add(int fd, const char *app_id, int kind,
                           const char *type, int rate_hz) {
    SensorSub *slot = NULL;
    for (int i = 0; i < MAX_SENSOR_SUBS; i++) {
        SensorSub *s = &g_sensor_subs[i];
        if (s->used && s->fd == fd && s->kind == kind &&
            (kind != 0 || strcmp(s->type, type) == 0)) {
            slot = s;
            break;
        }
        if (!slot && !s->used) {
            slot = s;   // remember a free slot but keep scanning for a match
        }
    }
    if (!slot) {
        return;   // table full
    }
    slot->used = true;
    slot->fd = fd;
    slot->kind = kind;
    snprintf(slot->app_id, sizeof(slot->app_id), "%s", app_id ? app_id : "");
    if (kind == 0) {
        snprintf(slot->type, sizeof(slot->type), "%s", type ? type : "");
    } else {
        slot->type[0] = '\0';
    }
    slot->rate_hz = clamp_rate(rate_hz);
    slot->next_ms = now_ms();   // deliver the first sample promptly
}

// Drop a subscription. type==NULL removes every sub of `kind` on this fd.
static void sensor_sub_remove(int fd, int kind, const char *type) {
    for (int i = 0; i < MAX_SENSOR_SUBS; i++) {
        SensorSub *s = &g_sensor_subs[i];
        if (s->used && s->fd == fd && s->kind == kind &&
            (kind != 0 || !type || strcmp(s->type, type) == 0)) {
            s->used = false;
        }
    }
}

// Drop every subscription on a fd (called when the connection closes).
static void sensor_unsubscribe_fd(int fd) {
    for (int i = 0; i < MAX_SENSOR_SUBS; i++) {
        if (g_sensor_subs[i].used && g_sensor_subs[i].fd == fd) {
            g_sensor_subs[i].used = false;
        }
    }
}

// Push a sensor sample / location update to each subscription whose deadline has
// passed, then advance that subscription's deadline by its period. A subscription
// whose grant was revoked (or never held) is skipped silently.
static void sensor_tick(void) {
    int64_t t = now_ms();
    for (int i = 0; i < MAX_SENSOR_SUBS; i++) {
        SensorSub *s = &g_sensor_subs[i];
        if (!s->used || t < s->next_ms) {
            continue;
        }
        int period = 1000 / (s->rate_hz > 0 ? s->rate_hz : 1);
        s->next_ms = t + (period > 0 ? period : 1);

        const char *perm = (s->kind == 1) ? "location" : "sensors";
        Grant *g = grant_find(s->app_id, perm);
        if (!g || !g->granted) {
            continue;   // not (or no longer) permitted: stream nothing
        }

        char msg[320];
        int m;
        if (s->kind == 1) {
            double lat = 0, lng = 0;
            float acc = 0, alt = 0, spd = 0, brg = 0;
            if (!location_synth(&lat, &lng, &acc, &alt, &spd, &brg)) {
                continue;
            }
            m = snprintf(msg, sizeof(msg),
                         "{\"op\":\"location_update\",\"lat\":\"%.6f\","
                         "\"lng\":\"%.6f\",\"accuracy\":\"%.1f\","
                         "\"altitude\":\"%.1f\",\"speed\":\"%.2f\","
                         "\"bearing\":\"%.1f\",\"t\":\"%lld\"}\n",
                         lat, lng, acc, alt, spd, brg, (long long)t);
        } else {
            float v[3];
            int acc = 3;
            int n = sensor_synth(s->type, t, v, &acc);
            if (n == 0) {
                continue;
            }
            m = snprintf(msg, sizeof(msg),
                         "{\"op\":\"sensor_sample\",\"type\":\"%s\",\"t\":\"%lld\","
                         "\"n\":\"%d\",\"accuracy\":\"%d\",\"v0\":\"%.6f\","
                         "\"v1\":\"%.6f\",\"v2\":\"%.6f\"}\n",
                         s->type, (long long)t, n, acc, v[0], v[1], v[2]);
        }
        if (m > 0 && m < (int)sizeof(msg)) {
            ssize_t w = write(s->fd, msg, (size_t)m);
            (void)w;
        }
    }
}

// ms until the soonest subscription deadline; -1 (block) when there are none.
static int sensor_poll_timeout(void) {
    int64_t soonest = -1;
    for (int i = 0; i < MAX_SENSOR_SUBS; i++) {
        if (g_sensor_subs[i].used &&
            (soonest < 0 || g_sensor_subs[i].next_ms < soonest)) {
            soonest = g_sensor_subs[i].next_ms;
        }
    }
    if (soonest < 0) {
        return -1;
    }
    int64_t rem = soonest - now_ms();
    return rem < 0 ? 0 : (int)rem;
}

// Answer a synchronous one-shot location_get on the requesting connection. Gated
// by the `location` grant; a denial (or no fix) replies {"ok":"0"}.
static void handle_location_get(int fd, const char *line) {
    char app_id[96] = {0};
    json_get(line, "app_id", app_id, sizeof(app_id));
    Grant *g = grant_find(app_id, "location");
    double lat = 0, lng = 0;
    float acc = 0, alt = 0, spd = 0, brg = 0;
    char reply[320];
    int m;
    if (g && g->granted &&
        location_synth(&lat, &lng, &acc, &alt, &spd, &brg)) {
        m = snprintf(reply, sizeof(reply),
                     "{\"ok\":\"1\",\"lat\":\"%.6f\",\"lng\":\"%.6f\","
                     "\"accuracy\":\"%.1f\",\"altitude\":\"%.1f\","
                     "\"speed\":\"%.2f\",\"bearing\":\"%.1f\",\"t\":\"%lld\"}\n",
                     lat, lng, acc, alt, spd, brg, (long long)now_ms());
    } else {
        m = snprintf(reply, sizeof(reply), "{\"ok\":\"0\"}\n");
    }
    if (m > 0 && m < (int)sizeof(reply)) {
        ssize_t w = write(fd, reply, (size_t)m);
        (void)w;
    }
}

// Process one request line. Perm ops reply on the same connection; register and
// intent_resolve come over a persistent control connection (no reply). `slot`
// is the client's table index, so a register can record its mailbox app_id.
static void handle_line(int slot, int fd, char *line) {
    char op[32] = {0};
    json_get(line, "op", op, sizeof(op));

    // A consent dialog is already on screen: the two ops that can raise one must
    // wait their turn rather than stack a second dialog over it. Set the line
    // aside — the client stays parked on its fd, exactly as it would be if we
    // were simply slow — and replay it when the current dialog closes. Every
    // other op (settings_get above all, which the dialog itself is blocked on)
    // falls through and is served normally: that is the point of serving while a
    // prompt is up.
    if (g_in_consent &&
        (strcmp(op, "perm_request") == 0 || strcmp(op, "notify_post") == 0)) {
        if (g_n_deferred < MAX_DEFERRED) {
            g_deferred[g_n_deferred].slot = slot;
            snprintf(g_deferred[g_n_deferred].line,
                     sizeof(g_deferred[g_n_deferred].line), "%s", line);
            g_n_deferred++;
        }
        return;
    }

    // Persistent intents control connection: register this app's mailbox, or
    // resolve one of its outgoing intents.
    if (strcmp(op, "register") == 0) {
        char app_id[96] = {0};
        json_get(line, "app_id", app_id, sizeof(app_id));
        if (slot >= 0 && slot < MAX_CLIENTS) {
            snprintf(g_client_app[slot], sizeof(g_client_app[slot]), "%s",
                     app_id);
        }
        fprintf(stderr, "[zsysd] register app=%s (slot %d)\n", app_id, slot);
        flush_pending(app_id, fd);
        return;
    }
    if (strcmp(op, "intent_resolve") == 0) {
        handle_intent_resolve(line);
        return;
    }
    // zelto-install sends this after registering a runtime-installed package so
    // the broker picks up the new manifest (permissions / intent handlers / exec)
    // without a reboot (P13). Re-scans both manifest dirs.
    if (strcmp(op, "reload") == 0) {
        load_manifests();
        fprintf(stderr, "[zsysd] reloaded manifests (%d total)\n", g_n_manifests);
        return;
    }

    // --- notifications ---
    // A surface subscribes as a sink (the shade and the lock screen both do).
    // Records its ctrl fd in the sink SET — not last-wins, see the store's notes.
    if (strcmp(op, "notify_subscribe") == 0) {
        sink_subscribe_fd(fd);
        fprintf(stderr, "[zsysd] notify sink subscribed (slot %d, %d total)\n",
                slot, g_n_sinks);
        return;
    }
    // notify_post is a synchronous round-trip (replies {"id":..} on this conn).
    if (strcmp(op, "notify_post") == 0) {
        handle_notify_post(fd, line);
        return;
    }
    if (strcmp(op, "notify_cancel") == 0) {
        char id[24] = {0};
        json_get(line, "id", id, sizeof(id));
        notif_drop((int64_t)atoll(id));
        return;
    }
    // From the shade: body tap (drop only — the shade routed the deep link
    // itself) or action tap (route to the poster, then drop).
    if (strcmp(op, "notify_tap") == 0) {
        char id[24] = {0};
        json_get(line, "id", id, sizeof(id));
        notif_drop((int64_t)atoll(id));
        return;
    }
    if (strcmp(op, "notify_action") == 0) {
        char id[24] = {0}, action[64] = {0};
        json_get(line, "id", id, sizeof(id));
        json_get(line, "action", action, sizeof(action));
        route_notify_action((int64_t)atoll(id), action);
        return;
    }
    if (strcmp(op, "notify_channel") == 0) {
        char cid[64] = {0}, cname[64] = {0}, imp[8] = {0};
        json_get(line, "id", cid, sizeof(cid));
        json_get(line, "name", cname, sizeof(cname));
        json_get(line, "importance", imp, sizeof(imp));
        fprintf(stderr, "[zsysd] notify_channel id=%s name=%s importance=%s\n",
                cid, cname, imp);
        return;
    }
    // --- settings ---
    // settings_get is a synchronous fast read (in-memory; replies on this conn).
    if (strcmp(op, "settings_get") == 0) {
        char key[64] = {0};
        json_get(line, "key", key, sizeof(key));
        char reply[256];
        int m = snprintf(reply, sizeof(reply), "{\"value\":\"%s\"}\n",
                         key[0] ? setting_value(key) : "");
        if (m > 0 && m < (int)sizeof(reply)) {
            ssize_t w = write(fd, reply, (size_t)m);
            (void)w;
        }
        return;
    }
    // settings_set: persist (write-through to disk) + broadcast to subscribers.
    if (strcmp(op, "settings_set") == 0) {
        char key[64] = {0}, value[160] = {0};
        json_get(line, "key", key, sizeof(key));
        json_get(line, "value", value, sizeof(value));
        if (key[0] && settings_apply(key, value)) {
            fprintf(stderr, "[zsysd] settings_set %s=%s -> %d subscriber(s)\n",
                    key, value, g_n_settings_subs);
        }
        return;
    }
    // snapshot_policy: may the compositor photograph this app's window for the
    // App Switcher? Answers the manifest's `no_snapshot=1` (default: yes).
    //
    // WHY THIS LIVES HERE AND NOT IN ZCOMP. The flag is a manifest declaration,
    // and zsysd is the process that reads manifests — it already holds this exact
    // table for permissions, share targets and links, already merges the baked-in
    // dir with the runtime-installed one, and already rebuilds it on the
    // installer's {"op":"reload"}. Teaching zcomp to read manifests instead would
    // duplicate the parser, the two-directory merge and the reload signal inside
    // the compositor, and give the compositor a policy file to watch.
    //
    // WHY NOT A sys.* SETTINGS KEY. settings_set is ungated — any client can write
    // any key — so publishing the deny-list as a setting would let one app clear
    // another app's flag. A dedicated READ-ONLY op has no such write path.
    //
    // Answering per-app rather than shipping the whole list keeps the reply
    // bounded, and zcomp asks once per window (at map, where it first learns the
    // app_id) and caches the answer, so nothing queries on the capture path.
    if (strcmp(op, "snapshot_policy") == 0) {
        char sapp[96] = {0};
        json_get(line, "app_id", sapp, sizeof(sapp));
        bool allow = true;
        for (int i = 0; i < g_n_manifests; i++) {
            if (strcmp(g_manifests[i].id, sapp) == 0) {
                allow = !g_manifests[i].no_snapshot;
                break;
            }
        }
        char reply[64];
        int m = snprintf(reply, sizeof(reply), "{\"allow\":\"%d\"}\n", allow ? 1 : 0);
        if (m > 0 && m < (int)sizeof(reply)) {
            ssize_t w = write(fd, reply, (size_t)m);
            (void)w;
        }
        if (!allow) {
            fprintf(stderr, "[zsysd] snapshot_policy %s -> DENY (no_snapshot)\n",
                    sapp);
        }
        return;
    }

    // settings_subscribe: record this ctrl fd in the observer set (multiple).
    if (strcmp(op, "settings_subscribe") == 0) {
        settings_subscribe_fd(fd);
        fprintf(stderr, "[zsysd] settings observer subscribed (slot %d, %d total)\n",
                slot, g_n_settings_subs);
        return;
    }

    // --- sensors + location (P38) ---
    // A stream subscription keyed by this connection's fd; sensor_tick pushes
    // samples to it at the requested (clamped) rate. Fire-and-forget (no reply);
    // permission is checked at each tick against the cached grant.
    if (strcmp(op, "sensor_subscribe") == 0) {
        char sapp[96] = {0}, type[24] = {0}, rate[8] = {0};
        json_get(line, "app_id", sapp, sizeof(sapp));
        json_get(line, "type", type, sizeof(type));
        json_get(line, "rate", rate, sizeof(rate));
        if (type[0]) {
            sensor_sub_add(fd, sapp, 0, type, rate[0] ? atoi(rate) : 5);
            fprintf(stderr, "[zsysd] sensor_subscribe app=%s type=%s rate=%s\n",
                    sapp, type, rate[0] ? rate : "5");
        }
        return;
    }
    if (strcmp(op, "sensor_unsubscribe") == 0) {
        char type[24] = {0};
        json_get(line, "type", type, sizeof(type));
        sensor_sub_remove(fd, 0, type[0] ? type : NULL);
        // Logged symmetrically with the subscribe: an app backgrounding pauses its
        // streams (libzelto's when-in-use gate), so a stream that stops without the
        // app exiting is the expected battery/privacy behaviour, not a leak.
        fprintf(stderr, "[zsysd] sensor_unsubscribe type=%s\n",
                type[0] ? type : "*");
        return;
    }
    if (strcmp(op, "location_subscribe") == 0) {
        char sapp[96] = {0}, rate[8] = {0};
        json_get(line, "app_id", sapp, sizeof(sapp));
        json_get(line, "rate", rate, sizeof(rate));
        sensor_sub_add(fd, sapp, 1, NULL, rate[0] ? atoi(rate) : 1);
        fprintf(stderr, "[zsysd] location_subscribe app=%s\n", sapp);
        return;
    }
    if (strcmp(op, "location_unsubscribe") == 0) {
        sensor_sub_remove(fd, 1, NULL);
        fprintf(stderr, "[zsysd] location_unsubscribe\n");
        return;
    }
    // location_get is a synchronous one-shot (replies on this conn, like
    // settings_get), so an app can read a single fix without a stream.
    if (strcmp(op, "location_get") == 0) {
        handle_location_get(fd, line);
        return;
    }
    if (strcmp(op, "notify_badge") == 0) {
        char bapp[96] = {0}, count[16] = {0};
        json_get(line, "app_id", bapp, sizeof(bapp));
        json_get(line, "count", count, sizeof(count));
        // Forwarding the badge to the bar/shade is Planned; record it for now.
        fprintf(stderr, "[zsysd] notify_badge app=%s count=%s (forward Planned)\n",
                bapp, count);
        return;
    }

    // Permission request/status: reply with the decision on this connection.
    char app_id[96] = {0}, perm[32] = {0};
    json_get(line, "app_id", app_id, sizeof(app_id));
    json_get(line, "perm", perm, sizeof(perm));
    const char *st;
    if (!op[0] || !app_id[0] || !perm[0]) {
        st = "denied";
    } else {
        st = decide(op, app_id, perm);
    }
    fprintf(stderr, "[zsysd] %s app=%s perm=%s -> %s\n", op[0] ? op : "?",
            app_id[0] ? app_id : "?", perm[0] ? perm : "?", st);
    char reply[64];
    int m = snprintf(reply, sizeof(reply), "{\"status\":\"%s\"}\n", st);
    if (m > 0) {
        ssize_t w = write(fd, reply, (size_t)m);
        (void)w;
    }
}

// One turn of the daemon: wait up to `timeout_ms` for socket traffic, accept new
// connections, and handle whatever arrived. Factored out of main so show_consent
// can call it too — that is what keeps the broker answering while a consent
// dialog (itself a client of ours) is starting up.
static void serve_once(int timeout_ms) {
    struct pollfd pfds[1 + MAX_CLIENTS];
    int slot_of[1 + MAX_CLIENTS];
    pfds[0].fd = g_lfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    int nf = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_fd[i] >= 0) {
            pfds[nf].fd = g_client_fd[i];
            pfds[nf].events = POLLIN;
            pfds[nf].revents = 0;
            slot_of[nf] = i;
            nf++;
        }
    }

    if (poll(pfds, (nfds_t)nf, timeout_ms) < 0) {
        if (errno != EINTR) {
            perror("zsysd: poll");
        }
        return;
    }

    // Battery: advance the source whenever a tick is due (poll woke us at the
    // deadline even with no socket traffic). Cheap in-memory + one settings
    // fan-out on an actual change.
    if (g_batt_active && now_ms() >= g_next_batt_ms) {
        g_next_batt_ms = now_ms() + g_batt_tick_ms;
        battery_tick();
    }

    // Sensor/location streams: push a sample to each subscription whose deadline
    // has passed (the poll woke us at the soonest deadline, computed below).
    sensor_tick();

    // New connection.
    if (pfds[0].revents & POLLIN) {
        int c = accept(g_lfd, NULL, NULL);
        if (c >= 0) {
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_client_fd[i] < 0) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                close(c);   // table full
            } else {
                g_client_fd[slot] = c;
                g_client_app[slot][0] = '\0';   // unregistered until it says so
            }
        }
    }

    // Existing connections with data.
    for (int k = 1; k < nf; k++) {
        if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) {
            continue;
        }
        int slot = slot_of[k];
        int cfd = g_client_fd[slot];
        char buf[REQ_MAX];
        ssize_t r = read(cfd, buf, sizeof(buf) - 1);
        if (r <= 0) {
            close(cfd);
            g_client_fd[slot] = -1;
            g_client_app[slot][0] = '\0';   // mailbox gone
            sink_unsubscribe_fd(cfd);       // a notification sink went away
            settings_unsubscribe_fd(cfd);   // drop a settings observer too
            sensor_unsubscribe_fd(cfd);     // and any sensor/location streams
            continue;
        }
        buf[r] = '\0';
        // Each request is one newline-terminated line; handle every complete
        // line in this read (a status conn sends one; a control conn may
        // batch several).
        char *save = NULL;
        for (char *ln = strtok_r(buf, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            handle_line(slot, cfd, ln);
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    load_manifests();
    settings_load();
    publish_notif_count();   // store is empty at boot -> clear any stale count
    battery_init();

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    // Build the path straight into sun_path (bounded by its size, so no
    // truncation warning); reuse it for unlink/logging.
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    const char *path = addr.sun_path;
    unlink(path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("zsysd: socket");
        return 1;
    }
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("zsysd: bind");
        return 1;
    }
    if (listen(lfd, 8) < 0) {
        perror("zsysd: listen");
        return 1;
    }
    fprintf(stderr, "[zsysd] listening on %s (%d manifests loaded)\n", path,
            g_n_manifests);

    // g_client_fd is the live connection table (perm status conns are transient;
    // intents control conns are long-lived mailboxes keyed by g_client_app).
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_client_fd[i] = -1;
        g_client_app[i][0] = '\0';
    }

    g_lfd = lfd;

    for (;;) {
        // Wake for whichever periodic source is due first (battery drain or a
        // sensor/location stream). -1 from one loses to a finite deadline.
        int bt = battery_poll_timeout();
        int st = sensor_poll_timeout();
        int timeout = (bt < 0) ? st : (st < 0 ? bt : (bt < st ? bt : st));
        serve_once(timeout);

        // Replay whatever queued behind a consent dialog, now that it is gone.
        // Taken off the queue BEFORE handling, because handling one may raise the
        // next dialog (and so defer more lines behind it).
        while (g_n_deferred > 0 && !g_in_consent) {
            Deferred d = g_deferred[0];
            memmove(&g_deferred[0], &g_deferred[1],
                    (size_t)(--g_n_deferred) * sizeof(g_deferred[0]));
            if (d.slot >= 0 && d.slot < MAX_CLIENTS && g_client_fd[d.slot] >= 0) {
                handle_line(d.slot, g_client_fd[d.slot], d.line);
            }
        }
    }

    close(lfd);
    unlink(path);
    return 0;
}
