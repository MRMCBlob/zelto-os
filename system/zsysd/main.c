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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MANIFEST_DIR "/usr/share/zelto/apps"
#define CONSENT_BIN "/usr/bin/zelto-consent"
#define CHOOSER_BIN "/usr/bin/zelto-chooser"
#define MAX_GRANTS 128
#define MAX_MANIFESTS 64
#define MAX_CLIENTS 32
#define MAX_PENDING 16
#define REQ_MAX 512

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
    char kind[16];    // "share" | "open_url"
    char mime[64];
    char url[256];
    char payload[256];
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

// Scan the manifest dir once into g_manifests (id + permissions= line per file).
static void load_manifests(void) {
    DIR *d = opendir(MANIFEST_DIR);
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
        snprintf(path, sizeof(path), "%s/%s", MANIFEST_DIR, de->d_name);
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

        if (poll(pfds, (nfds_t)nf, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("zsysd: poll");
            break;
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
