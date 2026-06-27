# zcomp - the Zelto Wayland compositor (wlroots).
# See docs/contributing/compositor-internals.md.
#
# The build adapts to what is available:
#   - If wlroots/wayland/egl/etc. are present, build the real compositor that
#     brings up a DRM output and paints a frame.
#   - Otherwise (e.g. a bare host doing a quick smoke build) fall back to the
#     stub, which only prints its version and exits. This keeps `meson setup`
#     working before the graphics stack is installed.

cc = meson.get_compiler('c')

wlroots = dependency('wlroots-0.17', required : false)
if not wlroots.found()
  wlroots = dependency('wlroots', required : false)
endif
wayland_server = dependency('wayland-server', required : false)
wayland_protos = dependency('wayland-protocols', required : false)
xkbcommon     = dependency('xkbcommon', required : false)
pixman        = dependency('pixman-1', required : false)
libdrm        = dependency('libdrm', required : false)
egl           = dependency('egl', required : false)
glesv2        = dependency('glesv2', required : false)

have_graphics = (
  wlroots.found() and wayland_server.found() and xkbcommon.found()
  and pixman.found() and libdrm.found() and egl.found() and glesv2.found()
)

zcomp_inc = include_directories('include')

if have_graphics
  message('zcomp: building full wlroots compositor')

  # wlroots needs the unstable API opt-in.
  add_project_arguments('-DWLR_USE_UNSTABLE', language : 'c')

  subdir('protocols')

  zcomp_deps = [
    wlroots, wayland_server, xkbcommon, pixman, libdrm, egl, glesv2,
    cc.find_library('m', required : false),
  ]

  zcomp_src = files(
    'src/main.c',
    'src/server.c',
    'src/output.c',
    'src/render.c',
  )

  executable(
    'zcomp',
    zcomp_src,
    protocol_src,
    include_directories : zcomp_inc,
    dependencies : zcomp_deps,
    install : true,
  )
else
  warning('zcomp: graphics stack not found; building stub only')

  executable(
    'zcomp',
    files('src/main_stub.c'),
    include_directories : zcomp_inc,
    install : true,
  )
endif
