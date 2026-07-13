// C API: Zelto Script host (<zelto/script.h>)
//
// The embedding surface of the Zelto Script runtime: hand it a .js entry file
// and it boots a QuickJS context wired to libzelto, then runs the ordinary
// libzelto app loop with the script's default-exported component as body().
// See docs/zelto-script/runtime.md.
//
// The `zelto-script` binary in this directory is a two-line user of this API;
// it exists so a manifest can say `exec=/usr/bin/zelto-script /path/app.js`.
// A C app that wants to host script screens of its own links libzscript and
// calls z_script_main directly.
#ifndef ZELTO_SCRIPT_H
#define ZELTO_SCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

// Load `entry` (an ES module whose default export is the root component), then
// run it as an app: connects to Wayland under `app_id` (NULL = derive from the
// file name), rebuilds the view tree by calling the component, and pumps JS
// timers + promise jobs on the app loop. Returns the app's exit status; a load
// or first-render failure prints the JS exception and returns non-zero.
int z_script_main(const char *entry, const char *app_id, const char *title);

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_SCRIPT_H
