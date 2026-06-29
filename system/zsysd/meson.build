# zsysd — the system-service broker (permissions today; lifecycle/packages/
# intents later). A plain daemon: no UI, no wlroots/SDK deps, just libc + a unix
# socket. Installed as /usr/bin/zsysd; started by the initramfs init.
executable(
  'zsysd',
  files('main.c'),
  install : true,
)
