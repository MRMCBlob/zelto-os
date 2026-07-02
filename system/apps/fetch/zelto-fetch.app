# Zelto app manifest (key=value). Scanned by the launcher (install tile) and by
# zsysd. Fetch is the P12 networking demo: it declares the `network` permission,
# which the user grants at runtime (the P8 consent modal) the first time it sends
# a request. It GETs http://10.0.2.2:8080/hello.txt — under QEMU user-mode
# networking 10.0.2.2 is the host, where the NET=1 harness runs an HTTP server.
id=os.zelto.fetch
name=Fetch
subtitle=HTTP GET over the network permission
exec=/usr/bin/zelto-fetch
color=2e9bff
permissions=network
