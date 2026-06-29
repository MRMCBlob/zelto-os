# "Store": the P13 runtime-installer UI. Installed as /usr/bin/zelto-store. A
# baked-in app whose tile drives zelto-install on a staged .zap.
executable(
  'zelto-store',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-store.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
