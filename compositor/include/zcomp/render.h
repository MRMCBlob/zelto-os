// zcomp render: paints one frame for an output. The proof-of-GPU-path step of
// the bootstrap: clear to a solid color, then draw one rectangle, via the
// wlroots GLES2 render pass (EGL/GLES under the hood).
#ifndef ZCOMP_RENDER_H
#define ZCOMP_RENDER_H

#include "zcomp/output.h"

// Render and present a single frame for this output, then schedule the next.
void zcomp_output_render(ZcompOutput *output);

#endif  // ZCOMP_RENDER_H
