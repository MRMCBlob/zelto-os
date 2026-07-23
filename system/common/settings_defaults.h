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

// --- keys whose NAME two surfaces have to agree on (P49) ---------------------
// The same rule one line up, applied to the spelling rather than the value. The
// keyboard's learned dictionary is read from two processes — the keyboard
// publishes the count and honours the clear, Settings displays the count and
// requests the clear — so the strings were about to exist as a #define in one
// file and four raw literals in the other, which is the drift this header was
// written to stop.
//
// sys.kbd_learned: how many words the keyboard has learned. Published by the
// keyboard, displayed by Settings > Keyboard. Read-only to everyone else.
#define ZELTO_KEY_KBD_LEARNED "sys.kbd_learned"
// sys.kbd_forget_learned: an EPOCH, not a command. Settings bumps it; the
// keyboard compares it against one it persisted and forgets everything when they
// differ — which is what makes a clear requested while the keyboard is not
// running still happen on its next boot. See kbd_learn() in system/keyboard.
#define ZELTO_KEY_KBD_FORGET "sys.kbd_forget_learned"

#endif  // ZELTO_SYSTEM_COMMON_SETTINGS_DEFAULTS_H
