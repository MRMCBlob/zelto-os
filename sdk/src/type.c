// Dynamic Type: the seam between a semantic type STEP and the units the shaper
// gets. See the long note above z_font_units() in <zelto/ui.h> for the design
// decisions (why an additive point offset and not a multiplier, which surfaces
// opt out, and where the range stops). This file is the mechanism.
//
// PROCESS-GLOBAL, NOT PER-APP, and that is the point: a ZFont step is converted
// in five places (the node default, Font(), WrapText, EllipsizeText,
// z_line_height), three of which have no ZApp in hand. Threading a size through
// them would be the same mistake as threading it through the 19 call sites. A
// process draws exactly one surface in this OS, so one process-global is one
// surface's text size — the granularity the setting actually has.

#include <stdbool.h>

#include <zelto/ui.h>

#include "internal.h"

// The offsets, in POINTS, indexed by size step. Written as a table rather than a
// formula because it IS a table: Apple's spacing is one point per step below the
// default and two above it, and a closed form for that is a lie about where the
// numbers came from.
static const int g_offset_pt[Z_TEXT_SIZE_STEPS] = Z_TEXT_SIZE_OFFSETS;

// The names, in the order the steps run. These are the platform's names and they
// are what a Settings screen shows; a step index is not a label.
static const char *const g_names[Z_TEXT_SIZE_STEPS] = {
    "Extra Small", "Small", "Medium", "Default", "Large", "Extra Large",
    "Largest",
    // The five accessibility steps. iOS calls them "Accessibility Medium" and so
    // on; those names describe the position INSIDE the accessibility range and
    // read as smaller than "Largest", which they are not. AX1..AX5 is what the
    // documentation calls them and what the slider's own position says.
    "AX1", "AX2", "AX3", "AX4", "AX5",
};

static int g_step = Z_TEXT_SIZE_DEFAULT;
static bool g_bold = false;
static bool g_enabled = true;

// Clamp rather than reject. The value arrives as a brokered STRING that any
// process can write, and z_setting_get_int of a corrupt store returns whatever
// strtoll made of it — a screen at step -400 is not a smaller failure than a
// screen at step 3, it is an unreadable one.
static int clamp_step(int step) {
    if (step < 0) {
        return 0;
    }
    if (step >= Z_TEXT_SIZE_STEPS) {
        return Z_TEXT_SIZE_STEPS - 1;
    }
    return step;
}

void z_text_scaling_disable(void) { g_enabled = false; }

int z_text_size(void) { return g_enabled ? g_step : Z_TEXT_SIZE_DEFAULT; }

const char *z_text_size_name(int step) { return g_names[clamp_step(step)]; }

bool z_text_bold(void) { return g_enabled && g_bold; }

// The reflow break. One comparison, in one place — see the long note in
// <zelto/ui.h> for where the 9 came from (it was measured, on Settings ▸ Lock
// Screen, at the step where the row runs out of height and the stepper runs off
// the right edge simultaneously).
//
// Gated on g_enabled for the same reason z_font_units() is: a process that
// called z_text_scaling_disable() draws at the default size, and a surface
// drawing default-size type must not rearrange itself as though it were not.
bool z_text_size_reflows(void) {
    return g_enabled && g_step >= Z_TEXT_SIZE_REFLOW_FIRST;
}

// Returns true when the value actually moved, so the caller knows whether a
// rebuild is owed. Called from startup (once) and from the settings fan-out.
bool z_text_size_apply(int step, bool bold) {
    step = clamp_step(step);
    bool changed = (step != g_step) || (bold != g_bold);
    g_step = step;
    g_bold = bold;
    return changed;
}

float z_font_units(ZFont step) {
    float base = (float)step;
    if (!g_enabled || g_step == Z_TEXT_SIZE_DEFAULT) {
        return base;
    }
    // THE DISPLAY STEP DOES NOT SCALE. Z_FONT_DISPLAY is the lock screen's clock
    // — 92pt, above the reading ladder entirely. It is not text you read, it is a
    // number that IS the screen, and it was sized against the screen's height
    // rather than against a body of prose. Six more points on 92 is 6.5%: too
    // small to help anyone who needed the setting, and large enough to push the
    // clock into the widgets under it. Making the exception explicit is cheaper
    // than discovering it as a layout bug on one screen.
    if (step == Z_FONT_DISPLAY) {
        return base;
    }
    float units = base + (float)Z_PT(g_offset_pt[g_step]);
    // The floor is Caption2's own size: the smallest step stops shrinking rather
    // than passing under itself, and the steps above it that would land below it
    // stop there too. (Apple's table floors at 11pt the same way — Caption2 is
    // 11pt at the four smallest sizes.)
    float floor_units = (float)Z_FONT_CAPTION2;
    return units < floor_units ? floor_units : units;
}

float z_row_h(ZApp *app) {
    float line = z_line_height(app, Z_FONT_BODY);
    float derived = line + 2.0f * (float)Z_ROW_VPAD;
    float floor_h = (float)Z_ROW_H;
    return derived > floor_h ? derived : floor_h;
}
