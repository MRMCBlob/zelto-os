# The brightness dim overlay (P19): a libzelto OVERLAY layer-shell app that
# observes sys.brightness and paints a full-screen translucent scrim to dim the
# whole screen. Started by init like the bar/shade. Installed as /usr/bin/zelto-dim.
executable(
  'zelto-dim',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/*.h
  dependencies : zelto_dep,
  install : true,
)
