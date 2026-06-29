# "Fetch": the networking demo app. Installed as /usr/bin/zelto-fetch.
executable(
  'zelto-fetch',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-fetch.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
