# The status bar: a wlr-layer-shell System UI client, installed as
# /usr/bin/zelto-bar and started by the initramfs after zcomp.
executable(
  'zelto-bar',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/glyphs.h
  dependencies : zelto_dep,
  install : true,
)
