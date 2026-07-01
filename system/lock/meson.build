# The lock screen + idle lifecycle (P20): a libzelto OVERLAY layer-shell app that
# drives idle -> dim -> lock -> off off ext-idle-notify + the sys.lock_* broker
# settings, and blocks the app beneath while locked (whole input region +
# EXCLUSIVE keyboard). Started by init last so it is the topmost overlay.
# Installed as /usr/bin/zelto-lock.
executable(
  'zelto-lock',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)
