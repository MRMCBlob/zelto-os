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
> (input arrives from `zcomp` over `wl_seat`). Live as of P5: **`Scroll`**, the
> virtualised **`List`**, the **`Navigator`** stack with an animated slide
> transition, the spring **animation** engine (`z_animated_value` / `_spring`,
> `Offset`, `z_with_animation`), **`OnPan`** drag gestures (tap-vs-pan slop) and
> wheel/drag scrolling with fling momentum, plus `OnTapData` for data rows. Still
> **Planned**: `Grid`, `TabView`, sheets/modals, horizontal/paged scrolling,
> shared-element transitions, long-press/swipe recognizers and focus traversal
> beyond the first focusable view.

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
Scroll(app, .axis = Z_AXIS_VERTICAL, child);         // app first: allocates a ZScroll cell
List(app, .data = items, .stride = sizeof(Item), .count = n,
     .row_height = 64, .key = key_fn, .row = row_fn); // virtualised: only visible rows built
```

`Scroll`/`List` take `app` first because they allocate a persistent `ZScroll`
cell (scroll offset + fling) from the current screen by call order. `List`
virtualises by a fixed `row_height`: it builds only the rows in (and just around)
the viewport and keys each (`key`) so the reconciler reuses them across scrolls.
`data`/`stride` let it index any contiguous array — item `i` is
`(const char *)data + i*stride`; `row(app, item, i)` builds it.

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
Navigator(app, .root = ScreenFn);                    // app first (owns the stack)
TabView(Tab("Home", "home", HomeFn), ...);           // Planned
Sheet(.open = b, .on_close = cb, .detents = Z_DETENT_MEDIUM, child);   // Planned
Modal(.open = b, .on_close = cb, child);             // Planned
Alert(.open = b, .title = "…", .message = "…", actions...);            // Planned
```

A screen is `ZView screen(ZApp *app, void *props)`. It reaches the stack via
`z_navigation(app)` and gets the `props` passed at push time (props must outlive
the screen — point into stable state):

```c
typedef ZView (*ZScreenFn)(ZApp *app, void *props);
ZNav *z_navigation(ZApp *app);
void z_nav_push(ZNav *nav, ZScreenFn screen, void *props);
void z_nav_pop(ZNav *nav);
```

Pushes/pops animate with the standard spring (a horizontal slide). The system
back — the **Escape**/**Backspace** key, or a left-edge swipe — pops the top
screen automatically.

## Modifiers

Modifiers wrap a `ZView` and return a `ZView` (apply outermost-last):

```c
Padding(16, view);
Background(Z_COLOR_SURFACE, view);
CornerRadius(Z_RADIUS_MD, view);
Clip(Z_RADIUS_MD, view);        // mask the SUBTREE to this view's rounded frame
Shadow(Z_ELEVATION_1, view);
Frame(width, height, view);
Font(Z_FONT_TITLE, view);
Foreground(Z_COLOR_TEXT, view);
Opacity(0.5f, view);            // Planned
OnTap(action, view);
OnTapData(action, data, view);  // tap handler carrying a per-view data pointer
OnPan(pan_cb, view);            // drag recognizer (ZPanEvent)
Offset(animated_x, y, view);    // bind an animated value to a translation
```

Tokens: `Z_COLOR_*`, `Z_FONT_*`, `Z_RADIUS_*`, `Z_ELEVATION_*`, `Z_SPACE_*`
([../../overview/design-language.md](../../overview/design-language.md)).

### `CornerRadius` vs. `Clip`

These are reached for interchangeably and are not the same thing.

`CornerRadius` rounds a view's **own** paint — its fill, its background, its image mask —
and says nothing about its children, which draw straight past the corner. `Clip` is the
mask, and it applies to the **descendants**.

Reach for `Clip` when a child's silhouette must be the parent's rather than its own: a
fill that grows inside a rounded slab, a progress bar in a pill, artwork bled to a card's
edge. Rounding the child instead is the trap — it rounds all four of the child's corners,
including the ones out in the middle of the parent where the fill's head is meant to be a
straight cut, so a half-full slider reads as a lozenge floating in a slot rather than as a
level. (This is exactly what the tall `Slider` did until `Clip` existed.)

A view that is both a surface and a container wants both:

```c
Background(Z_COLOR_SURFACE_3,
    CornerRadius(Z_RADIUS_PANEL,        // the slab's own fill
        Clip(Z_RADIUS_PANEL,            // ...and what its children may paint
            ZStack(fill_grown_to_value, glyph))));
```

Clips nest and intersect, so a clipped card inside a clipped scroll viewport does the
right thing. The cost is per-pixel and only along the rounded edge; a clip's straight
interior takes the same path as an unclipped view.

### The type scale is in POINTS

`Z_FONT_*` steps are written as HIG **point** sizes and converted to Zelto's screen unit
by `Z_TYPE()` in `zelto/ui.h`. The phone output is 720x1440 raw pixels against a 390pt
design reference, so one point is ~1.85 screen units and `Z_FONT_BODY` (17pt) reaches the
renderer as 31.

This matters when you write a metric next to a type step. **Metrics are in screen units**
— `Frame(104, 104, …)` is 104 pixels, not points — so pairing a raw 17 with a 104px icon
gets you type at half the size the layout was built for. Always name the step; never
write a pixel size into `Font()`.

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
taps are routed by hit-testing the laid-out tree (deepest `OnTap`/`OnTapData`
under the cursor wins); keys go to the first focusable view (any `Button`/`OnKey`),
and Enter/Space activates a focused control's `on_tap`.

A data row can't close over its item in strict C, so `OnTapData` binds a pointer
that the handler receives as a third argument:

```c
typedef void (*ZTapAction)(ZApp *app, void *state, void *data);
static void open_item(ZApp *app, void *state, void *data) {   // data = the Item*
    z_nav_push(z_navigation(app), detail_screen, data);
}
// in a List row: OnTapData(open_item, (void *)item, Row(item))
```

Pan/drag is a real recognizer now: a press that moves past the slop threshold
becomes a pan (cancelling the tap). Inside a `Scroll`/`List` vertical drags
scroll; `OnPan` is for custom drags.

```c
typedef void (*ZPanHandler)(ZApp *app, void *state, const ZPanEvent *e);
// e->phase is Z_PAN_BEGIN | _CHANGED | _END; e->translation_x/y since start;
// e->velocity_x/y (px/s, valid at _END for a fling); e->x/y current position.
```

## Animation

```c
void z_with_animation(ZApp *app, ZSpring spring, ZAction change);  // _STANDARD / _SNAPPY
ZAnimated *z_animated_value(ZApp *app, float initial);  // persistent, by call order
void z_animated_set(ZAnimated *v, float to);          // jump (no animation)
void z_animated_spring(ZAnimated *v, float to);       // spring toward `to`
float z_animated_get(const ZAnimated *v);             // current value
```

An animated value is a **persistent** scalar (it survives rebuilds — allocated
from the current screen by call order, like a hook). `z_animated_spring` advances
it on the wl_surface frame callback: the loop repaints continuously while any
value is in flight and idles once everything settles. Bind one to a transform
with `Offset(animated_x, y, view)` — the subtree shifts by
`(z_animated_get(animated_x), y)`. The `Navigator` slide and gesture-driven drags
are both built on this ([../../guides/animation.md](../../guides/animation.md)).

## Focus & scroll handles

```c
ZFocus *z_focus(ZApp *app);     z_focus_request(focus);   // ZFocus Planned
ZScroll *z_scroll(ZApp *app);   z_scroll_to(sc, 0, 0, true);
```

`z_scroll(app)` fetches the current screen's next retained `ZScroll` cell — the
same one a `Scroll`/`List` in that position uses — for programmatic control
(e.g. scroll-to-top). `ZFocus` traversal beyond the first focusable view is
**Planned**.

## See also

- [system.md](system.md) — storage, net, notifications.
- [platform.md](platform.md) — lifecycle, permissions, background.
- [gfx.md](gfx.md) — low-level drawing / custom views.
