# Zelto app manifest (key=value). The first SCRIPT app: no binary of its own —
# exec= runs the shared Zelto Script runtime on the app's .js entry, passing the
# manifest id so the shell (launcher, switcher, permissions) sees one identity.
# The launcher/zsysd split exec= on whitespace (system/common/exec_cmd.h), which
# is what makes an interpreted app launchable by the same machinery as a native
# one.
#
# The declarations below are the point of P35: a script app asks for capabilities,
# receives intents and is version-stamped for packaging exactly like a C app —
# the broker cannot tell (and does not care) which language is behind the id.
id=os.zelto.jsdemo
name=JS Demo
subtitle=Zelto Script: gestures, motion, system APIs
version=1.0.0
exec=/usr/bin/zelto-script --id os.zelto.jsdemo /usr/share/zelto/scripts/jsdemo.js
permissions=network,notifications
share_targets=text/plain
links=jsdemo
color=e0a53a
icon=/usr/share/zelto/apps/icons/os.zelto.jsdemo.png
