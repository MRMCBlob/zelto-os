# C API: UI (`<zelto/ui.h>`)

The native UI API. Apps are Wayland clients linking `libzelto`. UI is declarative: a
`body` function returns a `ZView` tree that the framework diffs into the GPU scene graph.
Conventions: [../conventions.md](../conventions.md).

## App entry

```c
Z_APP(StateType, bodyFn)
```

Generates the entry point. `StateType` is your app state struct; `bodyFn` has signature:

```c
ZView body(ZApp *app, StateType *state);
```

| Symbol | Signature | Notes |
|---|---|---|
| `Z_APP(T, fn)` | macro | Declares entry + binds state type `T` |
| `z_invalidate` | `void z_invalidate(ZApp *app)` | Request a rebuild (like `setState`) |
| `z_app_quit` | `void z_app_quit(ZApp *app)` | Request app exit |
| `ZACT({ ... })` | macro | Wrap a statement block as a callback |

## Views

All return `ZView`. Options use designated initializers; children are variadic.

### Layout

```c
VStack(.spacing = 12, .align = Z_ALIGN_LEADING, .padding = 16, child1, child2, ...);
HStack(.spacing = 8, child1, child2, ...);
ZStack(.align = Z_ALIGN_CENTER, child1, child2, ...);
Spacer();
Grid(.columns = 3, .spacing = 8, children...);
Scroll(.axis = Z_AXIS_VERTICAL, child);
List(.data = items, .count = n, .key = key_fn, .row = row_fn);
```

`align`: `Z_ALIGN_LEADING|CENTER|TRAILING`. `axis`: `Z_AXIS_VERTICAL|HORIZONTAL|BOTH`.

### Content

```c
Text(const char *fmt, ...);              // printf-style
Image(const char *source);
Button(ZAction onTap, const char *fmt, ...);
TextField(.value = s, .on_change = cb, .placeholder = "…");
Switch(.value = b, .on_change = cb);
Slider(.value = f, .on_change = cb, .min = 0, .max = 1);
Spinner(.size = Z_SPINNER_SMALL);
Progress(.value = 0.5f);
```

### Presentation / navigation

```c
Navigator(.root = ScreenFn);
TabView(Tab("Home", "home", HomeFn), ...);
Sheet(.open = b, .on_close = cb, .detents = Z_DETENT_MEDIUM, child);
Modal(.open = b, .on_close = cb, child);
Alert(.open = b, .title = "…", .message = "…", actions...);
```

## Modifiers

Modifiers wrap a `ZView` and return a `ZView` (apply outermost-last):

```c
Padding(16, view);
Background(Z_COLOR_SURFACE, view);
CornerRadius(Z_RADIUS_MD, view);
Shadow(Z_ELEVATION_1, view);
Frame(width, height, view);
Font(Z_FONT_TITLE, view);
Foreground(Z_COLOR_TEXT, view);
Opacity(0.5f, view);
OnTap(action, view);
OnPan(pan_cb, view);
```

Tokens: `Z_COLOR_*`, `Z_FONT_*`, `Z_RADIUS_*`, `Z_ELEVATION_*`, `Z_SPACE_*`
([../../overview/design-language.md](../../overview/design-language.md)).

## Callbacks & actions

```c
typedef void (*ZAction)(ZApp *app, void *ud);

OnTap(ZACT({ s->count++; z_invalidate(app); }), Button(...));
```

`ZACT` captures `app` and `state` in scope. For pan/gesture callbacks:

```c
void on_pan(ZApp *app, ZPanEvent *e, void *ud);   // e->translation_x, e->velocity_x, ...
```

## Animation

```c
z_with_animation(ZSpring spring, ZAction change);     // Z_SPRING_STANDARD / _SNAPPY
ZAnimated *z_animated_value(ZApp *app, float initial);
void z_animated_set(ZAnimated *v, float to);          // immediate
void z_animated_spring(ZAnimated *v, float to);       // spring to value
```

Use `Offset(animated, 0, view)` to bind an animated value to a transform
([../../guides/animation.md](../../guides/animation.md)).

## Focus & scroll handles

```c
ZFocus *z_focus(ZApp *app);     z_focus_request(focus);
ZScroll *z_scroll(ZApp *app);   z_scroll_to(sc, 0, 0, true);
```

## See also

- [system.md](system.md) — storage, net, notifications.
- [platform.md](platform.md) — lifecycle, permissions, background.
- [gfx.md](gfx.md) — low-level drawing / custom views.
