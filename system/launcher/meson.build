# The app launcher: a libzelto toplevel (app_id os.zelto.launcher) that fork/
# execs app binaries on tile tap. Installed as /usr/bin/zelto-launcher.
executable(
  'zelto-launcher',
  files('main.c'),
  include_directories : include_directories('..'),
  dependencies : zelto_dep,
  install : true,
)
