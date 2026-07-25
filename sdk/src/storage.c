// libzelto storage client: preferences, files, and SQLite — all scoped to the
// app's private data directory on a real writable disk ($ZELTO_DATA_DIR, mounted
// by init at /var/zelto), keyed by the active app's app_id.
//
//   <data>/apps/<app_id>/documents   persistent user data (prefs file, DBs)
//   <data>/apps/<app_id>/cache        evictable
//
// Unlike the perm/intent/notify clients, storage needs no zsysd round-trip: it
// is direct filesystem access. zsysd's role (provisioning the dir at install)
// is implicit here — we mkdir the tree on first use. Every write fsync()s so the
// data reaches the host image (the guest flush -> virtio-blk flush -> host fsync
// chain) and survives a reboot.
// (_GNU_SOURCE for the GNU extensions comes from the project-wide build args.)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "internal.h"

// ---------------------------------------------------------------------------
// Path helpers.
// ---------------------------------------------------------------------------

// The persistent data root, exported to apps by init. Falls back to /var/zelto
// if the env is unset (matching init's mountpoint).
static const char *data_root(void) {
    const char *d = getenv("ZELTO_DATA_DIR");
    return (d && d[0]) ? d : "/var/zelto";
}

// Create `path` and every missing parent directory (like mkdir -p). The final
// component is treated as a directory. Best-effort; existing dirs are fine.
static void mkdir_p(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                /* keep going; a later component may still succeed */
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        /* ignore */
    }
}

// Build "<data>/apps/<app_id>/<sub>" into `out`, creating that dir. Returns
// false if there is no active app_id or the path would overflow.
static bool app_subdir(char *out, size_t cap, const char *sub) {
    const char *id = z_active_app_id();
    if (!id || !id[0]) {
        return false;
    }
    int m = snprintf(out, cap, "%s/apps/%s/%s", data_root(), id, sub);
    if (m <= 0 || (size_t)m >= cap) {
        return false;
    }
    mkdir_p(out);
    return true;
}

// Build "<data>/apps/<app_id>/<sub>/<rel>" (the dir is created; the file is not).
// Returns a heap string the caller frees, or NULL on failure.
static char *app_path(const char *sub, const char *rel) {
    char dir[512];
    if (!app_subdir(dir, sizeof(dir), sub)) {
        return NULL;
    }
    char full[768];
    int m = snprintf(full, sizeof(full), "%s/%s", dir, rel ? rel : "");
    if (m <= 0 || (size_t)m >= sizeof(full)) {
        return NULL;
    }
    return strdup(full);
}

char *z_path_documents(const char *rel) { return app_path("documents", rel); }
char *z_path_cache(const char *rel) { return app_path("cache", rel); }

// THE SHARED MEDIA ROOT — "<data>/media/<rel>", and deliberately NOT under
// apps/<app_id>/.
//
// Everything above this line is scoped by the calling app precisely so that one
// app cannot read another's files. A photo library is the first thing in this OS
// that has to break that, and it is worth writing down why rather than treating
// it as an oversight: a library is defined by having MORE THAN ONE WRITER and
// more than one reader. The screenshot service and the camera both put photos
// in; Photos browses them, the share sheet sends them, and the wallpaper picker
// (P25) is supposed to be able to choose one. If the pictures lived in the
// camera's private directory, "set as wallpaper" would be a copy and the library
// would just be a directory one app happens to own.
//
// So this is a THIRD root beside documents/ and cache/, at the same level as
// apps/, with the same lifetime and the same fsync guarantees. The scoping that
// was removed is replaced by a convention rather than by nothing: media/ holds
// system-defined subtrees (photos/, thumbs/) named by system/common/photos.h,
// not per-app namespaces.
//
// WHAT THIS IS NOT. It is not a permission boundary — any process that can call
// libzelto can read it. Gating who may enumerate the library belongs to zsysd
// (the `camera` grant already exists and a `photos` grant would join it), and
// pretending a path is a control would be the same mistake as a privacy
// indicator an app can suppress. Stated here so the absence is a decision on
// record instead of an assumption.
char *z_path_media(const char *rel) {
    char dir[512];
    int m = snprintf(dir, sizeof(dir), "%s/media", data_root());
    if (m <= 0 || (size_t)m >= sizeof(dir)) {
        return NULL;
    }
    mkdir_p(dir);
    char full[768];
    m = snprintf(full, sizeof(full), "%s/%s", dir, rel ? rel : "");
    if (m <= 0 || (size_t)m >= sizeof(full)) {
        return NULL;
    }
    return strdup(full);
}

// Create `path` and its parents. Exposed because the media tree's subdirectories
// are named by a shared header rather than by this file, so the caller that owns
// the convention is the one that has to make the directory.
void z_mkdir_p(const char *path) { mkdir_p(path); }

// ---------------------------------------------------------------------------
// Files.
// ---------------------------------------------------------------------------

bool z_file_write(const char *path, const void *data, size_t len) {
    char *full = z_path_documents(path);
    if (!full) {
        return false;
    }
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    free(full);
    if (fd < 0) {
        return false;
    }
    bool ok = true;
    const char *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w <= 0) {
            ok = false;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (ok) {
        fsync(fd);   // push through ext4 -> virtio-blk -> host image
    }
    close(fd);
    return ok;
}

ZBytes z_file_read(const char *path) {
    ZBytes out = {0};
    char *full = z_path_documents(path);
    if (!full) {
        return out;
    }
    int fd = open(full, O_RDONLY);
    free(full);
    if (fd < 0) {
        return out;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return out;
    }
    size_t len = (size_t)st.st_size;
    char *buf = malloc(len + 1);
    if (!buf) {
        close(fd);
        return out;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    out.data = buf;
    out.len = got;
    out.ok = (got == len);
    return out;
}

bool z_file_delete(const char *path) {
    char *full = z_path_documents(path);
    if (!full) {
        return false;
    }
    bool ok = unlink(full) == 0;
    free(full);
    return ok;
}

void z_list_free(ZList *list) {
    if (!list) {
        return;
    }
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    free(list);
}

ZList *z_file_list(const char *dir) {
    char *full = z_path_documents(dir ? dir : "");
    if (!full) {
        return NULL;
    }
    DIR *d = opendir(full);
    free(full);
    if (!d) {
        return NULL;
    }
    ZList *list = calloc(1, sizeof(*list));
    if (!list) {
        closedir(d);
        return NULL;
    }
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (list->count == cap) {
            int ncap = cap ? cap * 2 : 8;
            char **ni = realloc(list->items, (size_t)ncap * sizeof(char *));
            if (!ni) {
                break;
            }
            list->items = ni;
            cap = ncap;
        }
        list->items[list->count++] = strdup(e->d_name);
    }
    closedir(d);
    return list;
}

// ---------------------------------------------------------------------------
// Preferences: a tiny TAB-separated key/value file in documents (.prefs). Keys
// and values must not contain TAB or newline (fine for the small settings prefs
// is meant for). Load-all / rewrite-all on each mutation — prefs sets are small
// and rare. Reads return a pointer into a per-call static buffer.
// ---------------------------------------------------------------------------

#define PREFS_FILE ".prefs"

// Read the whole prefs file into a heap buffer (NUL-terminated), or NULL.
static char *prefs_load(void) {
    ZBytes b = z_file_read(PREFS_FILE);
    if (!b.data) {
        return NULL;
    }
    return b.data;   // already NUL-terminated by z_file_read
}

// Find `key`'s value in the loaded text; copies into out (cap). Returns true if
// found. Lines are "key\tvalue\n".
static bool prefs_find(const char *text, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        const char *tab = memchr(p, '\t', linelen);
        if (tab && (size_t)(tab - p) == klen && memcmp(p, key, klen) == 0) {
            size_t vlen = linelen - (size_t)(tab + 1 - p);
            if (vlen >= cap) {
                vlen = cap - 1;
            }
            memcpy(out, tab + 1, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

// Rewrite the prefs file with `key` set to `value` (value==NULL removes it).
static bool prefs_store(const char *key, const char *value) {
    char *text = prefs_load();
    // Build the new content: every existing line except `key`, then the new one.
    char *buf = malloc(8192);
    if (!buf) {
        free(text);
        return false;
    }
    size_t len = 0;
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        const char *tab = memchr(p, '\t', linelen);
        bool is_key = tab && (size_t)(tab - p) == klen && memcmp(p, key, klen) == 0;
        if (!is_key && linelen > 0 && len + linelen + 1 < 8192) {
            memcpy(buf + len, p, linelen);
            len += linelen;
            buf[len++] = '\n';
        }
        p = eol ? eol + 1 : NULL;
    }
    if (value) {
        int m = snprintf(buf + len, 8192 - len, "%s\t%s\n", key, value);
        if (m > 0 && (size_t)m < 8192 - len) {
            len += (size_t)m;
        }
    }
    free(text);
    bool ok = z_file_write(PREFS_FILE, buf, len);
    free(buf);
    return ok;
}

bool z_prefs_set_str(const char *key, const char *value) {
    if (!key || !value) {
        return false;
    }
    return prefs_store(key, value);
}

const char *z_prefs_get_str(const char *key, const char *fallback) {
    static char val[1024];
    if (!key) {
        return fallback;
    }
    char *text = prefs_load();
    if (!text) {
        return fallback;
    }
    bool found = prefs_find(text, key, val, sizeof(val));
    free(text);
    return found ? val : fallback;
}

bool z_prefs_set_int(const char *key, int64_t value) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    return z_prefs_set_str(key, tmp);
}

int64_t z_prefs_get_int(const char *key, int64_t fallback) {
    const char *s = z_prefs_get_str(key, NULL);
    if (!s) {
        return fallback;
    }
    return (int64_t)strtoll(s, NULL, 10);
}

bool z_prefs_remove(const char *key) {
    if (!key) {
        return false;
    }
    return prefs_store(key, NULL);
}

// ---------------------------------------------------------------------------
// Database (SQLite). Opaque wrappers over sqlite3 / sqlite3_stmt. The DB file
// lives in documents; SQLite's default synchronous=FULL fsync()s on commit, so
// writes survive a reboot like the file/prefs paths.
// ---------------------------------------------------------------------------

struct ZDatabase {
    sqlite3 *db;
};

struct ZRows {
    sqlite3_stmt *stmt;
    bool done;
};

// z_args macro helpers: tag a parameter by type at the call site.
ZArg z_arg_int_(int64_t x) {
    ZArg a = {0, x, NULL};
    return a;
}
ZArg z_arg_str_(const char *x) {
    ZArg a = {1, 0, x};
    return a;
}
ZArgs z_args_make_(int n, const ZArg *v) {
    ZArgs args = {0, {{0, 0, NULL}}};
    if (n < 0) {
        n = 0;
    }
    if (n > (int)(sizeof(args.v) / sizeof(args.v[0]))) {
        n = (int)(sizeof(args.v) / sizeof(args.v[0]));
    }
    args.n = n;
    for (int i = 0; i < n; i++) {
        args.v[i] = v[i];
    }
    return args;
}

ZDatabase *z_db_open(const char *name) {
    if (!name || !name[0]) {
        return NULL;
    }
    // Ensure the file lands in documents and ends in .db.
    char fname[128];
    const char *dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".db") == 0) {
        snprintf(fname, sizeof(fname), "%s", name);
    } else {
        snprintf(fname, sizeof(fname), "%s.db", name);
    }
    char *path = z_path_documents(fname);
    if (!path) {
        return NULL;
    }
    sqlite3 *raw = NULL;
    int rc = sqlite3_open(path, &raw);
    free(path);
    if (rc != SQLITE_OK) {
        if (raw) {
            sqlite3_close(raw);
        }
        return NULL;
    }
    ZDatabase *db = calloc(1, sizeof(*db));
    if (!db) {
        sqlite3_close(raw);
        return NULL;
    }
    db->db = raw;
    return db;
}

bool z_db_exec(ZDatabase *db, const char *sql) {
    if (!db || !db->db || !sql) {
        return false;
    }
    return sqlite3_exec(db->db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

// Prepare `sql` and bind `args` (1-based placeholders). Returns the stmt or NULL.
static sqlite3_stmt *prepare_bound(sqlite3 *raw, const char *sql, ZArgs args) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    for (int i = 0; i < args.n; i++) {
        if (args.v[i].is_text) {
            sqlite3_bind_text(stmt, i + 1, args.v[i].s ? args.v[i].s : "", -1,
                              SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_int64(stmt, i + 1, args.v[i].i);
        }
    }
    return stmt;
}

bool z_db_run(ZDatabase *db, const char *sql, ZArgs args) {
    if (!db || !db->db || !sql) {
        return false;
    }
    sqlite3_stmt *stmt = prepare_bound(db->db, sql, args);
    if (!stmt) {
        return false;
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

ZRows *z_db_query(ZDatabase *db, const char *sql, ZArgs args) {
    if (!db || !db->db || !sql) {
        return NULL;
    }
    sqlite3_stmt *stmt = prepare_bound(db->db, sql, args);
    if (!stmt) {
        return NULL;
    }
    ZRows *r = calloc(1, sizeof(*r));
    if (!r) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    r->stmt = stmt;
    return r;
}

bool z_rows_next(ZRows *r) {
    if (!r || r->done) {
        return false;
    }
    if (sqlite3_step(r->stmt) == SQLITE_ROW) {
        return true;
    }
    r->done = true;
    return false;
}

int64_t z_rows_int(ZRows *r, int col) {
    if (!r) {
        return 0;
    }
    return sqlite3_column_int64(r->stmt, col);
}

const char *z_rows_str(ZRows *r, int col) {
    if (!r) {
        return NULL;
    }
    return (const char *)sqlite3_column_text(r->stmt, col);
}

void z_rows_free(ZRows *r) {
    if (!r) {
        return;
    }
    if (r->stmt) {
        sqlite3_finalize(r->stmt);
    }
    free(r);
}

void z_db_close(ZDatabase *db) {
    if (!db) {
        return;
    }
    if (db->db) {
        sqlite3_close(db->db);
    }
    free(db);
}
