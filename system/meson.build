# Zelto System UI + bundled apps. The shell (status bar + launcher) and the
# extra launchable app, all linking libzelto (zelto_dep). Skipped automatically
# if the SDK was disabled (deps missing).

# zsysd is a plain daemon (no SDK dep); always build it.
subdir('zsysd')

if zelto_dep.found()
  subdir('bar')
  subdir('launcher')
  subdir('apps/cards')
  subdir('apps/notes')
  subdir('consent')
  subdir('chooser')
  subdir('share')
endif
