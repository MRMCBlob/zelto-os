# "Pinger": the notification-source demo app. Installed as /usr/bin/zelto-pinger.
executable(
  'zelto-pinger',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-pinger.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
