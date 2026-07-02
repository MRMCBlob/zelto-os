# Zelto app manifest (key=value) for the P13 packaging demo app "Widget".
#
# Unlike every other app's manifest, this one is NOT copied into the image under
# /usr/share/zelto/apps. It is packaged INTO widget.zap (as zelto.toml, with exec=
# rewritten to the in-package native path by meta/mkzap.sh). zelto-install writes
# it out to $ZELTO_DATA_DIR/apps/manifests/os.zelto.widget.app at install time,
# with exec= rewritten again to the installed binary path on /var/zelto. The
# launcher + zsysd then discover it from that runtime dir.
#
# `version` drives the update rule: a later .zap with the same id and a higher
# version replaces the install (private data preserved). zelto.toml is the
# documented target format; a real TOML parser is Planned (key=value for now).
id=os.zelto.widget
name=Widget
subtitle=Installed from a .zap
color=2eb66e
version=1.0.0
