# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Store is the P13 packaging demo's installer UI: it fork/execs
# /usr/bin/zelto-install on a staged .zap and reports the result. It is a normal
# baked-in app (unlike Widget, which is installed at runtime from a .zap). It
# needs no runtime permission — invoking the installer is a plain fork/exec.
id=os.zelto.store
name=Store
subtitle=Install signed .zap packages
exec=/usr/bin/zelto-store
color=2e9bff
icon=/usr/share/zelto/icons/store.svg
