# The permission consent dialog: a libzelto overlay layer-shell app shown by
# zsysd on a prompt. Installed as /usr/bin/zelto-consent.
executable(
  'zelto-consent',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/app_icons.h
  dependencies : zelto_dep,
  install : true,
)
