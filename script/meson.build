# Zelto Script — the QuickJS-backed runtime for JS apps (docs/zelto-script/).
#
# Two artifacts:
#   libzscript      the embeddable host (z_script_main): engine + bindings.
#   zelto-script    the CLI a manifest's exec= points at, for a .js app.
#
# Needs the SDK (it IS the binding target), so it follows libzelto's disabler.

if not zelto_dep.found()
  warning('zelto-script: libzelto unavailable; skipping the script runtime')
  zscript_dep = disabler()
  subdir_done()
endif

# The `zelto/*` modules are authored as JS (script/js/*.js) and baked into the
# binary: the runtime must boot with no stdlib files to find on disk.
zs_builtins = custom_target(
  'zs-builtins',
  input : files(
    'js/core.js',
    'js/ui.js',
    'js/storage.js',
    'js/permissions.js',
    'js/net.js',
    'js/notifications.js',
    'js/settings.js',
    'js/intents.js',
    'js/sensors.js',
    # andemu — the Android-shaped compat route (P38). These translate android.*
    # calls into the zelto/* APIs above; compat.js is the graceful contract.
    'android/compat.js',
    'android/pm.js',
    'android/hardware.js',
    'android/location.js',
    'android/os.js',
    'android/net.js',
    'android/content.js',
    'android/prefs.js',
    'android/widget.js',
  ),
  output : 'builtins.c',
  command : [
    find_program('python3'),
    files('js2c.py'),
    '@OUTPUT@',
    'zelto=@INPUT0@',
    'zelto/ui=@INPUT1@',
    'zelto/storage=@INPUT2@',
    'zelto/permissions=@INPUT3@',
    'zelto/net=@INPUT4@',
    'zelto/notifications=@INPUT5@',
    'zelto/settings=@INPUT6@',
    'zelto/intents=@INPUT7@',
    'zelto/sensors=@INPUT8@',
    'android/compat=@INPUT9@',
    'android/pm=@INPUT10@',
    'android/hardware=@INPUT11@',
    'android/location=@INPUT12@',
    'android/os=@INPUT13@',
    'android/net=@INPUT14@',
    'android/content=@INPUT15@',
    'android/prefs=@INPUT16@',
    'android/widget=@INPUT17@',
  ],
)

zscript_inc = include_directories('include')

libzscript = static_library(
  'zscript',
  files('src/host.c', 'src/ui.c', 'src/sys.c', 'src/sensors.c'),
  zs_builtins,
  include_directories : [zscript_inc, include_directories('src')],
  dependencies : [zelto_dep, quickjs_dep],
  install : false,
)

zscript_dep = declare_dependency(
  link_with : libzscript,
  include_directories : zscript_inc,
  dependencies : [zelto_dep, quickjs_dep],
)

executable(
  'zelto-script',
  files('src/main.c'),
  dependencies : zscript_dep,
  install : true,
)
