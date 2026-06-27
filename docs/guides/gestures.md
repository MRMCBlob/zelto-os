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

```c
OnTap(ZACT({ open(); }),
  OnPan(handle_pan,
    Card()));
```

See [../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [animation.md](animation.md)
- [navigation.md](navigation.md)
