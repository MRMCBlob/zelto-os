# zelto-install: the runtime package installer. A plain C CLI (no SDK dep) that
# links libsodium for Ed25519 signature + sha256 verification. Built only if
# libsodium is available for the target (install libsodium-dev:arm64 to cross it);
# otherwise skipped with a warning, like the SDK's optional deps.
libsodium_dep = dependency('libsodium', required : false)

if libsodium_dep.found()
  executable(
    'zelto-install',
    files('main.c'),
    dependencies : libsodium_dep,
    install : true,
  )
else
  warning('libsodium not found; skipping zelto-install (P13 packaging installer)')
endif
