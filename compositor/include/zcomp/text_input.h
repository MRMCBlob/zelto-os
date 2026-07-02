// zcomp text-input <-> input-method relay (the on-screen-keyboard bridge, P21).
//
// Two standard Wayland protocols meet here:
//   - text-input-v3   : an app's text field is a text-input client. It enables
//                        when focused, ships surrounding-text/content-type, and
//                        receives committed strings.
//   - input-method-v2 : the on-screen keyboard is the single input-method client.
//                        It is told to activate/deactivate (so it shows/hides) and
//                        sends commit_string / delete_surrounding_text back.
//
// The relay wires the two: a focused text-input that enables activates the input
// method (keyboard shows) and forwards its surrounding text; disabling deactivates
// it (keyboard hides); the input method's commit_string/preedit/delete are relayed
// to the focused text-input. wlroots ships both managers' marshalling inside
// libwlroots (like layer-shell), so no protocol XML is generated for the
// compositor — only the SDK generates the CLIENT bindings.
//
// virtual-keyboard-v1 is the documented fallback (a keyboard that injects raw
// keysyms, works for any app that reads wl_keyboard but cannot do smart insertion
// and gets no activate/deactivate to auto-show); the primary, authentic path is
// this text-input/input-method bridge. See docs/platform/soft-keyboard.md.
#ifndef ZCOMP_TEXT_INPUT_H
#define ZCOMP_TEXT_INPUT_H

#include <wayland-server-core.h>

struct ZcompServer;

// Bring up wlr_text_input_manager_v3 + wlr_input_method_manager_v2 and the relay
// that bridges them. Call AFTER the seat exists (it listens to seat keyboard
// focus changes to route text-input enter/leave). One per server.
void zcomp_text_input_init(struct ZcompServer *server);

#endif  // ZCOMP_TEXT_INPUT_H
