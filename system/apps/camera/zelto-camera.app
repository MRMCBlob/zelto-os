# Zelto app manifest (key=value). Scanned by the launcher and by zsysd. Camera
# declares the `camera` permission, which the user grants at runtime (the consent
# modal) on first launch; the broker re-checks that grant when the stream opens,
# so an app cannot start a preview by not asking. Captures land in the shared
# photo library and are browsed in Photos.
id=os.zelto.camera
name=Camera
subtitle=Take a photo into your library
version=1.0.0
exec=/usr/bin/zelto-camera
color=4a90d9
permissions=camera
icon=/usr/share/zelto/apps/icons/os.zelto.camera.png
