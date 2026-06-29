# "Widget": the P13 packaging demo app. Cross-built like any app, but NOT
# install:true and NO manifest install_data — it must not be baked into the image
# as an installed app. The initramfs build (meta/initramfs/build-initramfs.sh)
# instead packages this binary into a signed widget.zap (meta/mkzap.sh) and stages
# the .zap as a raw file; zelto-install installs it at runtime.
executable(
  'zelto-widget',
  files('main.c'),
  dependencies : zelto_dep,
  install : false,
)
