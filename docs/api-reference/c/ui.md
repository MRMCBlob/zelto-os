# C API: UI (`<zelto/ui.h>`)

The native UI API. Apps are Wayland clients linking `libzelto`. UI is declarative: a
`body` function returns a `ZView` tree that the framework diffs into the GPU scene graph.
Conventions: [../conventions.md](../conventions.md).

> **Availability (MVP).** The current phase implements `Z_APP`, `z_invalidate`,
> `z_app_quit`, the `VStack`/`HStack`/`ZStack`/`Spacer`/`Rect`/`Text` views, the
> `Background`/`Foreground`/`Padding`/`Frame`/`CornerRadius`/`Font`/`Grow` modifiers,
> the colour tokens and `z_rgba`, with single-pass stack/flex layout and software
> (shm) rendering. Interactivity is now live too: the `Button` view, the `OnTap`
> and `OnKey` modifiers, `ZAction`/`ZKeyAction` handlers, pointer hit-testing,
> single-target keyboard focus, and a frame-callback-driven rebuild/repaint loop
> (input arrives from `zcomp` over `wl_seat`). Still **Planned**: `Grid`, `List`,
> `Scroll`, presentation/navigation, animation, multi-recognizer gestures
> (pan/long-press/swipe) and focus traversal beyond the first focusable view.

> **C handlers vs. Script closures.** The Script/JS examples on this page pass
> inline closures (`() => …`). In C, handlers are ordinary named `ZAction` /
> `ZKeyAction` functions — the project builds under strict ISO C (`-std=c17
> -Wpedantic -Werror`), which rules out the GNU statement-expression / nested-
> function tricks an inline-block macro would need. Define a static function and
> pass it (see *Callbacks & actions* below).

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

All return `ZView`. For stacks, **children are listed first (positionally), then
options as designated initializers** — this lets any subset of options compose
cleanly in C (the children fill the leading `ZView[]` member; the `.field = value`
options follow). Views without children (e.g. `Rect`) just take options.

### Layout

```c
VStack(child1, child2, .spacing = 12, .align = Z_ALIGN_LEADING, .padding = 16);
HStack(child1, child2, .spacing = 8);
ZStack(child1, child2, .align = Z_ALIGN_CENTER);
Spacer();
Grid(.columns = 3, .spacing = 8, children...);       // Planned
Scroll(.axis = Z_AXIS_VERTICAL, child);              // Planned
List(.data = items, .count = n, .key = key_fn, .row = row_fn);  // Planned
```

`align`: `Z_ALIGN_LEADING|CENTER|TRAILING`. `axis`: `Z_AXIS_VERTICAL|HORIZONTAL|DEPTH`.
`Grow(n, view)` weights a child's share of free main-axis space; `Spacer()` is a
flexible gap.

### Content

```c
Rect(.color = Z_COLOR_PRIMARY, .width = 80, .height = 80, .radius = 12);  // solid box
Text(const char *fmt, ...);              // printf-style, shaped with HarfBuzz/FreeType
Image(const char *source);               // Planned
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

Handlers are named functions. A `ZAction` fires on tap or keyboard activation; a
`ZKeyAction` fires on a key press while the view holds focus. Both receive the
live `ZApp` and the app's persistent `state` (the pointer passed to `body`), so
they mutate state and call `z_invalidate` to schedule a rebuild:

```c
typedef void (*ZAction)(ZApp *app, void *state);
typedef void (*ZKeyAction)(ZApp *app, void *state, uint32_t keysym);

static void on_tap(ZApp *app, void *state) {
    App *s = state;
    s->count++;
    z_invalidate(app);
}

// in body():
Button(on_tap, "Count: %d", s->count);
OnTap(on_tap, Rect(.color = Z_COLOR_PRIMARY, .width = 80, .height = 80));
OnKey(on_key, VStack(/* ... */));
```

Handlers run on the app loop in response to input, not during `body()`, which is
why they take `app`/`state` as parameters rather than capturing them. Pointer
taps are routed by hit-testing the laid-out tree (deepest `OnTap` under the
cursor wins); keys go to the first focusable view (any `Button`/`OnKey`), and
Enter/Space activates a focused control's `on_tap`. Multi-recognizer pan/gesture
callbacks (`on_pan`, `ZPanEvent`) remain **Planned**
([../../guides/gestures.md](../../guides/gestures.md)).

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
