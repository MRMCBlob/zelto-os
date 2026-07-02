# "Share": the intent-source demo app. Installed as /usr/bin/zelto-share.
executable(
  'zelto-share',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-share.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
