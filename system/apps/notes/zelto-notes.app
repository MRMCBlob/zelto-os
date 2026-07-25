# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd to resolve intents. Notes is an intent *target*: it declares the MIME
# types it accepts shares for (share_targets=) and the URL schemes it opens
# (links=). zsysd routes matching z_share / z_open_url intents here.
id=os.zelto.notes
name=Notes
subtitle=Receives shares + deep links
exec=/usr/bin/zelto-notes
color=1d6e44
share_targets=text/plain,image/png
links=zelto
icon=/usr/share/zelto/apps/icons/os.zelto.notes.png
