# "Notepad": the persistent-storage demo app (P11). Installed as
# /usr/bin/zelto-notepad. Links libzelto (which links sqlite3 for z_db_*).
executable(
  'zelto-notepad',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher to build its install tile.
install_data(
  'zelto-notepad.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
