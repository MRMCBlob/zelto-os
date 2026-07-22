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

# ...and, since P42, the marshalling too — despite wlroots implementing this
# protocol. zelto-toplevel-capture-v1 takes a zwlr_foreign_toplevel_handle_v1 as
# a request argument, so wayland-scanner emits a reference to
# `zwlr_foreign_toplevel_handle_v1_interface` in OUR generated code. libwlroots
# exports its wlr_* C API but NOT the zwlr_* protocol metadata (verified with
# `nm -D`: wlr_foreign_toplevel_handle_v1_create is there, the interface constant
# is not), so that reference does not link. Generating our own copy supplies it.
#
# Two constants for one interface is safe, and libwayland is explicitly built for
# it: wl_interface_equal() compares interface POINTERS first and falls back to
# comparing names, precisely so that metadata duplicated across shared objects
# still validates. wlroots creates the handle resources against its hidden copy
# and we validate them against ours; the names match, so the object argument is
# accepted.
foreign_toplevel_c = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  input : meson.project_source_root() / 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  output : 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
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

# zelto-toplevel-capture-v1 — OUR protocol (App Switcher window thumbnails: the
# compositor snapshots a window as it leaves the foreground and hands the image
# to a switcher later). Like zelto-backdrop-v1, nothing ships its marshalling, so
# both the server header and the private code are generated here.
capture_xml = meson.project_source_root() / 'protocols/zelto-toplevel-capture-v1.xml'

capture_h = custom_target(
  'zelto-toplevel-capture-v1-protocol.h',
  input : capture_xml,
  output : 'zelto-toplevel-capture-v1-protocol.h',
  command : [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

capture_c = custom_target(
  'zelto-toplevel-capture-v1-protocol.c',
  input : capture_xml,
  output : 'zelto-toplevel-capture-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Listed in the executable's sources so Ninja generates it before compiling.
# include_directories('protocols') (in the parent build file) puts the build-tree
# copy on the include path.
protocol_src = [xdg_shell_h, layer_shell_h, foreign_toplevel_h,
                foreign_toplevel_c, backdrop_h, backdrop_c,
                capture_h, capture_c]
