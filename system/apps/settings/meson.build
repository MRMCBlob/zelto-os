# "Settings": the brokered-settings demo app (P18). Installed as
# /usr/bin/zelto-settings. A normal xdg toplevel that reads/writes the zsysd
# settings store (the same sys.* keys the shade's quick-settings chips use).
executable(
  'zelto-settings',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher to build its install tile.
install_data(
  'zelto-settings.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
