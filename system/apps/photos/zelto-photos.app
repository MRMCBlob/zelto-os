# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Photos browses the shared photo library at <data>/media/photos, which
# zelto-shot writes into. It declares `notifications` because it is the POSTER of
# the screenshot banner: zelto-shot has no window and no app loop, so it posts to
# the broker under this app's identity — which is also what makes the banner
# carry the Photos name and route a tap back here.
id=os.zelto.photos
name=Photos
subtitle=Screenshots and pictures you have taken
version=1.0.0
exec=/usr/bin/zelto-photos
color=c77dff
permissions=notifications
links=zelto://photos
icon=/usr/share/zelto/apps/icons/os.zelto.photos.png
