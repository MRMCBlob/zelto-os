// The shared notification card.
//
// A notification is ONE object that appears in three places: as a heads-up
// banner over whatever app is in front, as a row in the Notification Center, and
// as a card on the lock screen. If those are three separate pieces of layout code
// they drift — a different corner radius here, the reverse-DNS app id instead of
// the display name there — and the user stops recognising the banner they just
// swiped away as the card now sitting on their lock screen. So the mark lives
// here, once, and every surface that shows a notification calls this.
//
// The card is a MATERIAL, not a solid: it floats over the blurred wallpaper the
// way an iOS notification does rather than sitting on a panel. `interactive`
// distinguishes a live notification from a spent one (the shade's recently-
// dismissed history): a spent card is dimmer, flatter, and carries no handlers.
//
// Tap handling is the CALLER's: this returns the card's visual body, and the
// caller wraps it in whatever OnTapData its own state needs. `trailing` is an
// optional action button (NULL for none) built by the caller for the same reason.
#ifndef ZELTO_SYSTEM_COMMON_NOTIF_CARD_H
#define ZELTO_SYSTEM_COMMON_NOTIF_CARD_H

#include <stdbool.h>
#include <stdio.h>

#include <zelto/ui.h>

#include "common/app_icons.h"

#define ZELTO_NOTIF_ICON 38.0f
#define ZELTO_NOTIF_PAD 14.0f
#define ZELTO_NOTIF_GAP 14.0f
// A trailing action button is given a FIXED width, which is what makes the text
// column beside it knowable. WrapText has to be told its column when the tree is
// BUILT, before layout has distributed anything, so a button that sizes to its
// own label would leave the body's width unknown at exactly the moment it is
// needed. Fixing the button is also the better design: two notifications with
// different action labels used to have their text end in different places.
#define ZELTO_NOTIF_ACTION_W 120.0f

// The width of the card's text column — the value to hand WrapText. The caller
// knows the CARD's width (it framed it); this turns that into the column, so the
// arithmetic lives next to the layout it describes instead of at each call site.
static inline float zelto_notif_text_w(float card_w, bool has_trailing) {
    float w = card_w - 2.0f * ZELTO_NOTIF_PAD - ZELTO_NOTIF_ICON
              - ZELTO_NOTIF_GAP;
    if (has_trailing) {
        w -= ZELTO_NOTIF_ACTION_W + ZELTO_NOTIF_GAP;
    }
    return w > 40.0f ? w : 40.0f;
}

// One notification card. `app_id` resolves to the poster's icon + display name
// through its manifest; `trailing` (may be NULL) is the action button.
//
// `card_w` IS REQUIRED AND IS NOT DECORATION. A notification's title and body
// are arbitrary strings supplied by whichever app posted them — the one kind of
// text in the OS that is neither authored here nor bounded by anything — and
// until P45 they were plain Text nodes, which measure to a single line and run
// straight off the right edge of the card. Nothing clipped them, nothing warned,
// and the failure needed only a notification with a normal sentence in it.
static inline ZView zelto_notif_card(ZApp *app, float card_w,
                                     const char *app_id, const char *title,
                                     const char *body, ZView trailing,
                                     bool interactive) {
    // The posting app's icon (resolved from app_id via its manifest, like
    // Recents), falling back to the shared Placeholder if it has none or it will
    // not load. App icons are square, so a plain aspect-fit Image is right here.
    char ipath[256];
    const char *icon =
        (zelto_icon_for_app_id(app_id, ipath, sizeof(ipath)) &&
         z_image_loads(ipath))
            ? ipath
            : zelto_placeholder_icon();

    // The app's own name, not its reverse-DNS id: "os.zelto.pinger" is a database
    // key, and printing it on the card is the surest sign a notification was laid
    // out by an engineer. The manifest has the display name.
    char name[96];
    const char *who = zelto_name_for_app_id(app_id, name, sizeof(name))
                          ? name
                          : app_id;

    ZColor cap = interactive ? Z_COLOR_TEXT_MUTED : Z_COLOR_TEXT_FAINT;
    ZColor ink = interactive ? Z_COLOR_TEXT : Z_COLOR_TEXT_MUTED;
    ZColor bg = interactive ? Z_COLOR_MATERIAL_THICK : Z_COLOR_MATERIAL_REGULAR;

    float tw = zelto_notif_text_w(card_w, trailing != NULL);

    ZStackOpts row = {.padding = ZELTO_NOTIF_PAD, .spacing = ZELTO_NOTIF_GAP,
                      .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Frame(ZELTO_NOTIF_ICON, ZELTO_NOTIF_ICON,
        CornerRadius(ZELTO_NOTIF_ICON * Z_RADIUS_ICON, Image(icon)));
    // `who` stays a single Text: it is the poster's DISPLAY NAME out of its
    // manifest, which is ours and is capped at 96 bytes, so it is bounded in a
    // way the title and body are not. It is also a caption that should read as
    // one line — wrapping it would be a worse result than the bound it has.
    row.children[k++] = Grow(1.0f,
        VStack(
            Foreground(cap, Weight(Z_WEIGHT_MEDIUM,
                Font(Z_FONT_CAPTION2, Text("%s", who)))),
            Foreground(ink, Weight(Z_WEIGHT_SEMIBOLD,
                WrapText(app, title, .width = tw, .size = Z_FONT_HEADLINE,
                         .weight = Z_WEIGHT_SEMIBOLD))),
            Foreground(ink,
                WrapText(app, body, .width = tw, .size = Z_FONT_SUBHEAD)),
            .spacing = 2, .align = Z_ALIGN_LEADING));
    if (trailing) {
        row.children[k++] = trailing;
    }

    return Shadow(interactive ? Z_ELEV_2 : Z_ELEV_1,
        Background(bg,
            CornerRadius(Z_RADIUS_CARD, z_stack(Z_AXIS_HORIZONTAL, &row))));
}

#endif  // ZELTO_SYSTEM_COMMON_NOTIF_CARD_H
