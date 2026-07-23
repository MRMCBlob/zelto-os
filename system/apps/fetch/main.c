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
#include <stdlib.h>
#include <string.h>

#include <zelto/ui.h>

#define FETCH_URL "http://10.0.2.2:8080/hello.txt"

// The screen's own margin. Named because the prose below has to wrap to the
// column it leaves, and a wrap width that repeats a padding literal is a wrap
// width that goes stale the day the padding moves.
#define FETCH_PAD 32.0f
#define PANEL_PAD 18.0f

typedef struct FetchState {
    ZApp *app;            // captured each build so the net callback can repaint
    int requests;         // how many fetches we've kicked off
    int status;           // last HTTP status (0 = transport error / none yet)
    bool pending;         // a request is in flight
    bool done;            // a response has come back
    bool error;           // the last result was an error
    bool armed;           // ZELTO_FETCH_AUTO timer scheduled once
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

    // Headless test hook (P45): ZELTO_FETCH_AUTO=1 issues the GET on its own a
    // beat after the first build, so a harness can test whether the request is
    // GATED without tapping a button. The ACTUATE harness needs exactly this —
    // its predecessor tapped "Fetch" at coordinates measured off a screenshot,
    // which is the habit that rotted five harnesses into unconditional passes.
    // Same shape as ZELTO_CONSENT_AUTO in the consent dialog.
    if (!state->armed) {
        state->armed = true;
        const char *au = getenv("ZELTO_FETCH_AUTO");
        if (au && au[0] && au[0] != '0') {
            z_after(app, 600, do_fetch, state);
        }
    }

    ZColor panel_bg = state->error  ? Z_COLOR_DANGER_DIM
                      : state->done ? Z_COLOR_ACCENT_DIM
                                    : Z_COLOR_SURFACE_2;

    const char *status_line =
        state->pending ? "fetching..."
        : state->done  ? (state->error ? "error" : "200 OK")
                       : "idle";
    char status[96];
    snprintf(status, sizeof(status), "requests %d   status: %s",
             state->requests, status_line);

    // The app's text column, and the panel's height as an expression over the
    // two lines inside it rather than a measured-once total (P50's ALERT_H).
    float col_w = (float)z_app_width(app) - 2.0f * FETCH_PAD;
    float panel_h = z_line_height(app, Z_FONT_CAPTION) +
                    z_line_height(app, Z_FONT_BODY) + 8.0f +
                    2.0f * PANEL_PAD;
    if (panel_h < 160.0f) {
        panel_h = 160.0f;
    }

    return Background(Z_COLOR_BG,
        VStack(
            Spacer(),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Fetch"))),
            // WRAPPED, and it was overflowing BEFORE Dynamic Type existed: this
            // sentence measures 702 units at the default text size in a 656-unit
            // column, so ~46 units of it have been painting past the app's own
            // margin since P43 rescaled the type. Nothing looked broken because
            // the surface behind it is the same flat background. The P50 audit
            // found it by booting every surface at the largest size — where it
            // wants 910 — and the default-size control is what made it a
            // pre-existing bug rather than a new one.
            Foreground(Z_COLOR_TEXT_MUTED,
                WrapText(app, "HTTP GET over the network permission",
                         .width = (float)z_app_width(app) - 2.0f * FETCH_PAD,
                         .size = Z_FONT_CALLOUT)),
            Spacer(),
            Background(Z_COLOR_PRIMARY,
                Button(do_fetch, "Fetch")),
            // The status read-out is one line of two facts, and at the
            // accessibility sizes it wants 887 units in a 656 column. It is not
            // an identifier that could be cut short — the interesting half is at
            // the END — so it wraps.
            Foreground(Z_COLOR_TEXT_MUTED,
                WrapText(app, status, .width = col_w, .size = Z_FONT_CAPTION)),
            // THE PANEL WAS 560x160, BOTH LITERALS, AND BOTH ARE FIXED BOXES
            // HOLDING TEXT. Its own caption 'response body' wants 567 units at
            // AX5, so the box was narrower than the label naming it. The width is
            // the app's column now, and the height is an expression over the two
            // lines it holds plus the padding it holds them in — floored at the
            // 160 it used to be so an ordinary-sized device sees no change.
            Background(panel_bg,
                CornerRadius(14,
                    Frame(col_w, panel_h,
                        VStack(
                            Foreground(Z_COLOR_TEXT_MUTED,
                                Font(Z_FONT_CAPTION, Text("response body"))),
                            // The BODY is a string off the network in a box this
                            // app chose: the P46 answer to that is a measured cut,
                            // not a wrap — a response wrapped over ten lines is a
                            // panel that has eaten the screen.
                            Foreground(Z_COLOR_TEXT_INV,
                                EllipsizeText(app,
                                    state->done || state->pending
                                        ? state->body
                                        : "(tap Fetch)",
                                    .width = col_w - 2.0f * PANEL_PAD,
                                    .size = Z_FONT_BODY)),
                            .spacing = 8, .align = Z_ALIGN_LEADING,
                            .padding = PANEL_PAD)))),
            Spacer(),
            .padding = FETCH_PAD, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(FetchState, fetch_body, "os.zelto.fetch")
