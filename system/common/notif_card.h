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

// One notification card. `app_id` resolves to the poster's icon + display name
// through its manifest; `trailing` (may be NULL) is the action button.
static inline ZView zelto_notif_card(const char *app_id, const char *title,
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

    ZStackOpts row = {.padding = 14, .spacing = 14, .align = Z_ALIGN_CENTER};
    int k = 0;
    row.children[k++] = Frame(ZELTO_NOTIF_ICON, ZELTO_NOTIF_ICON,
        CornerRadius(ZELTO_NOTIF_ICON * Z_RADIUS_ICON, Image(icon)));
    row.children[k++] = Grow(1.0f,
        VStack(
            Foreground(cap, Weight(Z_WEIGHT_MEDIUM,
                Font(Z_FONT_CAPTION2, Text("%s", who)))),
            Foreground(ink, Weight(Z_WEIGHT_SEMIBOLD,
                Font(Z_FONT_HEADLINE, Text("%s", title)))),
            Foreground(ink, Font(Z_FONT_SUBHEAD, Text("%s", body))),
            .spacing = 2, .align = Z_ALIGN_LEADING));
    if (trailing) {
        row.children[k++] = trailing;
    }

    return Shadow(interactive ? Z_ELEV_2 : Z_ELEV_1,
        Background(bg,
            CornerRadius(Z_RADIUS_CARD, z_stack(Z_AXIS_HORIZONTAL, &row))));
}

#endif  // ZELTO_SYSTEM_COMMON_NOTIF_CARD_H
