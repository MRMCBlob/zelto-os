# "Cards": the second launchable demo app. Installed as /usr/bin/zelto-cards.
executable(
  'zelto-cards',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher to build its install tiles.
install_data(
  'zelto-cards.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
