# The 'hello' sample: a native libzelto app installed into the image as
# /usr/bin/zelto-hello and launched by the initramfs after zcomp.

executable(
  'zelto-hello',
  files('main.c'),
  dependencies : zelto_dep,
  install : true,
)
