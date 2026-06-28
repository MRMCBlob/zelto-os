project(
  'zelto-os',
  'c',
  version : '0.0.1',
  license : 'Apache-2.0',
  default_options : [
    'c_std=c17',
    'warning_level=3',   # -Wall -Wextra
    'werror=true',       # -Werror
    'buildtype=debugoptimized',
  ],
  meson_version : '>=1.1.0',
)

# Zelto OS top-level build. Ties the per-component subprojects together
# (see docs/contributing/repo-layout.md). P2/P3: the compositor (zcomp), the
# native UI SDK (libzelto) and the sample apps that link it.
add_project_arguments(
  ['-D_GNU_SOURCE'],
  # The declarative-UI macros (VStack/HStack/List/Scroll/...) build a struct
  # whose first member is the positional `children[]` array, then trailing
  # designated options — brace elision is the whole point of the API. GCC's
  # -Wmissing-braces (under -Wextra) fires on that elision with a fragile,
  # content-dependent heuristic (it flags some call sites and not others). We
  # keep every other warning as an error; only this known false positive is off.
  '-Wno-missing-braces',
  language : 'c',
)

subdir('compositor')
subdir('sdk')
subdir('samples')
