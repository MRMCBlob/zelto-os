// Zelto System UI — notification shade.
//
// The sink for posted notifications. An ordinary libzelto OVERLAY layer-shell
// app anchored to the top edge (a heads-up banner strip above the apps), it
// subscribes to zsysd as the single notification sink (z_notify_subscribe) and
// renders one banner card per active notification: the poster's app id, the
// title, the body, and — when present — an action button. Unlike the consent /
// chooser overlays it does NOT grab the keyboard (a passive heads-up surface
// must not steal focus from the foreground app); pointer taps still reach it
// because the overlay composites on top.
//
// Tapping a card body opens its tap_route deep link itself (z_open_url, the same
// P9 intents client every app has) and reports the tap so zsysd drops the
// banner. Tapping an action reports it so zsysd routes it back to the poster's
// z_on_notification_action, then drops the banner. A full pull-down shade with
// history/gestures is Planned — heads-up banners are the MVP.
// See docs/guides/notifications.md + docs/contributing/services-and-ipc.md.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zelto/ui.h>

#define MAX_BANNERS 8

// One active banner: a copy of the pushed notification (the ZShownNotification
// strings are valid only for the duration of the show callback, so copy them).
typedef struct Banner {
    bool used;
    int64_t id;
    char app_id[96];
    char title[128];
    char body[192];
    char tap_route[256];
    char action_id[64];
    char action_title[64];
} Banner;

typedef struct ShadeState {
    bool subscribed;
    Banner banners[MAX_BANNERS];
} ShadeState;

// zsysd pushed a new notification: store it (replacing an existing one with the
// same id) and repaint.
static void on_show(ZApp *app, const ZShownNotification *n, void *ud) {
    (void)app;
    ShadeState *s = ud;
    Banner *b = NULL;
    for (int i = 0; i < MAX_BANNERS; i++) {
        if (s->banners[i].used && s->banners[i].id == n->id) {
            b = &s->banners[i];
            break;
        }
    }
    if (!b) {
        for (int i = 0; i < MAX_BANNERS; i++) {
            if (!s->banners[i].used) {
                b = &s->banners[i];
                break;
            }
        }
    }
    if (!b) {
        return;   // strip full (heads-up MVP keeps only a handful)
    }
    b->used = true;
    b->id = n->id;
    snprintf(b->app_id, sizeof(b->app_id), "%s", n->app_id ? n->app_id : "");
    snprintf(b->title, sizeof(b->title), "%s", n->title ? n->title : "");
    snprintf(b->body, sizeof(b->body), "%s", n->body ? n->body : "");
    snprintf(b->tap_route, sizeof(b->tap_route), "%s",
             n->tap_route ? n->tap_route : "");
    snprintf(b->action_id, sizeof(b->action_id), "%s",
             n->action_id ? n->action_id : "");
    snprintf(b->action_title, sizeof(b->action_title), "%s",
             n->action_title ? n->action_title : "");
}

// zsysd dropped a notification (cancel / tap / action): remove its banner.
static void on_hide(ZApp *app, int64_t id, void *ud) {
    (void)app;
    ShadeState *s = ud;
    for (int i = 0; i < MAX_BANNERS; i++) {
        if (s->banners[i].used && s->banners[i].id == id) {
            s->banners[i].used = false;
        }
    }
}

// Body tap: route the deep link ourselves (P9 intents) and tell zsysd to drop
// the banner. The data pointer is the stable Banner this card stands for.
static void tap_body(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    Banner *b = data;
    if (b->tap_route[0]) {
        z_open_url(b->tap_route);
    }
    z_notify_report_tap(b->id);
}

// Action tap: report it so zsysd routes it to the poster's mailbox, then drops
// the banner.
static void tap_action(ZApp *app, void *state, void *data) {
    (void)app;
    (void)state;
    Banner *b = data;
    z_notify_report_action(b->id, b->action_id);
}

// One banner card: app id caption + title + body and, when the notification
// carries one, an action button. The WHOLE card is the body-tap target; the
// action button is a deeper tap target nested inside it, so hit-testing (deepest
// handler wins) routes an action tap to the action and any other tap to the
// body. Full-width like a tile.
static ZView banner_card(Banner *b) {
    ZStackOpts row = {.padding = 16, .spacing = 16, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] =
        VStack(
            Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
                Font(Z_FONT_CAPTION, Text("%s", b->app_id))),
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_CALLOUT, Text("%s", b->title))),
            Foreground(z_rgba(0xc6, 0xcf, 0xd8, 0xff),
                Text("%s", b->body)),
            .spacing = 4, .align = Z_ALIGN_LEADING);
    row.children[k++] = Spacer();
    if (b->action_id[0]) {
        row.children[k++] = OnTapData(tap_action, b,
            Background(z_rgba(0x2e, 0x9b, 0xff, 0xff),
                CornerRadius(12,
                    Padding(14,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_CALLOUT,
                                Text("%s", b->action_title)))))));
    }

    return OnTapData(tap_body, b,
        Background(z_rgba(0x1a, 0x20, 0x28, 0xff),
            CornerRadius(16, z_stack(Z_AXIS_HORIZONTAL, &row))));
}

static ZView shade_body(ZApp *app, ShadeState *s) {
    // Subscribe as the notification sink on the first build (the ctrl_fd exists
    // by now — it is opened before the loop in app_run).
    if (!s->subscribed) {
        s->subscribed = true;
        z_notify_subscribe(app, on_show, on_hide, s);
    }

    ZStackOpts col = {.padding = 8, .spacing = 8, .align = Z_ALIGN_LEADING};
    int k = 0;
    for (int i = 0; i < MAX_BANNERS && k < Z_MAX_CHILDREN - 1; i++) {
        if (s->banners[i].used) {
            col.children[k++] = banner_card(&s->banners[i]);
        }
    }

    // No active banners: a fully transparent strip (nothing visible). Active
    // banners: the cards over a transparent backdrop so apps show through the
    // gaps. (The strip itself still occupies the top of the screen — a
    // pull-down/dynamic-height shade is Planned.)
    if (k == 0) {
        return Background(z_rgba(0, 0, 0, 0), Spacer());
    }
    col.children[k++] = Spacer();
    return Background(z_rgba(0, 0, 0, 0), z_stack(Z_AXIS_VERTICAL, &col));
}

// OVERLAY layer, anchored top across the width and floated below the 40px status
// bar (margin_top) so the bar stays visible, NOT keyboard-exclusive (a passive
// heads-up surface). A fixed-height strip; because the software renderer can
// only produce opaque surfaces the strip is opaque (it covers the top of the
// app while shown). A truly transparent / dynamic-height pull-down shade is
// Planned.
Z_LAYER_APP(ShadeState, shade_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .height = 150,
            .margin_top = 40,
            .keyboard = false)
