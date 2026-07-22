// libzelto networking client (P12). A hand-rolled, non-blocking HTTP/1.x client
// and a minimal WebSocket client, both driven from the app's multi-fd poll loop
// (app.c) so a request never blocks the render loop — the canonical pattern the
// perm/intent/notify clients already use for their sockets.
//
// A ZNetRequest is a heap builder + in-flight handle. z_net_send first checks the
// `network` permission (z_perm_status; if undecided it shows the consent dialog
// via z_perm_request and resumes on grant), then opens a non-blocking TCP socket
// to the (numeric IPv4) host, writes the request, and reads the response until
// EOF (Connection: close). When the response is complete the ZNetCallback fires
// from the loop with res->status / res->ok + z_net_body / z_net_header.
//
// Scope: plain HTTP (no TLS), numeric IPv4 hosts (no DNS) — both Planned. The
// WebSocket client does the HTTP/1.1 Upgrade handshake and unfragmented text
// frames; binary/fragmented framing is a documented stub.
// (_GNU_SOURCE comes from the project-wide build args; do not redefine it.)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"
#include "zelto/ui.h"

// A tiny substring search over a length-bounded (non-NUL-terminated) buffer.
static char *mem_find(const char *hay, size_t hn, const char *needle, size_t nn);

// At most this many concurrent HTTP requests / WebSockets, total bounded by
// Z_NET_POLL_MAX (one fd each) so they fit the app loop's poll array.
#define Z_NET_MAX 8
#define Z_WS_MAX 4

// ---------------------------------------------------------------------------
// HTTP request state machine.
// ---------------------------------------------------------------------------
typedef enum {
    NS_PERM_WAIT,   // waiting on the async permission consent (no fd yet)
    NS_CONNECT,     // non-blocking connect in flight (wait POLLOUT)
    NS_SEND,        // writing the request (wait POLLOUT)
    NS_RECV,        // reading the response (wait POLLIN)
} NetState;

struct ZNetRequest {
    char method[8];
    char host[128];
    int port;
    char path[512];
    char hdrs[1024];        // accumulated "K: V\r\n" request headers
    void *body;
    size_t body_len;

    ZNetCallback cb;
    void *ud;

    int fd;                 // -1 until connect (still -1 during NS_PERM_WAIT)
    NetState state;
    char *sendbuf;
    size_t send_len, send_off;
    char *rbuf;             // response accumulator (NUL-terminated)
    size_t rcap, rlen;
};

// ZNetResponse is defined publicly in <zelto/ui.h> (status + ok are read by the
// app; the rest are framework-internal and used only here).

static ZNetRequest *g_active[Z_NET_MAX];

// ---------------------------------------------------------------------------
// WebSocket state machine.
// ---------------------------------------------------------------------------
typedef enum {
    WS_CONNECT,   // non-blocking connect (wait POLLOUT)
    WS_HS,        // handshake sent, reading the 101 response (wait POLLIN)
    WS_OPEN,      // open: reading frames (wait POLLIN)
    WS_DEAD,      // closed/failed (skipped by the loop, freed lazily)
} WsState;

struct ZWebSocket {
    char host[128];
    int port;
    char path[512];
    int fd;
    WsState state;
    char *sendbuf;          // handshake request until it is flushed
    size_t send_len, send_off;
    char *ibuf;             // inbound frame accumulator
    size_t icap, ilen;
    ZWsMessageCb msg_cb;
    void *msg_ud;
};

static ZWebSocket *g_ws[Z_WS_MAX];

// ---------------------------------------------------------------------------
// URL parsing + non-blocking TCP connect (shared by HTTP and WS).
// ---------------------------------------------------------------------------
static bool parse_url(const char *url, const char *scheme, int def_port,
                      char *host, size_t hostn, int *port, char *path,
                      size_t pathn) {
    size_t sl = strlen(scheme);
    if (strncmp(url, scheme, sl) != 0) {
        return false;
    }
    const char *p = url + sl;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    size_t hlen = (size_t)((colon ? colon : hostend) - p);
    if (hlen == 0 || hlen + 1 > hostn) {
        return false;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    *port = colon ? atoi(colon + 1) : def_port;
    if (slash) {
        snprintf(path, pathn, "%s", slash);
    } else {
        snprintf(path, pathn, "/");
    }
    return true;
}

// Open a non-blocking TCP socket and start connecting. *in_progress is set true
// if the connect is still pending (poll for POLLOUT). Returns -1 on error.
static int tcp_connect_nb(const char *host, int port, bool *in_progress) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        return -1;   // numeric IPv4 only (DNS is Planned)
    }
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) {
        *in_progress = false;
        return fd;
    }
    if (errno == EINPROGRESS) {
        *in_progress = true;
        return fd;
    }
    close(fd);
    return -1;
}

// ---------------------------------------------------------------------------
// HTTP request lifecycle.
// ---------------------------------------------------------------------------
static bool add_active(ZNetRequest *r) {
    for (int i = 0; i < Z_NET_MAX; i++) {
        if (!g_active[i]) {
            g_active[i] = r;
            return true;
        }
    }
    return false;
}

static void destroy_request(ZNetRequest *r) {
    for (int i = 0; i < Z_NET_MAX; i++) {
        if (g_active[i] == r) {
            g_active[i] = NULL;
        }
    }
    if (r->fd >= 0) {
        close(r->fd);
    }
    free(r->sendbuf);
    free(r->rbuf);
    free(r->body);
    free(r);
}

// Build a ZNetResponse from the accumulated bytes, fire the callback, free both.
static void finish_ok(ZNetRequest *r) {
    ZNetResponse *res = calloc(1, sizeof(*res));
    if (res) {
        res->raw = r->rbuf;        // transfer ownership
        res->raw_len = r->rlen;
        r->rbuf = NULL;
        if (res->raw && strncmp(res->raw, "HTTP/", 5) == 0) {
            const char *sp = strchr(res->raw, ' ');
            if (sp) {
                res->status = atoi(sp + 1);
            }
        }
        res->ok = (res->status >= 200 && res->status < 300);
        char *split = res->raw ? strstr(res->raw, "\r\n\r\n") : NULL;
        if (split) {
            res->hdr_end = split;
            res->body = split + 4;
            res->body_len = res->raw_len - (size_t)(res->body - res->raw);
        } else {
            res->body = res->raw;
            res->body_len = res->raw_len;
        }
    }
    if (r->cb) {
        r->cb(res, r->ud);
    }
    if (res) {
        free(res->raw);
        free(res);
    }
    destroy_request(r);
}

// A transport error: fire the callback with a zeroed response (status 0).
static void finish_err(ZNetRequest *r) {
    ZNetResponse res = {0};
    if (r->cb) {
        r->cb(&res, r->ud);
    }
    destroy_request(r);
}

// Assemble the request bytes (request line + headers + optional body).
static void build_request(ZNetRequest *r) {
    char head[2560];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n",
                     r->method, r->path, r->host, r->hdrs);
    if (n < 0 || n >= (int)sizeof(head)) {
        n = 0;
    }
    if (r->body_len) {
        int m = snprintf(head + n, sizeof(head) - (size_t)n,
                         "Content-Length: %zu\r\n", r->body_len);
        if (m > 0 && n + m < (int)sizeof(head)) {
            n += m;
        }
    }
    int m = snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
    if (m > 0 && n + m < (int)sizeof(head)) {
        n += m;
    }
    size_t total = (size_t)n + r->body_len;
    r->sendbuf = malloc(total);
    if (!r->sendbuf) {
        return;
    }
    memcpy(r->sendbuf, head, (size_t)n);
    if (r->body_len) {
        memcpy(r->sendbuf + n, r->body, r->body_len);
    }
    r->send_len = total;
    r->send_off = 0;
}

// Permission is granted (cached or just consented): connect and start sending.
static void start_connect(ZNetRequest *r) {
    build_request(r);
    if (!r->sendbuf) {
        finish_err(r);
        return;
    }
    bool in_progress = false;
    r->fd = tcp_connect_nb(r->host, r->port, &in_progress);
    if (r->fd < 0) {
        finish_err(r);
        return;
    }
    r->state = in_progress ? NS_CONNECT : NS_SEND;
}

// Async permission outcome for a queued z_net_send.
static void net_perm_cb(ZApp *app, ZPermStatus status, void *ud) {
    (void)app;
    ZNetRequest *r = ud;
    if (status == Z_PERM_GRANTED) {
        start_connect(r);
    } else {
        finish_err(r);
    }
}

// Advance one HTTP request given its poll readiness.
static void net_advance(ZNetRequest *r) {
    if (r->state == NS_CONNECT) {
        int err = 0;
        socklen_t l = sizeof(err);
        getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) {
            finish_err(r);
            return;
        }
        r->state = NS_SEND;
    }
    if (r->state == NS_SEND) {
        while (r->send_off < r->send_len) {
            ssize_t w = write(r->fd, r->sendbuf + r->send_off,
                              r->send_len - r->send_off);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;   // wait for the next POLLOUT
                }
                finish_err(r);
                return;
            }
            r->send_off += (size_t)w;
        }
        r->state = NS_RECV;
        return;   // wait for POLLIN
    }
    if (r->state == NS_RECV) {
        for (;;) {
            if (r->rlen + 4097 > r->rcap) {
                size_t nc = r->rcap ? r->rcap * 2 : 8192;
                char *nb = realloc(r->rbuf, nc);
                if (!nb) {
                    finish_err(r);
                    return;
                }
                r->rbuf = nb;
                r->rcap = nc;
            }
            ssize_t rd = read(r->fd, r->rbuf + r->rlen, 4096);
            if (rd > 0) {
                r->rlen += (size_t)rd;
                r->rbuf[r->rlen] = '\0';
                continue;
            }
            if (rd == 0) {
                finish_ok(r);   // EOF: server closed -> response complete
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;   // more to come on a later POLLIN
            }
            finish_err(r);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Public HTTP API.
// ---------------------------------------------------------------------------
ZNetRequest *z_net_request(const char *method, const char *url) {
    if (!method || !url) {
        return NULL;
    }
    ZNetRequest *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->fd = -1;
    snprintf(r->method, sizeof(r->method), "%s", method);
    if (!parse_url(url, "http://", 80, r->host, sizeof(r->host), &r->port,
                   r->path, sizeof(r->path))) {
        free(r);
        return NULL;
    }
    return r;
}

ZNetRequest *z_net_get(const char *url) { return z_net_request("GET", url); }

void z_net_set_header(ZNetRequest *r, const char *k, const char *v) {
    if (!r || !k || !v) {
        return;
    }
    size_t cur = strlen(r->hdrs);
    snprintf(r->hdrs + cur, sizeof(r->hdrs) - cur, "%s: %s\r\n", k, v);
}

void z_net_set_body(ZNetRequest *r, const void *data, size_t len) {
    if (!r || !data || !len) {
        return;
    }
    free(r->body);
    r->body = malloc(len);
    if (!r->body) {
        r->body_len = 0;
        return;
    }
    memcpy(r->body, data, len);
    r->body_len = len;
}

void z_net_send(ZNetRequest *r, ZNetCallback cb, void *ud) {
    if (!r) {
        return;
    }
    r->cb = cb;
    r->ud = ud;
    if (!add_active(r)) {
        finish_err(r);   // too many in flight
        return;
    }
    // Airplane mode (P19): the brokered `sys.airplane` setting halts ALL network
    // before the permission check, so flipping Airplane in Settings makes every
    // request fail immediately (no consent prompt, no connect) — the airplane
    // toggle's real, observable actuation. Off again -> requests proceed as before.
    // Enforced here in the one path every libzelto net client (HTTP + WS) funnels
    // through; a fast in-memory broker read, like z_perm_status.
    if (z_setting_get_int("sys.airplane", 0) != 0) {
        // Logged for the same reason the dim scrim is (P45): "airplane gates the
        // network" is a claim about something NOT happening, and the only honest
        // way to test a negative is to make the refusal itself observable. An
        // absent connection is indistinguishable from a request never made.
        fprintf(stderr, "zelto: net: request refused (airplane mode)\n");
        fflush(stderr);
        finish_err(r);
        return;
    }
    // Gate on the network permission, exactly like any sensitive capability.
    ZPermStatus st = z_perm_status("network");
    if (st == Z_PERM_GRANTED) {
        start_connect(r);
    } else if (st == Z_PERM_DENIED) {
        finish_err(r);
    } else {
        // Undecided: show the consent dialog; resume from net_perm_cb on grant.
        r->state = NS_PERM_WAIT;
        z_perm_request("network", net_perm_cb, r);
    }
}

void z_net_cancel(ZNetRequest *r) {
    if (r) {
        destroy_request(r);   // no callback fires
    }
}

ZBytes z_net_body(ZNetResponse *res) {
    ZBytes b = {0};
    if (res && res->raw) {
        b.data = res->body;
        b.len = res->body_len;
        b.ok = true;
    }
    return b;
}

void z_net_headers(ZNetResponse *res, ZNetHeaderCb cb, void *ud) {
    if (!res || !res->raw || !cb) {
        return;
    }
    // Same walk as z_net_header, but handing every field to the callback instead
    // of matching one name — what a binding needs to materialise the whole header
    // set (a script's `res.headers`) without reading the response's internals.
    const char *end = res->hdr_end ? res->hdr_end : res->raw + res->raw_len;
    const char *line = strchr(res->raw, '\n');   // skip the status line
    line = line ? line + 1 : res->raw;
    while (line < end) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);
        const char *colon = memchr(line, ':', llen);
        if (colon) {
            char name[128], value[512];
            size_t nlen = (size_t)(colon - line);
            if (nlen >= sizeof(name)) {
                nlen = sizeof(name) - 1;
            }
            memcpy(name, line, nlen);
            name[nlen] = '\0';

            const char *v = colon + 1;
            while (v < line + llen && (*v == ' ' || *v == '\t')) {
                v++;
            }
            size_t vlen = (size_t)(line + llen - v);
            while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) {
                vlen--;
            }
            if (vlen >= sizeof(value)) {
                vlen = sizeof(value) - 1;
            }
            memcpy(value, v, vlen);
            value[vlen] = '\0';

            cb(name, value, ud);
        }
        if (!nl) {
            break;
        }
        line = nl + 1;
    }
}

const char *z_net_header(ZNetResponse *res, const char *name) {
    if (!res || !res->raw || !name) {
        return NULL;
    }
    // Search the header block [first line .. hdr_end) for "name:" (case-insens).
    const char *end = res->hdr_end ? res->hdr_end : res->raw + res->raw_len;
    const char *line = strchr(res->raw, '\n');
    line = line ? line + 1 : res->raw;
    size_t nlen = strlen(name);
    while (line < end) {
        const char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t llen = nl ? (size_t)(nl - line) : (size_t)(end - line);
        if (llen > nlen && line[nlen] == ':' &&
            strncasecmp(line, name, nlen) == 0) {
            const char *v = line + nlen + 1;
            while (v < line + llen && (*v == ' ' || *v == '\t')) {
                v++;
            }
            size_t vlen = (size_t)(line + llen - v);
            while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) {
                vlen--;
            }
            if (vlen >= sizeof(res->scratch)) {
                vlen = sizeof(res->scratch) - 1;
            }
            memcpy(res->scratch, v, vlen);
            res->scratch[vlen] = '\0';
            return res->scratch;
        }
        if (!nl) {
            break;
        }
        line = nl + 1;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// WebSocket client (unfragmented text frames).
// ---------------------------------------------------------------------------
static bool ws_add(ZWebSocket *w) {
    for (int i = 0; i < Z_WS_MAX; i++) {
        if (!g_ws[i]) {
            g_ws[i] = w;
            return true;
        }
    }
    return false;
}

static void ws_destroy(ZWebSocket *w) {
    for (int i = 0; i < Z_WS_MAX; i++) {
        if (g_ws[i] == w) {
            g_ws[i] = NULL;
        }
    }
    if (w->fd >= 0) {
        close(w->fd);
    }
    free(w->sendbuf);
    free(w->ibuf);
    free(w);
}

ZWebSocket *z_ws_open(const char *url) {
    if (!url) {
        return NULL;
    }
    // Airplane mode halts the network (P19) — same gate as z_net_send.
    if (z_setting_get_int("sys.airplane", 0) != 0) {
        return NULL;
    }
    // Best-effort permission gate: deny only on a cached denial. By the time an
    // app opens a socket it has usually already been granted `network` (e.g. via
    // a prior z_net_send), so PROMPT proceeds rather than blocking the handle.
    if (z_perm_status("network") == Z_PERM_DENIED) {
        return NULL;
    }
    ZWebSocket *w = calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->fd = -1;
    if (!parse_url(url, "ws://", 80, w->host, sizeof(w->host), &w->port, w->path,
                   sizeof(w->path)) ||
        !ws_add(w)) {
        free(w);
        return NULL;
    }
    bool in_progress = false;
    w->fd = tcp_connect_nb(w->host, w->port, &in_progress);
    if (w->fd < 0) {
        ws_destroy(w);
        return NULL;
    }
    // RFC6455 handshake. The Sec-WebSocket-Key is the canonical example nonce;
    // we do not verify the server's Accept (MVP).
    char hs[1024];
    int n = snprintf(hs, sizeof(hs),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     w->path, w->host);
    if (n <= 0 || n >= (int)sizeof(hs)) {
        ws_destroy(w);
        return NULL;
    }
    w->sendbuf = malloc((size_t)n);
    if (!w->sendbuf) {
        ws_destroy(w);
        return NULL;
    }
    memcpy(w->sendbuf, hs, (size_t)n);
    w->send_len = (size_t)n;
    w->state = in_progress ? WS_CONNECT : WS_HS;
    return w;
}

void z_ws_on_message(ZWebSocket *ws, ZWsMessageCb cb, void *ud) {
    if (ws) {
        ws->msg_cb = cb;
        ws->msg_ud = ud;
    }
}

// Write one masked frame with the given opcode (client frames MUST be masked).
static void ws_write_frame(ZWebSocket *ws, uint8_t opcode, const void *data,
                           size_t len) {
    if (ws->fd < 0) {
        return;
    }
    uint8_t hdr[14];
    size_t hn = 0;
    hdr[hn++] = (uint8_t)(0x80 | opcode);   // FIN + opcode
    if (len < 126) {
        hdr[hn++] = (uint8_t)(0x80 | len);
    } else {
        hdr[hn++] = 0x80 | 126;
        hdr[hn++] = (uint8_t)((len >> 8) & 0xff);
        hdr[hn++] = (uint8_t)(len & 0xff);
    }
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(hdr + hn, mask, 4);
    hn += 4;
    ssize_t w = write(ws->fd, hdr, hn);
    (void)w;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)(p[i] ^ mask[i & 3]);
        w = write(ws->fd, &b, 1);
        (void)w;
    }
}

void z_ws_send(ZWebSocket *ws, const void *data, size_t len) {
    if (ws && ws->state == WS_OPEN) {
        ws_write_frame(ws, 0x1, data, len);   // text
    }
}

void z_ws_close(ZWebSocket *ws) {
    if (!ws) {
        return;
    }
    if (ws->state == WS_OPEN) {
        ws_write_frame(ws, 0x8, NULL, 0);     // close
    }
    ws_destroy(ws);
}

// Parse and dispatch every complete server frame buffered in ws->ibuf.
static void ws_parse_frames(ZWebSocket *ws) {
    size_t off = 0;
    while (ws->ilen - off >= 2) {
        uint8_t *f = (uint8_t *)ws->ibuf + off;
        uint8_t opcode = f[0] & 0x0f;
        bool masked = (f[1] & 0x80) != 0;
        uint64_t plen = f[1] & 0x7f;
        size_t hl = 2;
        if (plen == 126) {
            if (ws->ilen - off < 4) {
                break;
            }
            plen = ((uint64_t)f[2] << 8) | f[3];
            hl = 4;
        } else if (plen == 127) {
            break;   // 64-bit lengths unsupported (MVP)
        }
        size_t total = hl + (masked ? 4 : 0) + (size_t)plen;
        if (ws->ilen - off < total) {
            break;   // incomplete frame
        }
        uint8_t *payload = f + hl + (masked ? 4 : 0);
        if (opcode == 0x8) {   // close
            ws->state = WS_DEAD;
            off += total;
            break;
        }
        if (opcode == 0x1 && ws->msg_cb) {   // text
            char saved = (char)payload[plen];
            payload[plen] = '\0';
            ws->msg_cb(ws, (const char *)payload, (size_t)plen, ws->msg_ud);
            payload[plen] = saved;
        }
        off += total;
    }
    // Shift any partial trailing frame to the front.
    if (off) {
        memmove(ws->ibuf, ws->ibuf + off, ws->ilen - off);
        ws->ilen -= off;
    }
}

static void ws_advance(ZWebSocket *ws) {
    if (ws->state == WS_CONNECT) {
        int err = 0;
        socklen_t l = sizeof(err);
        getsockopt(ws->fd, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) {
            ws->state = WS_DEAD;
            return;
        }
        while (ws->send_off < ws->send_len) {
            ssize_t wr = write(ws->fd, ws->sendbuf + ws->send_off,
                               ws->send_len - ws->send_off);
            if (wr < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                ws->state = WS_DEAD;
                return;
            }
            ws->send_off += (size_t)wr;
        }
        ws->state = WS_HS;
        return;
    }
    // Both WS_HS and WS_OPEN read inbound bytes; grow + accumulate.
    for (;;) {
        if (ws->ilen + 4097 > ws->icap) {
            size_t nc = ws->icap ? ws->icap * 2 : 8192;
            char *nb = realloc(ws->ibuf, nc);
            if (!nb) {
                ws->state = WS_DEAD;
                return;
            }
            ws->ibuf = nb;
            ws->icap = nc;
        }
        ssize_t rd = read(ws->fd, ws->ibuf + ws->ilen, 4096);
        if (rd > 0) {
            ws->ilen += (size_t)rd;
            continue;
        }
        if (rd == 0) {
            ws->state = WS_DEAD;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        ws->state = WS_DEAD;
        return;
    }
    if (ws->state == WS_HS) {
        ws->ibuf[ws->ilen < ws->icap ? ws->ilen : ws->icap - 1] = '\0';
        char *split = mem_find(ws->ibuf, ws->ilen, "\r\n\r\n", 4);
        if (split) {
            // 101 Switching Protocols expected; accept any 1xx-2xx upgrade.
            bool ok = (ws->ilen >= 12 && memcmp(ws->ibuf, "HTTP/1.1 101", 12) == 0);
            size_t consumed = (size_t)(split + 4 - ws->ibuf);
            memmove(ws->ibuf, ws->ibuf + consumed, ws->ilen - consumed);
            ws->ilen -= consumed;
            ws->state = ok ? WS_OPEN : WS_DEAD;
            if (ws->state == WS_OPEN) {
                ws_parse_frames(ws);
            }
        }
        return;
    }
    ws_parse_frames(ws);
}

// A tiny memmem (avoid relying on the GNU extension's availability here).
static char *mem_find(const char *hay, size_t hn, const char *needle,
                      size_t nn) {
    if (nn == 0 || hn < nn) {
        return NULL;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (memcmp(hay + i, needle, nn) == 0) {
            return (char *)hay + i;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// App-loop integration (called from app.c's poll loop).
// ---------------------------------------------------------------------------
int z_net_collect_fds(struct pollfd *pfds, int max) {
    int n = 0;
    for (int i = 0; i < Z_NET_MAX && n < max; i++) {
        ZNetRequest *r = g_active[i];
        if (!r || r->fd < 0) {
            continue;
        }
        short ev = (r->state == NS_RECV) ? POLLIN : POLLOUT;
        pfds[n].fd = r->fd;
        pfds[n].events = ev;
        pfds[n].revents = 0;
        n++;
    }
    for (int i = 0; i < Z_WS_MAX && n < max; i++) {
        ZWebSocket *w = g_ws[i];
        if (!w || w->fd < 0 || w->state == WS_DEAD) {
            continue;
        }
        short ev = (w->state == WS_CONNECT) ? POLLOUT : POLLIN;
        pfds[n].fd = w->fd;
        pfds[n].events = ev;
        pfds[n].revents = 0;
        n++;
    }
    return n;
}

void z_net_handle_ready(struct pollfd *pfds, int count) {
    for (int i = 0; i < count; i++) {
        if (!(pfds[i].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) {
            continue;
        }
        int fd = pfds[i].fd;
        // Match against the HTTP table first, then the WS table. Either advance
        // may free the entry; we look it up fresh by fd so a stale index is safe.
        bool handled = false;
        for (int j = 0; j < Z_NET_MAX; j++) {
            if (g_active[j] && g_active[j]->fd == fd) {
                net_advance(g_active[j]);
                handled = true;
                break;
            }
        }
        if (handled) {
            continue;
        }
        for (int j = 0; j < Z_WS_MAX; j++) {
            if (g_ws[j] && g_ws[j]->fd == fd) {
                ws_advance(g_ws[j]);
                if (g_ws[j] && g_ws[j]->state == WS_DEAD) {
                    ws_destroy(g_ws[j]);
                }
                break;
            }
        }
    }
}
