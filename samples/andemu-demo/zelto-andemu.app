# Zelto app manifest (key=value). The andemu demo: an emulated Android app. It is
# a Zelto Script app run in ANDROID mode — exec= runs the shared runtime with
# --android on the app's .js, so the guest advertises the Android compat layer.
# runtime=andemu is what a packaged .zap declares; the installer synthesises the
# --android exec= from it (here it is baked in directly, as jsdemo bakes its exec).
#
# The permissions are the ZELTO names the android/* layer maps onto: the app asks
# Android for ACCESS_FINE_LOCATION / BODY_SENSORS, which resolve to these.
id=os.zelto.andemu.demo
name=andemu Demo
subtitle=An Android app on Zelto: android.* -> Zelto APIs
version=1.0.0
runtime=andemu
exec=/usr/bin/zelto-script --android --id os.zelto.andemu.demo /usr/share/zelto/scripts/andemu-demo.js
permissions=sensors,location,notifications
color=a4c639
icon=/usr/share/zelto/apps/icons/os.zelto.andemu.demo.png
