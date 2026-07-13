# QuickJS — the JavaScript engine Zelto Script runs on (docs/zelto-script/runtime.md).
#
# Vendored verbatim from https://bellard.org/quickjs/ (MIT, see LICENSE); only
# the engine core is kept — quickjs-libc (fs/process access) is deliberately
# left out, because an app sandbox gets its host bindings from script/ and
# nothing else. See third_party/quickjs/README.md for the update procedure.
#
# Upstream is not warning-clean under this project's -Wall -Wextra -Werror, and
# patching a third-party engine to be so is not a fight worth having: the target
# builds with warnings demoted to non-fatal, in its own static lib, so the strict
# settings still apply to every line of Zelto's own code.

quickjs_version = '2025-04-26'

qjs_cc = meson.get_compiler('c')

quickjs_lib = static_library(
  'quickjs',
  files(
    'cutils.c',
    'dtoa.c',
    'libregexp.c',
    'libunicode.c',
    'quickjs.c',
  ),
  c_args : [
    '-DCONFIG_VERSION="' + quickjs_version + '"',
    '-Wno-error',
  ],
  dependencies : [
    qjs_cc.find_library('m', required : false),
    dependency('threads'),
  ],
  override_options : ['warning_level=0', 'werror=false'],
  install : false,
)

# -isystem, not -I: quickjs.h's inline helpers (JS_DupValue, JS_NewInt32, …) are
# not clean under -Wpedantic -Wextra -Werror, and a consumer must not have to
# relax its own warnings to include an engine header.
quickjs_dep = declare_dependency(
  link_with : quickjs_lib,
  include_directories : include_directories('.', is_system : true),
)
