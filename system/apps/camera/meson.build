# "Camera": the capture half of P53's content pipeline. The frame source and its
# lifecycle live in libzelto (sdk/src/camera.c); the still goes into the shared
# photo library (system/common/photos.h) that zelto-shot also writes to.
# Installed as /usr/bin/zelto-camera.
executable(
  'zelto-camera',
  files('main.c'),
  include_directories : include_directories('../..'),  # system/ for common/
  dependencies : zelto_dep,
  install : true,
)

install_data(
  'zelto-camera.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
