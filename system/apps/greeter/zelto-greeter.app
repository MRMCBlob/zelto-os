# Greeter — the packaged SCRIPT app (P35).
#
# Deliberately NOT installed into the image: this manifest is the one INSIDE the
# .zap (meta/mkzap.sh stages it as zelto.toml). There is no exec= here — a script
# package declares only its entry, and zelto-install synthesises the command that
# runs it on the shared Zelto Script runtime. Letting the package name its own
# interpreter would mean a signed manifest could point exec= at any binary on the
# device, so the installer, not the author, decides what executes the .js.
#
# mkzap.sh adds the `script=` line pointing at the in-package copy.
id=os.zelto.greeter
name=Greeter
subtitle=Installed from a signed .zap
version=1.0.0
color=3ca370
icon=/usr/share/zelto/apps/icons/os.zelto.greeter.png
