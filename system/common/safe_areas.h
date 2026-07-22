// The screen's safe areas, in ONE place — and the reason they are not pixels.
//
// WHAT THESE ARE. Three strips of the display that the System UI owns and apps
// must not draw under: the status bar at the top, the home-indicator strip at
// the bottom, and the on-screen keyboard when it is up. Each is published to the
// compositor as a layer-surface EXCLUSIVE ZONE by the process that draws it, and
// zcomp shrinks every app window by the sum. That makes these numbers a CONTRACT
// BETWEEN PROCESSES, not a shared constant: the bar declares 81, and the shade,
// the dim scrim and the volume HUD each float themselves below "the bar" by
// repeating the number. When one copy moves and the others do not, a translucent
// overlay lands on top of the clock — and nothing fails, nothing warns, and the
// screenshot still looks like a phone.
//
// They were five copies of BAR_H, two of HOMEBAR_H and two of KBD_H, in files
// that are never opened together. This is the same drift P42 fixed for
// sys.brightness with settings_defaults.h, and it gets the same treatment: one
// definition, plus test/test_safe_areas_shared.sh to fail the day a literal
// comes back.
//
// WHY THEY ARE POINTS. Zelto's surface coordinate is a raw device pixel on a
// 720x1440 screen, and the design reference is a 390pt handset, so one HIG point
// is 1.85 screen units (Z_PT in <zelto/ui.h>). P43 found the whole type scale had
// been transcribed from Apple's tables 1:1 — point values spent as pixels, every
// label at 54% of the size its container was drawn for. These three numbers had
// the identical bug and survived it by a phase, for the identical reason: they
// were consistently wrong together, so nothing collided.
//
//   Measured against the display, before:
//     status bar    40 units = 2.8% of the height. Apple's modern top inset is
//                   5.2%; 2.8% is proportionally the CLASSIC 20pt bar, off a
//                   device family that still had a hardware home button — the
//                   exact idiom P40 deleted when it moved navigation onto the
//                   home-indicator gesture.
//     home indicator 34 units = 2.4%, against Apple's 4.0%. Its own comment said
//                   "Apple's bottom safe-area inset", which is 34 POINTS.
//     keyboard      300 units, whose 56-unit keys stand 30pt tall — under the
//                   44pt minimum touch target, on the densest grid of tap
//                   targets in the OS.
//
// WHICH ONES MOVED, AND WHICH DID NOT. A number copied off a spec sheet is in
// points and converts. A number DERIVED from the screen is already in screen
// units and must not be touched — the home grid's 104-unit icon falls out of a
// 4-column grid on a 720-unit width (14.4%, against Apple's 15.3%), and it stays.
// That distinction is the correction to P43, which generalised from two derived
// metrics to "the metrics are the half that is right".
//
// THE PAIR 44/34 IS ONE DEVICE'S SPEC, NOT TWO GUESSES. iPhone X-class safe area
// insets are 44pt top and 34pt bottom. The bottom one was already here and named
// as Apple's; the top one is its sibling. The 12/13/14 family's 47pt top is
// larger by exactly the extra clearance a Dynamic Island needs, and this display
// has no hardware to clear — reserving it would be cargo-culting a notch. 44pt
// is also the HIG minimum touch target, and the bar is a control (long-press to
// lock), which the old 40 units = 21.6pt was not close to.
#ifndef ZELTO_SYSTEM_COMMON_SAFE_AREAS_H
#define ZELTO_SYSTEM_COMMON_SAFE_AREAS_H

#include <zelto/ui.h>

// Status bar: system/bar's exclusive zone. Everything that floats below the bar
// (the shade, the dim scrim, the volume HUD) offsets by this, and the launcher
// starts its grid under it.
#define ZELTO_BAR_H Z_PT(44)        // 81

// Home indicator: system/homebar's exclusive zone — the strip the up-swipe
// gestures start from, and the bottom inset apps stop above.
#define ZELTO_HOMEBAR_H Z_PT(34)    // 62

// The keyboard. KBD_H is DERIVED from what the strip contains rather than
// declared, so a key that grows cannot silently overflow the surface it is
// drawn in — which is what a lone 300 was hiding: 4x56 + gaps = 264 in a 300
// strip, 36 units of slack that read as "sized" and were an accident.
//
// A key cap is 42pt: Apple's letter key, and the reason it is worth naming is
// that it is the one metric here a finger tests forty times a minute.
#define ZELTO_KEY_H Z_PT(42)        // 77
#define ZELTO_KEY_GAP Z_PT(6)       // 11 — between rows and between caps
#define ZELTO_KEY_PAD Z_PT(8)       // 14 — the strip's own inset
#define ZELTO_KBD_ROWS 4
#define ZELTO_KBD_H                                                     \
    (ZELTO_KBD_ROWS * ZELTO_KEY_H + (ZELTO_KBD_ROWS - 1) * ZELTO_KEY_GAP \
     + 2 * ZELTO_KEY_PAD)           // 369

// The SUGGESTION STRIP above the caps (P48): three slots — the word you typed and
// the two the dictionary thinks you meant — and the undo for autocorrect. It is
// one row of Callout-sized buttons with the strip's own inset above and below, so
// its height is an EXPRESSION over the same key metrics and not a spec-sheet
// number: a slot cap is ZELTO_KEY_H tall (matching a key) and the strip pads it by
// ZELTO_KEY_PAD top and bottom, exactly as the keyboard grid pads its rows.
//
// It is part of the keyboard's EXCLUSIVE ZONE when shown — the app shrinks by the
// keys AND the strip so the focused field stays above both — which is why it is
// here with the other safe areas and not a private constant in the keyboard. It
// is OFF for password fields, and there the exclusive zone is ZELTO_KBD_H alone;
// see the two heights the keyboard toggles between (ZELTO_KBD_H / _TOTAL_H).
#define ZELTO_SUGGEST_H (ZELTO_KEY_H + 2 * ZELTO_KEY_PAD)   // 105

// The whole surface with the strip up: what the keyboard reserves and how tall it
// draws itself when prediction is on. Derived, so the strip cannot silently
// overflow the surface the way a literal KBD_H once hid 36 units of slack.
#define ZELTO_KBD_TOTAL_H (ZELTO_KBD_H + ZELTO_SUGGEST_H)   // 474

#endif  // ZELTO_SYSTEM_COMMON_SAFE_AREAS_H
