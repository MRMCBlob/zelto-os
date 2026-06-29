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
#define MAX_GRANTS 128
#define MAX_MANIFESTS 64
#define MAX_CLIENTS 16
#define REQ_MAX 512

// --- manifest table (declared permissions per app) ------------------------
typedef struct Manifest {
    char id[96];
    char perms[256];   // CSV of declared permission names ("camera,network")
} Manifest;
static Manifest g_manifests[MAX_MANIFESTS];
static int g_n_manifests;

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

// Process one request line and write back one reply line.
static void handle_line(int fd, char *line) {
    char op[32] = {0}, app_id[96] = {0}, perm[32] = {0};
    json_get(line, "op", op, sizeof(op));
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

    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i] = -1;
    }

    for (;;) {
        struct pollfd pfds[1 + MAX_CLIENTS];
        int slot_of[1 + MAX_CLIENTS];
        pfds[0].fd = lfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        int nf = 1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                pfds[nf].fd = clients[i];
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
                    if (clients[i] < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    close(c);   // table full
                } else {
                    clients[slot] = c;
                }
            }
        }

        // Existing connections with data.
        for (int k = 1; k < nf; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            int slot = slot_of[k];
            int cfd = clients[slot];
            char buf[REQ_MAX];
            ssize_t r = read(cfd, buf, sizeof(buf) - 1);
            if (r <= 0) {
                close(cfd);
                clients[slot] = -1;
                continue;
            }
            buf[r] = '\0';
            // Each request is one newline-terminated line; handle every complete
            // line in this read (a short-lived status connection sends one).
            char *save = NULL;
            for (char *ln = strtok_r(buf, "\n", &save); ln;
                 ln = strtok_r(NULL, "\n", &save)) {
                handle_line(cfd, ln);
            }
        }
    }

    close(lfd);
    unlink(path);
    return 0;
}
