// zelto-install — the Zelto runtime package installer.
//
//     zelto-install <file.zap>
//
// A .zap is a signed ZIP holding an app's manifest (zelto.toml), its native
// binary (native/<abi>/...), optional assets, a content-hash manifest
// (MANIFEST.sha256), and a detached Ed25519 signature (SIGNATURE) over that
// manifest. See docs/packaging/zap-format.md + docs/packaging/signing.md.
//
// Install flow (any failure -> non-zero exit, nothing registered):
//   1. Unpack the .zap into a private temp dir (busybox `unzip`).
//   2. Verify SIGNATURE against the single trusted root key bundled at
//      /usr/share/zelto/keys/trusted.pub, over the bytes of MANIFEST.sha256
//      (libsodium crypto_sign_verify_detached — raw Ed25519, 64-byte sig /
//      32-byte key).
//   3. Verify every file listed in MANIFEST.sha256 hashes to its recorded value
//      (crypto_hash_sha256) — catches a binary tampered after signing.
//   4. Read the manifest's id=; refuse if absent.
//   5. Atomically install onto the persistent disk: the binary (+ assets) to
//      $ZELTO_DATA_DIR/installed/<id>/, and the manifest to
//      $ZELTO_DATA_DIR/apps/manifests/<id>.app with exec= rewritten to the
//      installed binary path. The app's private data dir
//      ($ZELTO_DATA_DIR/apps/<id>/) is left untouched (preserved across updates).
//   6. Nudge zsysd to re-scan its manifest dirs ({"op":"reload"}).
//
// Update rule: a later .zap with the same id and a HIGHER version= replaces the
// install; a lower version is refused as a downgrade. Key continuity is implicit
// here — there is one trusted root (a real publisher-key-per-app store is
// Planned). The granted permission set is bound to the package by virtue of the
// manifest coming from a signature-verified .zap.
//
// Plain C, no SDK dependency (like zsysd); links libsodium. (_GNU_SOURCE for
// mkdtemp/etc. comes from the project-wide build args.)
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sodium.h>

#define TRUSTED_PUB "/usr/share/zelto/keys/trusted.pub"
#define UNZIP_BIN "unzip"

// --- small helpers ---------------------------------------------------------

static const char *data_dir(void) {
    const char *d = getenv("ZELTO_DATA_DIR");
    return (d && d[0]) ? d : "/var/zelto";
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

// mkdir -p. Returns 0 on success.
static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t n = strlen(tmp);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

// Read an entire file into a malloc'd buffer (NUL-terminated for convenience).
// Returns 0 on success; caller frees *out.
static int read_file(const char *path, unsigned char **out, size_t *len) {
    *out = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t cap = 4096, n = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return -1;
    }
    for (;;) {
        if (n + 4096 + 1 > cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, 4096);
        if (r < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    *out = buf;
    *len = n;
    return 0;
}

// Copy src -> dst (mode applied), fsync'd so the install is durable on the disk.
static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) {
            close(in);
            close(out);
            return -1;
        }
        if (r == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                close(in);
                close(out);
                return -1;
            }
            off += w;
        }
    }
    fsync(out);
    close(in);
    close(out);
    return 0;
}

// Run an external command to completion; returns its exit code (-1 on failure).
static int run(const char *file, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(file, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void rm_rf(const char *path) {
    char *argv[] = {(char *)"rm", (char *)"-rf", (char *)path, NULL};
    (void)run("rm", argv);
}

// --- hashing / hex ---------------------------------------------------------

static void to_hex(const unsigned char *in, size_t n, char *out) {
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

// sha256 a file -> lowercase hex (65 bytes incl NUL). Returns 0 on success.
static int sha256_file_hex(const char *path, char *hex) {
    unsigned char *buf = NULL;
    size_t len = 0;
    if (read_file(path, &buf, &len) != 0) {
        return -1;
    }
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, buf, len);
    free(buf);
    to_hex(digest, sizeof(digest), hex);
    return 0;
}

// --- manifest (zelto.toml, our key=value subset) ---------------------------

typedef struct Manifest {
    char id[96];
    char name[64];
    char subtitle[96];
    char color[16];
    char version[32];
    char perms[256];
    char share_targets[256];
    char links[128];
    char exec[256];     // in-package native path, e.g. native/aarch64/zelto-widget
    char script[256];   // in-package .js path, e.g. script/jsdemo.js (script app)
} Manifest;

static void manifest_set(Manifest *m, const char *k, const char *v) {
    if (strcmp(k, "id") == 0) {
        snprintf(m->id, sizeof(m->id), "%s", v);
    } else if (strcmp(k, "name") == 0) {
        snprintf(m->name, sizeof(m->name), "%s", v);
    } else if (strcmp(k, "subtitle") == 0) {
        snprintf(m->subtitle, sizeof(m->subtitle), "%s", v);
    } else if (strcmp(k, "color") == 0) {
        snprintf(m->color, sizeof(m->color), "%s", v);
    } else if (strcmp(k, "version") == 0) {
        snprintf(m->version, sizeof(m->version), "%s", v);
    } else if (strcmp(k, "permissions") == 0) {
        snprintf(m->perms, sizeof(m->perms), "%s", v);
    } else if (strcmp(k, "share_targets") == 0) {
        snprintf(m->share_targets, sizeof(m->share_targets), "%s", v);
    } else if (strcmp(k, "links") == 0) {
        snprintf(m->links, sizeof(m->links), "%s", v);
    } else if (strcmp(k, "exec") == 0) {
        snprintf(m->exec, sizeof(m->exec), "%s", v);
    } else if (strcmp(k, "script") == 0) {
        snprintf(m->script, sizeof(m->script), "%s", v);
    }
}

static int parse_manifest(const char *path, Manifest *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        manifest_set(m, line, eq + 1);
    }
    fclose(f);
    return 0;
}

// Compare dotted-integer versions. >0 if a>b, 0 if equal, <0 if a<b. Missing
// components count as 0 ("1.2" < "1.2.1").
static int version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long va = 0, vb = 0;
        while (*a && *a != '.') {
            if (*a >= '0' && *a <= '9') {
                va = va * 10 + (*a - '0');
            }
            a++;
        }
        while (*b && *b != '.') {
            if (*b >= '0' && *b <= '9') {
                vb = vb * 10 + (*b - '0');
            }
            b++;
        }
        if (va != vb) {
            return va < vb ? -1 : 1;
        }
        if (*a == '.') {
            a++;
        }
        if (*b == '.') {
            b++;
        }
    }
    return 0;
}

// --- verification ----------------------------------------------------------

// Where the single trusted root key lives. On the device this is a fixed path in
// the read-only image — the whole point of the trust root is that a package
// cannot choose it. The simulator has no image, so ZELTO_TRUSTED_KEY retargets it
// at the repo's dev key (the ZELTO_CONSENT_BIN idiom). That override is a
// DEVELOPMENT affordance: it is only as trustworthy as the environment the
// installer is launched with, which on the device is the read-only init.
static const char *trusted_key_path(void) {
    const char *k = getenv("ZELTO_TRUSTED_KEY");
    return (k && k[0]) ? k : TRUSTED_PUB;
}

// Verify the detached signature over MANIFEST.sha256 against the trusted root.
static bool verify_signature(const char *workdir) {
    const char *pubpath = trusted_key_path();
    unsigned char *pub = NULL, *sig = NULL, *man = NULL;
    size_t publen = 0, siglen = 0, manlen = 0;
    bool ok = false;

    char mpath[1024], spath[1024];
    snprintf(mpath, sizeof(mpath), "%s/MANIFEST.sha256", workdir);
    snprintf(spath, sizeof(spath), "%s/SIGNATURE", workdir);

    if (read_file(pubpath, &pub, &publen) != 0 ||
        publen != crypto_sign_PUBLICKEYBYTES) {
        fprintf(stderr, "[zelto-install] trusted key missing/bad (%s)\n", pubpath);
        goto out;
    }
    if (read_file(spath, &sig, &siglen) != 0 ||
        siglen != crypto_sign_BYTES) {
        fprintf(stderr, "[zelto-install] SIGNATURE missing/wrong size\n");
        goto out;
    }
    if (read_file(mpath, &man, &manlen) != 0) {
        fprintf(stderr, "[zelto-install] MANIFEST.sha256 missing\n");
        goto out;
    }
    if (crypto_sign_verify_detached(sig, man, manlen, pub) != 0) {
        fprintf(stderr, "[zelto-install] SIGNATURE does not verify against the "
                        "trusted key (wrong key or tampered manifest)\n");
        goto out;
    }
    ok = true;
out:
    free(pub);
    free(sig);
    free(man);
    return ok;
}

// Verify every file in MANIFEST.sha256 hashes to its recorded value.
static bool verify_hashes(const char *workdir) {
    char mpath[1024];
    snprintf(mpath, sizeof(mpath), "%s/MANIFEST.sha256", workdir);
    FILE *f = fopen(mpath, "r");
    if (!f) {
        return false;
    }
    bool ok = true;
    int checked = 0;
    char line[1024];
    while (ok && fgets(line, sizeof(line), f)) {
        chomp(line);
        if (line[0] == '\0') {
            continue;
        }
        // "<64 hex>  <path>"  (sha256sum text format; path may start "./").
        if (strlen(line) < 67) {
            continue;
        }
        char want[65];
        memcpy(want, line, 64);
        want[64] = '\0';
        const char *p = line + 64;
        while (*p == ' ' || *p == '\t' || *p == '*') {
            p++;
        }
        char fpath[1200];
        snprintf(fpath, sizeof(fpath), "%s/%s", workdir, p);
        char got[65];
        if (sha256_file_hex(fpath, got) != 0) {
            fprintf(stderr, "[zelto-install] cannot hash listed file: %s\n", p);
            ok = false;
            break;
        }
        if (strcmp(want, got) != 0) {
            fprintf(stderr, "[zelto-install] HASH MISMATCH for %s\n", p);
            fprintf(stderr, "                want %s\n                got  %s\n",
                    want, got);
            ok = false;
            break;
        }
        checked++;
    }
    fclose(f);
    if (ok) {
        fprintf(stderr, "[zelto-install] %d file hash(es) verified\n", checked);
    }
    return ok;
}

// --- zsysd reload ----------------------------------------------------------

// Tell zsysd to re-scan its manifest dirs so a same-boot install is visible to
// the permission/intent broker immediately. Best-effort.
static void zsysd_reload(void) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime) {
        runtime = "/run";
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/zsysd.sock", runtime);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        const char *msg = "{\"op\":\"reload\"}\n";
        ssize_t w = write(fd, msg, strlen(msg));
        (void)w;
    }
    close(fd);
}

// --- install ---------------------------------------------------------------

// Write the runtime manifest atomically (tmp + rename), exec= rewritten to the
// installed binary's absolute path. Preserves every other declared field so the
// broker still sees the package's permissions / intent handlers.
static int write_runtime_manifest(const char *manifests_dir, const Manifest *m,
                                  const char *installed_exec) {
    char target[1200], tmp[1300];
    snprintf(target, sizeof(target), "%s/%s.app", manifests_dir, m->id);
    snprintf(tmp, sizeof(tmp), "%s/%s.app.tmp", manifests_dir, m->id);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "# Installed at runtime by zelto-install from a signed .zap.\n");
    fprintf(f, "id=%s\n", m->id);
    if (m->name[0]) {
        fprintf(f, "name=%s\n", m->name);
    }
    if (m->subtitle[0]) {
        fprintf(f, "subtitle=%s\n", m->subtitle);
    }
    if (m->color[0]) {
        fprintf(f, "color=%s\n", m->color);
    }
    if (m->version[0]) {
        fprintf(f, "version=%s\n", m->version);
    }
    if (m->perms[0]) {
        fprintf(f, "permissions=%s\n", m->perms);
    }
    if (m->share_targets[0]) {
        fprintf(f, "share_targets=%s\n", m->share_targets);
    }
    if (m->links[0]) {
        fprintf(f, "links=%s\n", m->links);
    }
    fprintf(f, "exec=%s\n", installed_exec);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);

    if (rename(tmp, target) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: zelto-install <file.zap>\n");
        return 2;
    }
    const char *zap = argv[1];
    if (sodium_init() < 0) {
        fprintf(stderr, "[zelto-install] libsodium init failed\n");
        return 1;
    }

    // 1. Unpack into a private temp dir.
    char workdir[] = "/tmp/zinstall.XXXXXX";
    if (!mkdtemp(workdir)) {
        fprintf(stderr, "[zelto-install] cannot create temp dir\n");
        return 1;
    }
    int rc = 1;
    {
        char *uz[] = {(char *)UNZIP_BIN, (char *)"-o", (char *)"-q",
                      (char *)zap, (char *)"-d", workdir, NULL};
        if (run(UNZIP_BIN, uz) != 0) {
            fprintf(stderr, "[zelto-install] failed to unzip %s\n", zap);
            goto cleanup;
        }
    }

    // 2 + 3. Verify the signature, then every file hash.
    if (!verify_signature(workdir)) {
        goto cleanup;
    }
    if (!verify_hashes(workdir)) {
        goto cleanup;
    }

    // 4. Read the manifest; id= is required.
    Manifest m;
    {
        char mpath[1024];
        snprintf(mpath, sizeof(mpath), "%s/zelto.toml", workdir);
        if (parse_manifest(mpath, &m) != 0) {
            fprintf(stderr, "[zelto-install] no zelto.toml in package\n");
            goto cleanup;
        }
    }
    if (!m.id[0]) {
        fprintf(stderr, "[zelto-install] manifest has no id=; refusing\n");
        goto cleanup;
    }
    // A package carries EITHER a native binary (exec=) or a Zelto Script entry
    // (script=). A script app ships no binary: it runs on the shared runtime
    // already installed on the device, so what the package contributes is the
    // .js — which the signature and the per-file hash cover exactly as they would
    // an ELF.
    if (!m.exec[0] && !m.script[0]) {
        fprintf(stderr,
                "[zelto-install] manifest has neither exec= nor script=; "
                "refusing\n");
        goto cleanup;
    }

    const char *data = data_dir();

    // Update rule: refuse a downgrade (same id, lower version than installed).
    char manifests_dir[1024];
    snprintf(manifests_dir, sizeof(manifests_dir), "%s/apps/manifests", data);
    {
        char existing[1200];
        snprintf(existing, sizeof(existing), "%s/%s.app", manifests_dir, m.id);
        Manifest old;
        if (parse_manifest(existing, &old) == 0 && old.id[0]) {
            if (m.version[0] && old.version[0] &&
                version_cmp(m.version, old.version) < 0) {
                fprintf(stderr,
                        "[zelto-install] %s v%s is older than installed v%s; "
                        "refusing downgrade\n",
                        m.id, m.version, old.version);
                goto cleanup;
            }
            fprintf(stderr, "[zelto-install] updating %s (installed v%s -> v%s)\n",
                    m.id, old.version[0] ? old.version : "?",
                    m.version[0] ? m.version : "?");
        }
    }

    // 5. Install the binary onto the persistent disk.
    char installed_dir[1024];
    snprintf(installed_dir, sizeof(installed_dir), "%s/installed/%s", data, m.id);
    if (mkdir_p(installed_dir) != 0) {
        fprintf(stderr, "[zelto-install] cannot create %s\n", installed_dir);
        goto cleanup;
    }
    // The payload: the ELF, or the script entry. A .js is data, not an image —
    // install it 0644 (it is executed by the runtime, never exec'd itself).
    const char *payload = m.script[0] ? m.script : m.exec;
    char bin_src[1300], bin_dst[1400];
    snprintf(bin_src, sizeof(bin_src), "%s/%s", workdir, payload);
    snprintf(bin_dst, sizeof(bin_dst), "%s/%s", installed_dir,
             base_name(payload));
    if (copy_file(bin_src, bin_dst, m.script[0] ? 0644 : 0755) != 0) {
        fprintf(stderr, "[zelto-install] cannot install payload -> %s\n", bin_dst);
        goto cleanup;
    }

    // A script app's exec= is SYNTHESISED here rather than declared: the package
    // must not get to choose which interpreter runs it, or a manifest could point
    // exec= at any binary on the device and the signature would faithfully attest
    // to it. The installer names the runtime; the package only supplies the .js.
    // (ZELTO_SCRIPT_BIN retargets it at the build tree for the simulator, the same
    // ZELTO_CONSENT_BIN idiom zsysd uses.)
    char exec_cmd[1600];
    if (m.script[0]) {
        const char *runtime = getenv("ZELTO_SCRIPT_BIN");
        if (!runtime || !runtime[0]) {
            runtime = "/usr/bin/zelto-script";
        }
        snprintf(exec_cmd, sizeof(exec_cmd), "%s --id %s %s", runtime, m.id,
                 bin_dst);
    } else {
        snprintf(exec_cmd, sizeof(exec_cmd), "%s", bin_dst);
    }

    // Optional assets/ tree (best-effort; copied wholesale).
    {
        char assets_src[1024];
        snprintf(assets_src, sizeof(assets_src), "%s/assets", workdir);
        struct stat st;
        if (stat(assets_src, &st) == 0 && S_ISDIR(st.st_mode)) {
            char assets_dst[1100];
            snprintf(assets_dst, sizeof(assets_dst), "%s/assets", installed_dir);
            mkdir_p(assets_dst);
            char *cp[] = {(char *)"cp", (char *)"-a", assets_src,
                          installed_dir, NULL};
            (void)run("cp", cp);
        }
    }

    // 6. Register the runtime manifest (atomic) + nudge zsysd.
    if (mkdir_p(manifests_dir) != 0) {
        fprintf(stderr, "[zelto-install] cannot create %s\n", manifests_dir);
        goto cleanup;
    }
    if (write_runtime_manifest(manifests_dir, &m, exec_cmd) != 0) {
        fprintf(stderr, "[zelto-install] cannot write runtime manifest\n");
        goto cleanup;
    }
    // Flush the disk so the install survives an unclean shutdown / reboot.
    sync();
    zsysd_reload();

    fprintf(stderr, "[zelto-install] installed %s (%s, %s) -> %s\n", m.id,
            m.name[0] ? m.name : "?", m.script[0] ? "script" : "native",
            exec_cmd);
    rc = 0;

cleanup:
    rm_rf(workdir);
    return rc;
}
