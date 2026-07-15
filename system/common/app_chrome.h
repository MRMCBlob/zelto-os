// The shared chrome every Zelto app screen is built from.
//
// Before this, each app invented its own: one centred its title in the middle of
// the screen, another left it at body size, a third centred its whole content
// column vertically so the page rearranged itself as content arrived. The result
// was that no two apps looked like they came from the same system — which is the
// thing the design language exists to prevent (Law of Similarity: consistent
// surfaces read as one product).
//
// A screen here is: a LARGE TITLE at the top left, content below it, both hard
// against the top of the page. That is the phone convention, and it means the
// title is where the eye lands first and content grows downward from a fixed
// anchor rather than sliding around it.
#ifndef ZELTO_APP_CHROME_H
#define ZELTO_APP_CHROME_H

#include <zelto/ui.h>

// The screen's title. Large and bold — a screen announces itself once, loudly,
// and then gets out of the way.
static inline ZView z_screen_title(const char *text) {
    return Weight(Z_WEIGHT_BOLD,
        Foreground(Z_COLOR_TEXT, Font(Z_FONT_LARGE_TITLE, z_text("%s", text))));
}

// The label above a group of rows/cards.
static inline ZView z_section(const char *text) {
    return Weight(Z_WEIGHT_SEMIBOLD,
        Foreground(Z_COLOR_TEXT_MUTED,
            Font(Z_FONT_FOOTNOTE, z_text("%s", text))));
}

// A content card: the standard raised, rounded surface a block of app content
// sits on. One radius, one fill, one elevation, everywhere.
static inline ZView z_card(ZView content) {
    return Shadow(Z_ELEV_1,
        Background(Z_COLOR_SURFACE,
            CornerRadius(Z_RADIUS_CARD, Padding(16.0f, content))));
}

// A titled content card: a muted caption over its value. The caption is SMALL and
// the value is large — the card is read for the value.
static inline ZView z_stat_card(const char *caption, const char *value) {
    return z_card(
        VStack(
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CAPTION2, z_text("%s", caption))),
            Weight(Z_WEIGHT_SEMIBOLD,
                Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY,
                    z_text("%s", value)))),
            .spacing = 4, .align = Z_ALIGN_LEADING));
}

#endif  // ZELTO_APP_CHROME_H
