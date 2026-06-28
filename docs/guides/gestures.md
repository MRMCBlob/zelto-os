# Gestures

Zelto recognizes touch gestures through **recognizers** attached as modifiers. They
compose, resolve conflicts (e.g. tap vs. drag), and integrate with scrolling and the
system back gesture.

## Basic gestures

```js
View
  .onTap(() => open())
  .onLongPress(() => showMenu())
  .onDoubleTap(() => like());
```

Each callback receives an event with location info:

```js
.onTap((e) => console.log(e.x, e.y));
```

## Drag / pan

```js
import { useAnimatedValue } from "zelto/ui";

const x = useAnimatedValue(0);

Card()
  .offset(x, 0)
  .onPan((e) => x.set(e.translationX))
  .onPanEnd((e) => {
    if (e.translationX > 120) dismiss();
    else x.spring(0);
  });
```

Pan event fields: `translationX/Y`, `velocityX/Y`, `x/y`. Drive an animated value for
frame-rate motion (see [animation.md](animation.md)).

## Swipe

```js
.onSwipe("left", () => next())
.onSwipe("right", () => prev());
```

Shorthand for a directional pan that crosses a velocity/distance threshold.

## Composition & priority

Multiple recognizers on one view negotiate automatically: a tap is canceled if movement
exceeds the slop threshold and a pan begins. To override priority:

```js
.gesture(tap, { priority: "high" })
```

To require one gesture before another (e.g. long-press *then* drag to reorder):

```js
.onLongPress(startReorder).onPan(moveItem)   // pan engages after long-press
```

## Interaction with scrolling

Inside a `Scroll` or `List`, vertical drags scroll by default. A child that wants
horizontal drags (a carousel, a swipe-to-delete row) claims the horizontal axis; the
scroll keeps the vertical axis. Use `.scrollLock("horizontal")` if you need to fully
capture dragging.

## System back gesture

Edge-swipe-back is handled by the navigator and maps to popping the current screen. You
usually don't implement it; to intercept (e.g. confirm unsaved changes):

```js
useBackGuard(() => hasUnsavedChanges ? confirmThenPop() : true);
```

Returning `true` allows the back; `false` cancels it. See [navigation.md](navigation.md).

## Hit testing

- Tap targets should be ≥ 44 px; pad small controls to reach it.
- `.hitSlop(n)` enlarges the touch area without changing layout.
- `.disabled(true)` stops a view (and children) from receiving input.

## C API

In C, gesture handlers are named functions (inline closures are a Script-only
convenience — see the note in [../api-reference/c/ui.md](../api-reference/c/ui.md)).
Live now: `OnTap` / `Button` taps, `OnTapData` (a tap carrying a per-view data
pointer, for data rows), and `OnPan` drag. A press that moves past the slop
threshold becomes a pan and cancels the tap. Long-press / swipe recognizers are
still **Planned**.

```c
static void open_card(ZApp *app, void *state) { /* ... */ z_invalidate(app); }

static void on_drag(ZApp *app, void *state, const ZPanEvent *e) {
    App *s = state;
    z_animated_set(s->x, e->translation_x);          // follow the finger
    if (e->phase == Z_PAN_END) {
        z_animated_spring(s->x, 0.0f);               // settle / fling on release
    }
}

// in body():
OnTap(open_card, Card());
OnPan(on_drag, Offset(s->x, 0, Card()));
```

A pointer tap is dispatched to the deepest `OnTap`/`OnTapData` view under the
cursor; a focused control is also activated by Enter/Space. Inside a `Scroll` /
`List`, vertical drags scroll (with fling momentum) without any `OnPan`. See
[../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [animation.md](animation.md)
- [navigation.md](navigation.md)
