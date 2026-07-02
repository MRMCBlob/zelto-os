# Animation

Zelto animations are **spring-based** by default — motion has mass and settles
naturally, matching the design language. You animate by changing state inside an
animation scope; the framework interpolates the affected properties (position, size,
opacity, color) automatically.

## Implicit animation

Wrap a state change in `withAnimation`; any view properties that differ between the old
and new tree animate:

```js
import { withAnimation, useState } from "zelto";

const [expanded, setExpanded] = useState(false);

return Box()
  .frame(expanded ? 300 : 120, 120)
  .background(expanded ? "accent" : "surface")
  .onTap(() => withAnimation(() => setExpanded(!expanded)));
```

Both the size and color transition with the default spring.

## Choosing a curve

```js
withAnimation({ spring: "snappy" }, () => setOpen(true));
withAnimation({ spring: { stiffness: 220, damping: 26 } }, () => ...);
withAnimation({ tween: { duration: 200, easing: "easeOut" } }, () => ...);
```

- Named springs: `"standard"` (default), `"snappy"`. Tokens in
  [../overview/design-language.md](../overview/design-language.md).
- Custom spring: `{ stiffness, damping, mass? }`.
- `tween` for fixed-duration easing when you specifically want it (prefer springs for
  movement).

## Transitions (insert / remove)

Control how views appear and disappear when added to or removed from the tree:

```js
show && Card().transition("slideUp");          // named
show && Card().transition({ enter: "fade", exit: "scale" });
```

Built-in transitions: `fade`, `scale`, `slideUp/Down/Left/Right`, `slideUp`. Combine
with `withAnimation` on the state that toggles `show`.

## Explicit / driven animation

For continuous or gesture-driven motion, use an animated value:

```js
import { useAnimatedValue } from "zelto/ui";

const x = useAnimatedValue(0);
// drive from a gesture:
.onPan((e) => x.set(e.translationX))
.onPanEnd(() => x.spring(0));        // spring back

return Card().offset(x, 0);
```

`useAnimatedValue` updates on the render thread without rebuilding the component, so
drags stay at frame rate.

## Shared-element transitions

Tag a view in two screens with the same key; the navigator animates between them:

```js
Image(url).sharedElement("photo-42");
```

See [navigation.md](navigation.md).

## Performance notes

- Animations run in the compositor scene graph and are GPU-composited; prefer animating
  `transform`/`opacity` over layout where possible.
- Avoid rebuilding large trees every frame — use `useAnimatedValue` for high-frequency
  motion instead of `setState` in a loop.
- Profile with [../tooling/profiling.md](../tooling/profiling.md).

## C API

In C the change is a named `ZAction` (no inline closures under strict ISO C), and
an animated value is a persistent scalar bound to a transform via `Offset`:

```c
static void expand(ZApp *app, void *state) { App *s = state; s->open = true; }
// ...
z_with_animation(app, Z_SPRING_STANDARD, expand);   // runs `expand`, springs under it

ZAnimated *x = z_animated_value(app, 0.0f);          // persistent (by call order)
z_animated_spring(x, 120.0f);                        // spring toward 120
return Offset(x, 0.0f, Card());                      // Card tracks x each frame
```

Springs advance on the wl_surface frame callback: continuous repaint while any
value is in flight, idle once settled. See
[../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [gestures.md](gestures.md)
- [navigation.md](navigation.md)
