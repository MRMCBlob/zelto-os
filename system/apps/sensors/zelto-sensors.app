# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Sensors is the P38 sensor/location demo: it declares the `sensors` and
# `location` permissions, which the user grants at runtime (the consent modal) on
# first launch. The readouts come from the zsysd sensor source (sim-scriptable via
# ZELTO_SIM_* env), streamed onto the app loop.
id=os.zelto.sensors
name=Sensors
subtitle=Live accelerometer, gyroscope, orientation + GPS
version=1.0.0
exec=/usr/bin/zelto-sensors
color=3ad07a
permissions=sensors,location
icon=/usr/share/zelto/apps/icons/os.zelto.sensors.png
