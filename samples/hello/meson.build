# The 'hello' sample: a native libzelto app installed into the image as
# /usr/bin/zelto-hello and launched by the initramfs after zcomp.

executable(
  'zelto-hello',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)

# App manifest scanned by the launcher to build its install tiles.
install_data(
  'zelto-hello.app',
  install_dir : get_option('datadir') / 'zelto/apps',
)
