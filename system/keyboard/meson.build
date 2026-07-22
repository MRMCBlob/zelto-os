# The on-screen keyboard (P21): a bottom-anchored TOP-layer System UI client and
# the system's single input-method-v2 client, installed as /usr/bin/zelto-keyboard
# and started by init like the bars. Shown only while a text field is focused.
executable(
  'zelto-keyboard',
  files('main.c'),
  include_directories : include_directories('..'),  # system/ for common/safe_areas.h
  dependencies : zelto_dep,
  install : true,
)
