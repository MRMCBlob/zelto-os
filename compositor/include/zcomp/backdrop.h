// zcomp backdrop blur (zelto-backdrop-v1): the compositor half of the system's
// "material" — the blurred, translucent panel a phone's shade / dock / keyboard /
// sheet is made of.
//
// A client cannot read the pixels behind its own surface, so the blur cannot be
// done client-side. A client instead attaches a zelto_backdrop_v1 to its surface
// and declares the rectangle it wants blurred; the compositor renders the scene
// BELOW that surface, blurs the rectangle, and parks the result in a scene buffer
// directly beneath the surface. The client then paints its own translucent tint
// and content over it. See compositor/src/backdrop.c.
#ifndef ZCOMP_BACKDROP_H
#define ZCOMP_BACKDROP_H

#include <wayland-server-core.h>

#include "zcomp/server.h"

struct ZcompOutput;

// Advertise the zelto_backdrop_manager_v1 global.
void zcomp_backdrop_init(ZcompServer *server);

// Refresh every live backdrop for this output, then leave the scene exactly as it
// was so the caller's ordinary commit paints the real frame. Cheap and a no-op
// when no client wants a backdrop; internally rate-limited (the blur is a second
// composite, so it is not worth doing at vsync). Call from the output frame
// handler BEFORE wlr_scene_output_commit.
void zcomp_backdrop_frame(ZcompServer *server, struct ZcompOutput *output);

#endif  // ZCOMP_BACKDROP_H
