# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Notepad is the P11 persistent-storage demo: it writes a counter to its
# prefs store and rows to a SQLite DB in its private documents dir on the
# virtio-blk ext4 disk, then reads them back after a reboot. Storage needs no
# runtime permission (it is direct, app-scoped filesystem access).
id=os.zelto.notepad
name=Notepad
subtitle=Persistent prefs + SQLite
exec=/usr/bin/zelto-notepad
color=2e9bff
