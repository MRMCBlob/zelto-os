# C API: Graphics (`<zelto/gfx.h>`)

Low-level drawing for **custom views** — charts, canvases, game surfaces — when the
built-in components aren't enough. Most apps never need this. Conventions:
[../conventions.md](../conventions.md).

## Custom-draw views

Embed a view that draws into a GPU-backed canvas each frame it's invalidated:

```c
ZView Canvas(.size = {w, h}, .draw = draw_fn, .ud = state);

void draw_fn(ZCanvas *c, float w, float h, void *ud);
```

The canvas integrates with the scene graph: it is composited, clipped, and animated like
any view, and only redrawn when invalidated (`z_canvas_invalidate(view)`).

## Canvas drawing

```c
void z_canvas_clear(ZCanvas *c, ZColor color);

// Paths
ZPath *z_path_new(void);
void   z_path_move_to(ZPath *p, float x, float y);
void   z_path_line_to(ZPath *p, float x, float y);
void   z_path_curve_to(ZPath *p, float cx1, float cy1, float cx2, float cy2, float x, float y);
void   z_path_close(ZPath *p);

// Fills & strokes
void z_canvas_fill_path(ZCanvas *c, ZPath *p, ZColor color);
void z_canvas_stroke_path(ZCanvas *c, ZPath *p, ZColor color, float width);
void z_canvas_fill_rect(ZCanvas *c, ZRect r, float radius, ZColor color);

// Text
void z_canvas_draw_text(ZCanvas *c, const char *text, ZTextStyle style, float x, float y);

// Images
void z_canvas_draw_image(ZCanvas *c, ZImageHandle img, ZRect dst);

// Transform & clip
void z_canvas_save(ZCanvas *c);
void z_canvas_restore(ZCanvas *c);
void z_canvas_translate(ZCanvas *c, float x, float y);
void z_canvas_scale(ZCanvas *c, float sx, float sy);
void z_canvas_rotate(ZCanvas *c, float radians);
void z_canvas_clip_rect(ZCanvas *c, ZRect r, float radius);
```

## Colors

```c
ZColor z_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
ZColor z_token_color(ZColorToken t);   // resolve a design token for the current theme
```

Prefer `z_token_color` so custom drawing follows light/dark + accent
([../../overview/design-language.md](../../overview/design-language.md)).

## Direct GL/Vulkan surface (advanced)

For games or engines, request a raw rendering surface and drive it yourself:

```c
ZGpuSurface *z_gpu_surface(ZView_opts);   // EGL/GLES context (Vulkan: planned)
void z_gpu_present(ZGpuSurface *s);        // submit a frame; synced to vsync by zcomp
```

The surface is still a compositor surface — `zcomp` composites it with the rest of the UI
and applies vsync. Details: [../../contributing/compositor-internals.md](../../contributing/compositor-internals.md).

## See also

- [ui.md](ui.md) — built-in components (use these first).
- [../../tooling/profiling.md](../../tooling/profiling.md) — frame/GPU profiling.
