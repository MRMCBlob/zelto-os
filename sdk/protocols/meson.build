# Client-side Wayland protocol bindings for libzelto. Unlike libwayland-server,
# the client library does not ship the xdg-shell marshalling code, so we generate
# both the header and the private code and compile them into libzelto.

wayland_scanner = find_program('wayland-scanner')
wl_protocol_dir = wayland_protos.get_variable(pkgconfig : 'pkgdatadir')

xdg_shell_xml = wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml'

xdg_client_header = custom_target(
  'xdg-shell-client-protocol.h',
  input : xdg_shell_xml,
  output : 'xdg-shell-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_client_code = custom_target(
  'xdg-shell-protocol.c',
  input : xdg_shell_xml,
  output : 'xdg-shell-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wlr-layer-shell-unstable-v1 client bindings (the status bar / launcher map a
# layer surface instead of an xdg_toplevel). Like xdg-shell on the client side,
# libwayland-client ships no marshalling for it, so generate both the header and
# the private code. XML vendored in protocols/ (no wlr-protocols package).
layer_shell_xml = meson.project_source_root() / 'protocols/wlr-layer-shell-unstable-v1.xml'

layer_client_header = custom_target(
  'wlr-layer-shell-unstable-v1-client-protocol.h',
  input : layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

layer_client_code = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.c',
  input : layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wlr-foreign-toplevel-management-unstable-v1 client bindings (the launcher binds
# the manager to list running app windows and request activate/close on them).
# libwayland-client ships no marshalling for it, so generate both the header and
# the private code. XML vendored in protocols/ (no wlr-protocols package).
foreign_toplevel_xml = meson.project_source_root() / 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml'

foreign_toplevel_client_header = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  input : foreign_toplevel_xml,
  output : 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

foreign_toplevel_client_code = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  input : foreign_toplevel_xml,
  output : 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# ext-idle-notify-v1 client bindings (P20 idle/lock lifecycle). The compositor
# reports user activity through wlr_idle_notifier_v1 (ext-idle-notify-v1); a
# client — zelto-lock — binds ext_idle_notifier_v1 and creates idle notifications
# with per-timeout `idled`/`resumed` events to drive the dim -> lock -> off state
# machine. Unlike the wlr-* protocols this one SHIPS in wayland-protocols
# (staging), so use the system XML directly rather than vendoring it. wlroots
# ships the SERVER marshalling in libwlroots, so only the client side is generated.
idle_notify_xml = wl_protocol_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml'

idle_notify_client_header = custom_target(
  'ext-idle-notify-v1-client-protocol.h',
  input : idle_notify_xml,
  output : 'ext-idle-notify-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

idle_notify_client_code = custom_target(
  'ext-idle-notify-v1-protocol.c',
  input : idle_notify_xml,
  output : 'ext-idle-notify-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# text-input-unstable-v3 client bindings (P21 on-screen keyboard). An app's text
# field is a text-input client: it enables on focus, ships surrounding text, and
# receives committed strings from the keyboard (relayed by the compositor). Like
# ext-idle-notify this protocol SHIPS in wayland-protocols (unstable), so use the
# system XML directly. libwayland-client ships no marshalling, so generate both
# the client header and the private code.
text_input_xml = wl_protocol_dir / 'unstable/text-input/text-input-unstable-v3.xml'

text_input_client_header = custom_target(
  'text-input-unstable-v3-client-protocol.h',
  input : text_input_xml,
  output : 'text-input-unstable-v3-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
text_input_client_code = custom_target(
  'text-input-unstable-v3-protocol.c',
  input : text_input_xml,
  output : 'text-input-unstable-v3-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# input-method-unstable-v2 client bindings (P21). The on-screen keyboard
# (zelto-keyboard) is the single input-method client: it is told activate/
# deactivate (show/hide) and sends commit_string / delete_surrounding_text back.
# Unlike text-input-v3 this one is NOT in wayland-protocols (it is a wlr-protocol),
# so the XML is vendored under protocols/ exactly like layer-shell/foreign-toplevel.
input_method_xml = meson.project_source_root() / 'protocols/input-method-unstable-v2.xml'

input_method_client_header = custom_target(
  'input-method-unstable-v2-client-protocol.h',
  input : input_method_xml,
  output : 'input-method-unstable-v2-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
input_method_client_code = custom_target(
  'input-method-unstable-v2-protocol.c',
  input : input_method_xml,
  output : 'input-method-unstable-v2-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# wlr-data-control-unstable-v1 client bindings (P22 clipboard). The on-screen
# keyboard's Paste key reads the CLIPBOARD selection through data-control because a
# keyboard layer surface never holds keyboard focus, so the compositor never sends
# it the core wl_data_device selection offer (that goes to the focused client
# only). Like layer-shell/input-method this is a wlr-protocol not in
# wayland-protocols, so the XML is vendored under protocols/. wlroots ships the
# SERVER marshalling in libwlroots, so only the client side is generated here.
data_control_xml = meson.project_source_root() / 'protocols/wlr-data-control-unstable-v1.xml'

data_control_client_header = custom_target(
  'wlr-data-control-unstable-v1-client-protocol.h',
  input : data_control_xml,
  output : 'wlr-data-control-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
data_control_client_code = custom_target(
  'wlr-data-control-unstable-v1-protocol.c',
  input : data_control_xml,
  output : 'wlr-data-control-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# zelto-backdrop-v1 client bindings — the blurred system material (P37). A client
# cannot read the pixels behind its own surface, so a System-UI surface asks the
# compositor to blur the scene under a rectangle of it (the shade's panel, the
# dock, the keyboard) and paints its translucent tint over the result. Our own
# protocol, vendored in protocols/ like the wlr-* ones; both the header and the
# marshalling are generated (nothing ships it).
backdrop_xml = meson.project_source_root() / 'protocols/zelto-backdrop-v1.xml'

backdrop_client_header = custom_target(
  'zelto-backdrop-v1-client-protocol.h',
  input : backdrop_xml,
  output : 'zelto-backdrop-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
backdrop_client_code = custom_target(
  'zelto-backdrop-v1-protocol.c',
  input : backdrop_xml,
  output : 'zelto-backdrop-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

sdk_protocol_src = [
  xdg_client_header, xdg_client_code,
  layer_client_header, layer_client_code,
  foreign_toplevel_client_header, foreign_toplevel_client_code,
  idle_notify_client_header, idle_notify_client_code,
  text_input_client_header, text_input_client_code,
  input_method_client_header, input_method_client_code,
  data_control_client_header, data_control_client_code,
  backdrop_client_header, backdrop_client_code,
]
