# "Photos": the photo-library browser (P53). The READER half of the capture ->
# store -> browse pipeline; the library itself is system/common/photos.h and is
# shared with zelto-shot (and, later, the camera). Installed as
# /usr/bin/zelto-photos.
executable(
  'zelto-photos',
  files('main.c'),
  include_directories : include_directories('../..'),  # system/ for common/
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher (install tile) and zsysd.
install_data(
  'zelto-photos.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
