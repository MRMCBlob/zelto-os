# The on-screen keyboard (P21): a bottom-anchored TOP-layer System UI client and
# the system's single input-method-v2 client, installed as /usr/bin/zelto-keyboard
# and started by init like the bars. Shown only while a text field is focused.
executable(
  'zelto-keyboard',
  # predict.c is the classifier; lm_trie.c is the language model behind its seam
  # and lm_bigram.c is what that model falls back to off the dictionary (P48).
  files('main.c', 'predict.c', 'lm_trie.c', 'lm_bigram.c'),
  include_directories : include_directories('..'),  # system/ for common/safe_areas.h
  dependencies : zelto_dep,
  install : true,
)
