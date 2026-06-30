# Zelto System UI + bundled apps. The shell (status bar + launcher) and the
# extra launchable app, all linking libzelto (zelto_dep). Skipped automatically
# if the SDK was disabled (deps missing).

# zsysd is a plain daemon (no SDK dep); always build it.
subdir('zsysd')

# zelto-install is a plain CLI (no SDK dep, links libsodium); build if it can.
subdir('installer')

if zelto_dep.found()
  subdir('bar')
  subdir('nav')
  subdir('launcher')
  subdir('apps/cards')
  subdir('apps/notes')
  subdir('apps/pinger')
  subdir('apps/notepad')
  subdir('apps/settings')
  subdir('apps/fetch')
  subdir('apps/store')
  subdir('apps/widget')
  subdir('consent')
  subdir('chooser')
  subdir('recents')
  subdir('share')
  subdir('shade')
  subdir('dim')
endif
