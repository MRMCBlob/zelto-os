# The home indicator: a wlr-layer-shell System UI client that owns the bottom
# safe-area strip and turns swipes off it into Home / app-switcher / app-switch
# navigation. Installed as /usr/bin/zelto-homebar and started by init like the
# bar, in the slot the old three-button zelto-nav held.
executable(
  'zelto-homebar',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/safe_areas.h
  dependencies : zelto_dep,
  install : true,
)
