# "Notes": the intent-target demo app. Installed as /usr/bin/zelto-notes.
executable(
  'zelto-notes',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest: declares the share-target + deep-link handlers zsysd resolves.
install_data(
  'zelto-notes.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
