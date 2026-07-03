# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Settings is the P18 brokered-settings demo: it reads and writes system
# toggles (sys.wifi / sys.mute / sys.bright / sys.airplane / sys.brightness)
# through the zsysd settings store — the same source of truth the quick-settings
# shade uses — so a flip here updates the shade chip live and survives a reboot.
# Needs no runtime permission (the settings broker is not perm-gated).
id=os.zelto.settings
name=Settings
subtitle=System toggles, shared with the shade
exec=/usr/bin/zelto-settings
color=5b8def
icon=/usr/share/zelto/icons/settings.svg
