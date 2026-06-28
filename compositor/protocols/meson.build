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

# Listed in the executable's sources so Ninja generates it before compiling.
# include_directories('protocols') (in the parent build file) puts the build-tree
# copy on the include path.
protocol_src = [xdg_shell_h]
