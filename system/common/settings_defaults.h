// Default values for brokered system settings, in ONE place.
//
// zsysd has no defaults table: a setting that has never been written simply is
// not in the store, and each client passes its own fallback to
// z_setting_get_int(). That is fine until two clients disagree — and they did.
// sys.brightness defaulted to 3 in Settings, in the status bar and in the dim
// overlay, three separate literals that had to be changed together and were
// nowhere near each other, so "the default brightness" was a value you could
// only discover by grepping.
//
// Anything read by more than one surface belongs here.
#ifndef ZELTO_SYSTEM_COMMON_SETTINGS_DEFAULTS_H
#define ZELTO_SYSTEM_COMMON_SETTINGS_DEFAULTS_H

// sys.brightness, 1..5. Full brightness on a fresh device: a phone that boots
// dimmed looks broken, and zelto-dim paints a real black scrim for anything
// below 5, so a mid default meant every unconfigured install ran at ~62%.
#define ZELTO_DEFAULT_BRIGHTNESS 5

// sys.volume, 0..10.
#define ZELTO_DEFAULT_VOLUME 5

#endif  // ZELTO_SYSTEM_COMMON_SETTINGS_DEFAULTS_H
