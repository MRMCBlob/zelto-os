# The volume-rocker overlay (P23): a libzelto OVERLAY layer-shell app that
# observes sys.volume / sys.mute and pops a transient centred slider on change,
# self-dismissing after ~1.5s. Started by init like the dim/lock overlays.
# Installed as /usr/bin/zelto-volume.
executable(
  'zelto-volume',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)
