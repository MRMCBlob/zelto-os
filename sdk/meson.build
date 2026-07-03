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
# Persistent storage (P11): the prefs/files store needs nothing extra, but the
# z_db_* SQLite wrapper links the system sqlite3.
sqlite3        = dependency('sqlite3', required : false)
# Image decode (P24): PNG app icons go through libpng; SVG icons use the in-house
# stroked rasterizer in image.c (no extra dep). libpng is the only new closure.
libpng         = dependency('libpng', required : false)

if not (wayland_client.found() and harfbuzz.found() and freetype.found()
        and xkbcommon.found() and sqlite3.found() and libpng.found())
  warning('libzelto: wayland-client / harfbuzz / freetype2 / xkbcommon / sqlite3 / libpng missing; skipping SDK')
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
    'src/image.c',
    'src/reconcile.c',
    'src/animation.c',
    'src/scroll.c',
    'src/navigation.c',
    'src/app.c',
    'src/storage.c',
    'src/net.c',
  ),
  sdk_protocol_src,
  include_directories : [zelto_inc, include_directories('src', 'protocols')],
  dependencies : [
    wayland_client, harfbuzz, freetype, xkbcommon, sqlite3, libpng,
    cc.find_library('m', required : false),
  ],
  install : true,
)

# Exported so samples/apps can link it: `dependencies : zelto_dep`.
zelto_dep = declare_dependency(
  link_with : libzelto,
  include_directories : zelto_inc,
  dependencies : [wayland_client, harfbuzz, freetype, xkbcommon, sqlite3, libpng],
)

# Bundle the default UI font into the image at the path libzelto looks for
# (overridable with $ZELTO_FONT). A single face is enough for the MVP.
install_data(
  'assets/fonts/ZeltoSans.ttf',
  install_dir : get_option('datadir') / 'zelto/fonts',
)
