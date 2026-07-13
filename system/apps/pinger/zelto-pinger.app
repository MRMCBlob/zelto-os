# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Pinger is a notification *source*: it declares the `notifications`
# permission, which the user grants at runtime (the P8 consent modal) the first
# time it posts. The notification it posts deep-links to zelto://note/7 (handled
# by Notes) and carries an "Ack" action routed back to Pinger.
id=os.zelto.pinger
name=Pinger
subtitle=Post a heads-up notification
exec=/usr/bin/zelto-pinger
color=b53d6a
permissions=notifications
icon=/usr/share/zelto/apps/icons/os.zelto.pinger.png
