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
    char exec[160];           // launch path for launch-if-needed delivery
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

// --- grant store (the cached decisions) -----------------------------------
typedef struct Grant {
    char app_id[96];
    char perm[32];
    bool granted;
    bool used;
} Grant;
static Grant g_grants[MAX_GRANTS];

// --- notification store + shade sink ---------------------------------------
// zsysd's third duty: a notification store + router. A perm-gated notify_post
// assigns a monotonic global id, stores the notification, and pushes a
// notify_show to the shade sink. The shade (a libzelto app that sent
// notify_subscribe) is recorded by its ctrl fd; only one sink (last wins). The
// shade reports a body tap (notify_tap -> drop the banner; it routed the deep
// link itself via z_open_url) or an action tap (notify_action -> route to the
// poster's mailbox, then drop). Cancel removes a notification by id. The store
// is in-memory only (lost on reboot); no history/grouping (heads-up only).
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
} Notification;
static Notification g_notifs[MAX_NOTIFS];
static int64_t g_next_notif_id = 1;
static int g_shade_fd = -1;   // ctrl fd of the subscribed shade (-1 = none)

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
// fd on its disconnect in the read loop (mirroring g_shade_fd).
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

// Show the System-UI consent dialog and block until the user answers. The
// dialog is a separate overlay layer-shell helper; it exits 0 for Allow, 1 for
// Deny (anything else — e.g. exec failure — counts as Deny).
static bool show_consent(const char *app_id, const char *perm) {
    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        execl(CONSENT_BIN, "zelto-consent", app_id, perm, (char *)NULL);
        _exit(2);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
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
        execl(m->exec, m->exec, (char *)NULL);
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
static int run_chooser(char ids[][96], int n) {
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
        int pick = run_chooser(cands, n);
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
            idx = run_chooser(cands, n);
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
        "\"action_id\":\"%s\",\"action_title\":\"%s\"}\n",
        (long long)n->id, n->app_id, n->title, n->body, n->tap_route,
        n->action_id, n->action_title);
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

// Remove a notification from the store and hide its banner on the shade.
static void notif_drop(int64_t id) {
    Notification *n = notif_find(id);
    if (!n) {
        return;
    }
    n->used = false;
    if (g_shade_fd >= 0) {
        send_notify_hide(g_shade_fd, id);
    }
    publish_notif_count();
}

// notify_post: perm-gate (the consent prompt blocks here exactly like a
// perm_request — the posting app waits synchronously on its transient conn),
// assign a global id, store it, reply {"id":"N"} on this connection, and push a
// notify_show to the shade sink. A denial replies {"id":"-1"}.
static void handle_notify_post(int fd, const char *line) {
    char app_id[96] = {0}, title[128] = {0}, body[192] = {0}, channel[64] = {0},
         tap_route[256] = {0}, action_id[64] = {0}, action_title[64] = {0};
    json_get(line, "app_id", app_id, sizeof(app_id));
    json_get(line, "title", title, sizeof(title));
    json_get(line, "body", body, sizeof(body));
    json_get(line, "channel", channel, sizeof(channel));
    json_get(line, "tap_route", tap_route, sizeof(tap_route));
    json_get(line, "action_id", action_id, sizeof(action_id));
    json_get(line, "action_title", action_title, sizeof(action_title));

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

    char reply[64];
    int m = snprintf(reply, sizeof(reply), "{\"id\":\"%lld\"}\n",
                     (long long)n->id);
    if (m > 0) {
        ssize_t w = write(fd, reply, (size_t)m);
        (void)w;
    }
    fprintf(stderr, "[zsysd] notify_post app=%s id=%lld -> shade %s\n", app_id,
            (long long)n->id, g_shade_fd >= 0 ? "yes" : "(no sink)");
    if (g_shade_fd >= 0) {
        send_notify_show(g_shade_fd, n);
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
// system is the poster). Reuses the notify store + shade sink. Used for the
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
    snprintf(n->app_id, sizeof(n->app_id), "os.zelto.system");
    snprintf(n->title, sizeof(n->title), "%s", title);
    snprintf(n->body, sizeof(n->body), "%s", body);
    n->channel[0] = n->tap_route[0] = '\0';
    n->action_id[0] = n->action_title[0] = '\0';
    fprintf(stderr, "[zsysd] system notification id=%lld: %s\n",
            (long long)n->id, title);
    if (g_shade_fd >= 0) {
        send_notify_show(g_shade_fd, n);
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

// Process one request line. Perm ops reply on the same connection; register and
// intent_resolve come over a persistent control connection (no reply). `slot`
// is the client's table index, so a register can record its mailbox app_id.
static void handle_line(int slot, int fd, char *line) {
    char op[32] = {0};
    json_get(line, "op", op, sizeof(op));

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
    // Shade subscribes as the sink (last subscriber wins). Records its ctrl fd.
    if (strcmp(op, "notify_subscribe") == 0) {
        g_shade_fd = fd;
        fprintf(stderr, "[zsysd] notify sink subscribed (slot %d)\n", slot);
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
    // settings_subscribe: record this ctrl fd in the observer set (multiple).
    if (strcmp(op, "settings_subscribe") == 0) {
        settings_subscribe_fd(fd);
        fprintf(stderr, "[zsysd] settings observer subscribed (slot %d, %d total)\n",
                slot, g_n_settings_subs);
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

    for (;;) {
        struct pollfd pfds[1 + MAX_CLIENTS];
        int slot_of[1 + MAX_CLIENTS];
        pfds[0].fd = lfd;
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

        if (poll(pfds, (nfds_t)nf, battery_poll_timeout()) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("zsysd: poll");
            break;
        }

        // Battery: advance the source whenever a tick is due (poll woke us at the
        // deadline even with no socket traffic). Cheap in-memory + one settings
        // fan-out on an actual change.
        if (g_batt_active && now_ms() >= g_next_batt_ms) {
            g_next_batt_ms = now_ms() + g_batt_tick_ms;
            battery_tick();
        }

        // New connection.
        if (pfds[0].revents & POLLIN) {
            int c = accept(lfd, NULL, NULL);
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
                if (cfd == g_shade_fd) {
                    g_shade_fd = -1;   // shade sink disconnected
                }
                settings_unsubscribe_fd(cfd);   // drop a settings observer too
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

    close(lfd);
    unlink(path);
    return 0;
}
