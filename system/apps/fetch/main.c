// Zelto demo app — "Fetch" (the networking demo, P12).
//
// Exercises the libzelto networking client from the app side. It declares
// `permissions=network` in its manifest, so the first z_net_send with no stored
// grant shows the System-UI consent dialog (the P8 modal); tapping Allow lets
// the request connect, and when the response arrives the callback fires on the
// app loop and the fetched body is rendered. A second tap reuses the cached
// grant — no modal.
//
// The request targets http://10.0.2.2:<port>/hello.txt — under QEMU user-mode
// networking 10.0.2.2 is the host, where the NET=1 harness runs a tiny HTTP
// server. Maps a plain xdg_toplevel below the bar.
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

#define FETCH_URL "http://10.0.2.2:8080/hello.txt"

typedef struct FetchState {
    ZApp *app;            // captured each build so the net callback can repaint
    int requests;         // how many fetches we've kicked off
    int status;           // last HTTP status (0 = transport error / none yet)
    bool pending;         // a request is in flight
    bool done;            // a response has come back
    bool error;           // the last result was an error
    char body[640];       // the fetched body (or a status line)
} FetchState;

// The response (or error) came back on the app loop — record it and repaint.
static void on_resp(ZNetResponse *res, void *ud) {
    FetchState *s = ud;
    s->pending = false;
    s->done = true;
    s->status = res ? res->status : 0;
    if (res && res->ok) {
        ZBytes b = z_net_body(res);
        size_t n = b.len < sizeof(s->body) - 1 ? b.len : sizeof(s->body) - 1;
        memcpy(s->body, b.data ? b.data : "", n);
        while (n && (s->body[n - 1] == '\n' || s->body[n - 1] == '\r')) {
            n--;   // drop the trailing newline (renders as a tofu glyph)
        }
        s->body[n] = '\0';
        s->error = false;
    } else {
        snprintf(s->body, sizeof(s->body), "request failed (status %d)",
                 s->status);
        s->error = true;
    }
    if (s->app) {
        z_invalidate(s->app);
    }
}

// Kick off an async GET. The first send (no grant) shows the consent modal.
static void do_fetch(ZApp *app, void *state) {
    FetchState *s = state;
    s->requests++;
    s->pending = true;
    s->done = false;
    s->error = false;
    snprintf(s->body, sizeof(s->body), "requesting %s ...", FETCH_URL);
    ZNetRequest *r = z_net_get(FETCH_URL);
    z_net_send(r, on_resp, s);   // r may be NULL on a bad URL; z_net_send no-ops
    z_invalidate(app);
}

static ZView fetch_body(ZApp *app, FetchState *state) {
    state->app = app;   // capture for the off-loop net callback

    ZColor panel_bg = state->error  ? Z_COLOR_DANGER_DIM
                      : state->done ? Z_COLOR_ACCENT_DIM
                                    : Z_COLOR_SURFACE_2;

    const char *status_line =
        state->pending ? "fetching..."
        : state->done  ? (state->error ? "error" : "200 OK")
                       : "idle";

    return Background(Z_COLOR_BG,
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Fetch"))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CALLOUT, Text("HTTP GET over the network permission"))),
            Spacer(),
            Background(Z_COLOR_PRIMARY,
                Button(do_fetch, "Fetch")),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION,
                     Text("requests %d   status: %s", state->requests,
                          status_line))),
            Background(panel_bg,
                CornerRadius(14,
                    Frame(560.0f, 160.0f,
                        VStack(
                            Foreground(Z_COLOR_TEXT_MUTED,
                                Font(Z_FONT_CAPTION, Text("response body"))),
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_BODY,
                                    Text("%s", state->done || state->pending
                                                   ? state->body
                                                   : "(tap Fetch)"))),
                            .spacing = 8, .align = Z_ALIGN_LEADING,
                            .padding = 18)))),
            Spacer(),
            .padding = 32, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(FetchState, fetch_body, "os.zelto.fetch")
