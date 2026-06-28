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

sdk_protocol_src = [
  xdg_client_header, xdg_client_code,
  layer_client_header, layer_client_code,
]
