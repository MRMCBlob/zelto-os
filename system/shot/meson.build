# zelto-shot: the system screenshot service (P53). Forked by the compositor when
# the capture chord fires; copies the output through wlr-screencopy, writes a PNG
# into the shared photo library and posts a notification. Installed as
# /usr/bin/zelto-shot.
#
# The screencopy CLIENT bindings are generated here rather than in sdk/protocols
# because this is their only consumer — libzelto has no reason to carry the
# marshalling for a protocol no app can use. The XML is vendored in protocols/
# like the other wlr-* ones (there is no wlr-protocols package).
screencopy_xml = meson.project_source_root() / 'protocols/wlr-screencopy-unstable-v1.xml'

screencopy_client_header = custom_target(
  'wlr-screencopy-unstable-v1-client-protocol.h',
  input : screencopy_xml,
  output : 'wlr-screencopy-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
screencopy_client_code = custom_target(
  'wlr-screencopy-unstable-v1-protocol.c',
  input : screencopy_xml,
  output : 'wlr-screencopy-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable(
  'zelto-shot',
  files('main.c'),
  screencopy_client_header,
  screencopy_client_code,
  include_directories : include_directories('..'),   # system/ for common/photos.h
  dependencies : [zelto_dep, wayland_client],
  install : true,
)
