// samples/hello - the smallest native Zelto app: a static declarative view tree
// (background + stacked/flexed rectangles + a shaped line of text) rendered by
// libzelto and composited by zcomp. See docs/getting-started/hello-world.md.
#include <zelto/ui.h>

// App state. Static for now; rebuilds happen via z_invalidate (Planned UI).
typedef struct HelloState {
    int unused;
} HelloState;

static ZView body(ZApp *app, HelloState *state) {
    (void)app;
    (void)state;

    return Background(z_rgba(0x12, 0x16, 0x1c, 0xff),
        VStack(
            // One shaped line of text (HarfBuzz + FreeType).
            Font(Z_FONT_TITLE,
                 Foreground(Z_COLOR_TEXT_INV, Text("Hello, Zelto OS"))),

            // A flex row: three rectangles sharing the width 1:2:1.
            Frame(0, 140,
                HStack(
                    Grow(1, Rect(.color = Z_COLOR_PRIMARY, .radius = 18)),
                    Grow(2, Rect(.color = Z_COLOR_ACCENT, .radius = 18)),
                    Grow(1, Rect(.color = Z_COLOR_SURFACE, .radius = 18)),
                    .spacing = 16)),

            // A couple of stacked fixed rectangles.
            Rect(.color = z_rgba(0x0a, 0x84, 0x8c, 0xff),
                 .width = 280, .height = 88, .radius = 14),
            Rect(.color = z_rgba(0x3a, 0x44, 0x52, 0xff),
                 .width = 200, .height = 56, .radius = 12),

            // Push the caption to the bottom.
            Spacer(),
            Foreground(z_rgba(0x9a, 0xa4, 0xad, 0xff),
                       Text("libzelto - static view tree on screen")),

            .padding = 48, .spacing = 28, .align = Z_ALIGN_CENTER));
}

Z_APP(HelloState, body)
