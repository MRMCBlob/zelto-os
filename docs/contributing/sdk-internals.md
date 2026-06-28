# SDK Internals (`libzelto`)

`libzelto` is the native UI toolkit apps link against. It turns a declarative view tree
into a retained, GPU-rendered scene graph and handles layout, text, animation, and
gestures. This page explains how it works inside.

## Pipeline

```
body() ──► ViewTree (immutable, per-build)
              │  diff
              ▼
        Scene Graph (retained)  ──► Layout ──► Display Lists ──► zcomp (composite)
              ▲                                                      │
              └──────────────── invalidate / animate ───────────────┘
```

1. **Build:** the app's `body` returns an immutable `ZView` tree (cheap, arena-allocated).
2. **Diff/reconcile:** the new tree is diffed against the retained scene graph; only
   changed nodes are updated, created, or removed.
3. **Layout:** a single-pass stack/flex layout computes frames
   ([../guides/layout.md](../guides/layout.md)).
4. **Paint:** changed nodes produce display lists (GPU draw commands).
5. **Composite:** lists are submitted to `zcomp`, which composites with damage tracking
   ([compositor-internals.md](compositor-internals.md)).

## View tree vs. scene graph

- The **view tree** is a value produced each build — describe-only, no identity beyond
  keys, thrown away after diffing.
- The **scene graph** is the retained, mutable structure with node identity, cached
  layout, and GPU resources. Diffing maps one onto the other so unchanged subtrees keep
  their cached layout/textures.

Stable keys (lists/conditionals) let the reconciler reuse nodes instead of recreating
them ([../ui/components/list.md](../ui/components/list.md)).

> **MVP status.** The reconciler is implemented as a positional diff (no list keys
> yet): each build goes into one of two alternating arenas, so the previous tree
> survives for diffing. Changed nodes (geometry or any visual property) contribute
> their old∪new rect to a damage set; the renderer keeps a retained double-buffer
> shm pool and repaints only the damaged rects (clipped), reporting them with
> `wl_surface_damage_buffer`. Unchanged regions keep their already-painted pixels.
> Per-buffer pending-damage tracking keeps the two pool buffers consistent.

## Layout engine

Single-pass, constraint-light:

- Parents offer available size; children report desired size; `grow`/`Spacer` distribute
  free main-axis space; `align` positions on the cross axis.
- Frames/min-max constrain; safe-area insets are applied at the screen root.
- Designed to avoid multi-pass constraint solving for predictable, fast mobile layout.

## Text

Shaping via **HarfBuzz**, rasterization via **FreeType**, with a glyph atlas cached on the
GPU. Type tokens resolve to concrete font specs per theme
([../overview/design-language.md](../overview/design-language.md)).

## Animation

- Declarative: `withAnimation`/transitions mark properties as animatable; the reconciler
  detects from/to values and drives springs.
- `useAnimatedValue` updates a property **on the render thread**, bypassing rebuilds —
  used for drags and continuous motion ([../guides/animation.md](../guides/animation.md)).
- Composited properties (transform/opacity) are advanced by `zcomp` each vsync.

## Gestures

A recognizer tree mirrors the view tree; libinput events arrive from `zcomp`, are
hit-tested, and dispatched to recognizers that negotiate (tap vs. pan, scroll lock)
([../guides/gestures.md](../guides/gestures.md)).

> **MVP status.** The client binds `wl_seat` and handles pointer + keyboard
> (xkbcommon) events directly in the app loop. Pointer taps hit-test the laid-out
> node tree (deepest `OnTap` under the cursor wins); a single keyboard focus
> target (first focusable node — any `Button`/`OnKey`) receives keys, and
> Enter/Space activates a focused control. The full multi-recognizer negotiation
> (pan/long-press/swipe, scroll lock, focus traversal) is still to come.

## Threading

- **App loop:** build/diff/layout and app/Script state.
- **Render thread:** paint submission + animated-value updates.
- Heavy app compute belongs in worker threads / native modules, not the app loop
  ([../api-reference/conventions.md](../api-reference/conventions.md)).

## Memory

- View nodes: per-build **arena**, bulk-freed each frame.
- Retained scene nodes + GPU resources: reference-counted, released on node removal.

## Zelto Script binding

The Script runtime calls the same C tree-builder under the hood; a Script `VStack(...)`
maps to the native `VStack` node, so both languages share one pipeline
([../zelto-script/runtime.md](../zelto-script/runtime.md)).

## See also

- [compositor-internals.md](compositor-internals.md) · [../api-reference/c/ui.md](../api-reference/c/ui.md)
