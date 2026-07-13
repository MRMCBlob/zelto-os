# Wayland protocols for zcomp. wlroots' <wlr/types/wlr_xdg_shell.h> includes a
# "xdg-shell-protocol.h" that each compositor must generate itself from the
# upstream XML with wayland-scanner; we do that here. Custom app<->shell and
# layer-shell protocols can be added the same way (drop the XML, generate).
# See docs/contributing/repo-layout.md (compositor/protocols/).

wayland_scanner = find_program('wayland-scanner')

wl_protocol_dir = wayland_protos.get_variable(pkgconfig : 'pkgdatadir')

# Server-side header for the stable xdg-shell protocol. The marshalling code is
# already inside libwlroots, so only the header is needed.
xdg_shell_h = custom_target(
  'xdg-shell-protocol.h',
  input : wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  output : 'xdg-shell-protocol.h',
  command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# Server-side header for wlr-layer-shell-unstable-v1 (System UI: status bar /
# launcher anchored layers). wlroots ships the marshalling inside libwlroots (the
# zwlr_layer_shell_v1_interface symbol), so — exactly like xdg-shell — only the
# generated header is needed; <wlr/types/wlr_layer_shell_v1.h> #includes it. The
# XML is vendored in protocols/ because no wlr-protocols package is installed.
layer_shell_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input : meson.project_source_root() / 'protocols/wlr-layer-shell-unstable-v1.xml',
  output : 'wlr-layer-shell-unstable-v1-protocol.h',
  command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# Server-side header for wlr-foreign-toplevel-management-unstable-v1 (the task
# switcher: zcomp publishes a handle per app window, the launcher lists them and
# requests activate/close). As with layer-shell, wlroots ships the marshalling
# inside libwlroots (the zwlr_foreign_toplevel_manager_v1 symbols verified with
# nm), so only the generated server-header is needed; the XML is vendored in
# protocols/ because no wlr-protocols package is installed.
foreign_toplevel_h = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-protocol.h',
  input : meson.project_source_root() / 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  output : 'wlr-foreign-toplevel-management-unstable-v1-protocol.h',
  command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# zelto-backdrop-v1 — OUR protocol (the blurred material behind a System-UI
# surface). Unlike the protocols above, wlroots obviously ships no marshalling for
# it, so the compositor needs BOTH the server header and the private code compiled
# in (the same pair the SDK generates on the client side).
backdrop_xml = meson.project_source_root() / 'protocols/zelto-backdrop-v1.xml'

backdrop_h = custom_target(
  'zelto-backdrop-v1-protocol.h',
  input : backdrop_xml,
  output : 'zelto-backdrop-v1-protocol.h',
  command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

backdrop_c = custom_target(
  'zelto-backdrop-v1-protocol.c',
  input : backdrop_xml,
  output : 'zelto-backdrop-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Listed in the executable's sources so Ninja generates it before compiling.
# include_directories('protocols') (in the parent build file) puts the build-tree
# copy on the include path.
protocol_src = [xdg_shell_h, layer_shell_h, foreign_toplevel_h, backdrop_h,
                backdrop_c]
