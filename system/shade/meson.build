# The notification shade: a libzelto OVERLAY layer-shell app that subscribes to
# zsysd as the notification sink and renders heads-up banner cards. Started by
# init like the bar. Installed as /usr/bin/zelto-shade.
executable(
  'zelto-shade',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/app_icons.h
  dependencies : zelto_dep,
  install : true,
)
