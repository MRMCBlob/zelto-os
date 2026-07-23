// Increase Contrast: the second value of the three design tokens that have one.
//
// See the measured table above z_ink_muted() in <zelto/gfx.h> for where the
// numbers came from and why only three tokens move. This file is the mechanism,
// and it is deliberately the same shape as sdk/src/type.c: one process-global,
// set from the brokered setting at startup and on change, read through a
// function that 121 call sites already go through without knowing it.
//
// NOT gated on z_text_scaling_disable(). The status bar, the keyboard and the
// home indicator opt out of Dynamic Type because their HEIGHTS are contracts
// other processes offset by — nothing about a contract says the clock has to be
// hard to read. A colour swap moves no geometry at all, which is exactly why
// this one is safe everywhere the other is not.

#include <stdbool.h>

#include <zelto/gfx.h>

static bool g_increase;

bool z_contrast_increased(void) { return g_increase; }

bool z_contrast_apply(bool increase) {
    bool changed = increase != g_increase;
    g_increase = increase;
    return changed;
}

// The shipped values and their high-contrast partners. Written out here rather
// than as macros in the header because the header's job is now to name the
// TOKEN; two values behind one name is what this file is.
ZColor z_ink_muted(void) {
    return g_increase ? z_rgba(0xc7, 0xc7, 0xcc, 0xff)
                      : z_rgba(0xa1, 0xa1, 0xa8, 0xff);
}

ZColor z_ink_faint(void) {
    return g_increase ? z_rgba(0xa8, 0xa8, 0xb0, 0xff)
                      : z_rgba(0x6c, 0x6c, 0x70, 0xff);
}

ZColor z_hairline(void) {
    return g_increase ? z_rgba(0x6c, 0x6c, 0x70, 0xff)
                      : z_rgba(0x38, 0x38, 0x3a, 0xff);
}
