// Zelto demo app — "Pinger" (the notification source).
//
// Exercises the notifications broker (P10) from the posting side. It declares
// `permissions=notifications` in its manifest, so the first post with no stored
// grant shows the System-UI consent dialog (the P8 modal); tapping Allow lets
// zsysd assign an id, store the notification, and push it to the shade as a
// heads-up banner.
//
// The posted notification carries a body, a tap_route deep link
// (zelto://note/7, handled by the Notes app) opened when the banner body is
// tapped, and one action button ("Ack"). Tapping the action routes back here via
// z_on_notification_action, and the UI shows the last tapped action — proof the
// round-trip closed. Maps a plain xdg_toplevel below the bar.
#include <stdio.h>

#include <zelto/ui.h>

typedef struct PingerState {
    int posts;              // how many notifications we've posted
    bool got_action;        // has an action come back?
    char last_action[64];   // the last action id tapped in the shade
} PingerState;

// Post a heads-up notification: body + tap_route deep link + one action button.
static void post_ping(ZApp *app, void *state) {
    PingerState *s = state;
    ZNotification *n = z_notify_new("Ping", "You have a new ping");
    z_notify_set_channel(n, "pings");
    z_notify_set_tap_route(n, "zelto://note/7");
    z_notify_add_action(n, "ack", "Ack");
    z_notify_post(n);   // synchronous; may show the consent dialog first
    s->posts++;
    z_invalidate(app);
}

// An action tapped on our banner came back through the broker: record it.
static void on_action(ZApp *app, const ZNotifyActionEvent *e, void *ud) {
    PingerState *s = ud;
    snprintf(s->last_action, sizeof(s->last_action), "%s",
             e->action_id ? e->action_id : "(body)");
    s->got_action = true;
    z_invalidate(app);
}

static ZView pinger_body(ZApp *app, PingerState *state) {
    // Register the action receiver on the first build (libzelto buffers any
    // action delivered before this runs, as for intents).
    z_on_notification_action(app, on_action, state);

    ZColor panel_bg = state->got_action ? Z_COLOR_SUCCESS_DIM
                                         : Z_COLOR_SURFACE_2;

    return Background(Z_COLOR_BG,
        VStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("Pinger"))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CALLOUT, Text("Posts heads-up notifications"))),
            Spacer(),
            Background(Z_COLOR_PRIMARY,
                Button(post_ping, "Post")),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION, Text("posted %d", state->posts))),
            Background(panel_bg,
                CornerRadius(14,
                    Frame(360.0f, 80.0f,
                        VStack(
                            Foreground(Z_COLOR_TEXT_MUTED,
                                Font(Z_FONT_CAPTION, Text("last action"))),
                            Foreground(Z_COLOR_TEXT_INV,
                                Font(Z_FONT_CALLOUT,
                                    Text("%s", state->got_action
                                                   ? state->last_action
                                                   : "(none yet)"))),
                            .spacing = 6, .align = Z_ALIGN_LEADING,
                            .padding = 16)))),
            Spacer(),
            .padding = 32, .spacing = 18, .align = Z_ALIGN_CENTER));
}

Z_APP_ID(PingerState, pinger_body, "os.zelto.pinger")
