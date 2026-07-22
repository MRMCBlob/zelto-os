# "Sensors": the sensor/location demo app. Installed as /usr/bin/zelto-sensors.
executable(
  'zelto-sensors',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-sensors.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
