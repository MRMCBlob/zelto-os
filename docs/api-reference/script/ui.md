# Zelto Script API: `zelto/ui`

Components, modifiers, and UI hooks. Import from `"zelto/ui"`. Each component also has a
catalog page under [../../ui/components/](../../ui/components/) with examples.

## Components

| Component | Signature | Catalog |
|---|---|---|
| `Text` | `Text(content, options?)` | [text](../../ui/components/text.md) |
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

## Theme provider

```js
Theme({ tokens: { "color.accent": {...}, "radius.md": 18 } }, child)
```

See [../../guides/styling-theming.md](../../guides/styling-theming.md).

## Rich text

```js
Text.rich([{ text: "…", weight, color, italic }, ...])
```
