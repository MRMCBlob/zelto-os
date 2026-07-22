# Zelto Script API: `zelto/ui`

Components, modifiers, and UI hooks. Import from `"zelto/ui"`. Each component also has a
catalog page under [../../ui/components/](../../ui/components/) with examples.

## Components

| Component | Signature | Catalog |
|---|---|---|
| `Text` | `Text(content, options?)` | [text](../../ui/components/text.md) |
| `WrapText` | `WrapText(content, width, size?)` | prose broken to a pixel column |
| `EllipsizeText` | `EllipsizeText(content, width, size?)` | one line, cut, real `…` |
| `Image` | `Image(source, options?)`, `Image.icon(name)`, `Image.adaptive(l, d)` | [image](../../ui/components/image.md) |
| `Button` | `Button(title, onTap, options?)`, `Button.icon(name, onTap)` | [button](../../ui/components/button.md) |
| `TextField` | `TextField(options)` | [textfield](../../ui/components/textfield.md) |
| `Switch` | `Switch(options)` | [switch](../../ui/components/switch.md) |
| `Slider` | `Slider(options)` | [slider](../../ui/components/slider.md) |
| `Progress` / `Spinner` | `Progress(options)` / `Spinner(options?)` | [progress](../../ui/components/progress.md) |
| `VStack`/`HStack`/`ZStack` | `(options?, children)` | [stack](../../ui/components/stack.md) |
| `Spacer` | `Spacer()` | [stack](../../ui/components/stack.md) |
| `Grid` | `Grid(options, children)` | [grid](../../ui/components/grid.md) |
| `Scroll` | `Scroll(options?, child)` | [scroll](../../ui/components/scroll.md) |
| `List` | `List(options)` | [list](../../ui/components/list.md) |
| `Navigator` | `Navigator(options)` | [navbar](../../ui/components/navbar.md) |
| `TabView` | `TabView(tabs, options?)` | [tabbar](../../ui/components/tabbar.md) |
| `Sheet`/`Modal`/`Alert`/`Dialog` | `(options, child?)` | [sheet](../../ui/components/sheet.md) |

### `Text` does not wrap and does not truncate

It measures to ONE line however long the string is — the toolkit does not wrap it,
clip it, or warn — so a paragraph in a fixed-width card paints straight through
the edge. That is not hypothetical: the JS Demo's two paragraphs did exactly that
until P46, because a script app had no way to wrap at all.

Both replacements need the **width at build time** (the split happens against the
real font, before any layout pass exists), so read it from `appSize()` rather than
writing a number:

```js
const textW = () => appSize().width - 2 * PAGE_PAD;

WrapText("A long description that will not fit on one line.", textW(), Font.subhead)
EllipsizeText(`saved: ${note.text}`, textW(), Font.caption)
```

Which one depends on **who wrote the string**. Prose you wrote wraps. A filename,
an app id, a name that arrived from somewhere else goes in a row of fixed height
and has to be cut, or the row grows by however many lines the data happens to
need — which is the case a script app hits most, since almost everything it
displays came from elsewhere.

## Modifiers

Chainable methods on any view. Return a new view; order matters (wrap outward).

### Layout

`.frame(w, h)` · `.frame({width,height,minWidth,maxWidth,minHeight,maxHeight})` ·
`.padding(value)` · `.grow(n)` · `.offset(x, y)` · `.aspectRatio(r)` · `.hidden(bool)`

### Style

`.font(token|spec)` · `.foreground(color)` · `.background(color|view)` ·
`.cornerRadius(token|n)` · `.border(width, color)` · `.shadow(token)` · `.opacity(n)` ·
`.blur(radius)` · `.tint(color)`

### Interaction

`.onTap(fn)` · `.onLongPress(fn)` · `.onDoubleTap(fn)` · `.onPan(fn)` · `.onPanEnd(fn)` ·
`.onSwipe(dir, fn)` · `.disabled(bool)` · `.hitSlop(n)` · `.a11yLabel(str)`

> **Status.** Bound today: `.onTap(fn)`, `.onPan(fn)` and `.onLongPress(fn)`, plus the
> offset modifiers `.offset(animated, y)`, `.offsetXY(x, y)` and `.offsetXYAnimated(x, y)`.
> An `onPan` handler receives `{ x, y, dx, dy, vx, vy, phase }` — `dx`/`dy` are the
> translation since the gesture began and `vx`/`vy` the release velocity (px/s) to hand to
> `.fling()`. `.onDoubleTap`, `.onSwipe`, `.hitSlop` and `.disabled` are not bound yet.

### Presentation / animation

`.transition(name|spec)` · `.sharedElement(key)` · `.sticky()` · `.swipeActions(spec)`

See [../../guides/styling-theming.md](../../guides/styling-theming.md),
[../../guides/gestures.md](../../guides/gestures.md),
[../../guides/animation.md](../../guides/animation.md).

## UI hooks

```js
useNavigation()      // { push, pop, popTo, popToRoot, replace }
useColorScheme()     // "light" | "dark"
useSafeArea()        // { top, bottom, leading, trailing }
useAnimatedValue(n)  // animated value: .set(n), .spring(n), .get()
useScroll()          // scroll handle: .scrollTo({x,y,animated})
useFocus()           // focus handle: .focus(), .blur()
useBackGuard(fn)     // intercept back/pop; return false to cancel
useTheme()           // resolved tokens for the current theme
```

> **Status.** Bound today (all from the `zelto` module):
>
> ```js
> useNavigation()      // { push(component, props), pop(), depth } — also exported
>                      // as `navigation`. Back (edge-swipe / Escape) pops on its own.
> useAnimatedValue(n)  // .get() .set(n) .pin(n) .spring(n, token) .grab() .fling(n, v)
> useTextField(text)   // { text, setText } — bind with TextField(field, placeholder)
> ```
>
> A spring takes a NAMED motion token (`Spring.standard` / `snappy` / `press`), never raw
> numbers, and Reduce Motion collapses every one of them to a jump inside the toolkit — a
> script gets that for free. `.grab()` / `.fling()` are the interruptible-drag pair: grab a
> spring mid-flight, drive it 1:1 from the finger, then release it with the finger's own
> velocity. `useColorScheme`, `useSafeArea`, `useScroll`, `useFocus`, `useBackGuard` and
> `useTheme` are not bound yet.

## Theme provider

```js
Theme({ tokens: { "color.accent": {...}, "radius.md": 18 } }, child)
```

See [../../guides/styling-theming.md](../../guides/styling-theming.md).

## Rich text

```js
Text.rich([{ text: "…", weight, color, italic }, ...])
```
