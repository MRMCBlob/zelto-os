# libzelto - the native Zelto UI toolkit apps link against.
# Retained view tree + single-pass layout + software (shm) renderer + HarfBuzz/
# FreeType text. See docs/contributing/sdk-internals.md.
#
# Built as a static library so a linked app's runtime closure is just its own
# Wayland/HarfBuzz/FreeType .so deps (simplest to bundle into the initramfs).

cc = meson.get_compiler('c')

wayland_client = dependency('wayland-client', required : false)
harfbuzz       = dependency('harfbuzz', required : false)
freetype       = dependency('freetype2', required : false)
xkbcommon      = dependency('xkbcommon', required : false)

if not (wayland_client.found() and harfbuzz.found() and freetype.found()
        and xkbcommon.found())
  warning('libzelto: wayland-client / harfbuzz / freetype2 / xkbcommon missing; skipping SDK')
  libzelto = disabler()
  zelto_dep = disabler()
  subdir_done()
endif

# wayland-protocols is needed by sdk/protocols (xdg-shell client bindings). It is
# discovered in the compositor build; rediscover here so the SDK is standalone.
wayland_protos = dependency('wayland-protocols')

subdir('protocols')

zelto_inc = include_directories('include')

libzelto = static_library(
  'zelto',
  files(
    'src/view.c',
    'src/layout.c',
    'src/text.c',
    'src/render.c',
    'src/reconcile.c',
    'src/app.c',
  ),
  sdk_protocol_src,
  include_directories : [zelto_inc, include_directories('src', 'protocols')],
  dependencies : [
    wayland_client, harfbuzz, freetype, xkbcommon,
    cc.find_library('m', required : false),
  ],
  install : true,
)

# Exported so samples/apps can link it: `dependencies : zelto_dep`.
zelto_dep = declare_dependency(
  link_with : libzelto,
  include_directories : zelto_inc,
  dependencies : [wayland_client, harfbuzz, freetype, xkbcommon],
)

# Bundle the default UI font into the image at the path libzelto looks for
# (overridable with $ZELTO_FONT). A single face is enough for the MVP.
install_data(
  'assets/fonts/ZeltoSans.ttf',
  install_dir : get_option('datadir') / 'zelto/fonts',
)
