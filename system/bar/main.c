// Zelto System UI — status bar.
//
// A wlr-layer-shell client anchored to the top edge in the TOP layer with an
// exclusive zone, so the compositor keeps the strip below it clear for apps.
// Same libzelto declarative loop as any app; only the surface role differs
// (Z_LAYER_APP). Shows a title on the left and a clock + indicator dots on the
// right. See docs/contributing/compositor-internals.md ("Surfaces & layers").
#include <stdio.h>
#include <time.h>

#include <zelto/ui.h>

#define BAR_H 40

typedef struct BarState {
    int unused;
} BarState;

// A small filled status dot (battery / signal indicator placeholder).
static ZView dot(ZColor c) {
    return Frame(10.0f, 10.0f, Rect(.color = c, .radius = 5));
}

static ZView bar_body(ZApp *app, BarState *state) {
    (void)state;
    (void)app;

    // Clock from wall time (updates whenever the bar rebuilds).
    char clock[8] = "--:--";
    time_t t = time(NULL);
    struct tm tmv;
    if (gmtime_r(&t, &tmv)) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }

    return Background(z_rgba(0x10, 0x14, 0x1a, 0xff),
        HStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_BODY, Text("Zelto"))),
            Spacer(),
            dot(z_rgba(0x3d, 0xc7, 0x8c, 0xff)),     // green
            dot(z_rgba(0x2e, 0x9b, 0xff, 0xff)),     // blue
            Foreground(Z_COLOR_TEXT_INV, Text("%s", clock)),
            .padding = 12, .spacing = 10, .align = Z_ALIGN_CENTER));
}

Z_LAYER_APP(BarState, bar_body,
            .layer = Z_LAYER_TOP,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT | Z_ANCHOR_RIGHT,
            .exclusive_zone = BAR_H,
            .height = BAR_H)
