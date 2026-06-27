# Component Catalog

Built-in views provided by `zelto/ui` (and the `libzelto` C macros). All are declarative
values that accept options and chained modifiers ([../guides/styling-theming.md](../guides/styling-theming.md)).

## Content

| Component | Purpose | Doc |
|---|---|---|
| `Text` | Text / labels | [components/text.md](components/text.md) |
| `Image` | Images, icons, remote/asset | [components/image.md](components/image.md) |
| `Button` | Tappable action | [components/button.md](components/button.md) |
| `TextField` | Text input | [components/textfield.md](components/textfield.md) |
| `Switch` | On/off toggle | [components/switch.md](components/switch.md) |
| `Slider` | Continuous value | [components/slider.md](components/slider.md) |
| `Progress` / `Spinner` | Progress + loading | [components/progress.md](components/progress.md) |

## Layout

| Component | Purpose | Doc |
|---|---|---|
| `VStack` / `HStack` / `ZStack` | Stacks | [components/stack.md](components/stack.md) |
| `Spacer` | Flexible gap | [components/stack.md](components/stack.md) |
| `Grid` | Grid layout | [components/grid.md](components/grid.md) |
| `Scroll` | Scrollable region | [components/scroll.md](components/scroll.md) |
| `List` | Recycled long lists | [components/list.md](components/list.md) |

## Navigation & presentation

| Component | Purpose | Doc |
|---|---|---|
| `Navigator` / `NavBar` | Stack navigation + bar | [components/navbar.md](components/navbar.md) |
| `TabView` / `TabBar` | Tabs | [components/tabbar.md](components/tabbar.md) |
| `Sheet` / `Modal` / `Alert` | Presentations | [components/sheet.md](components/sheet.md) |

## Conventions

- **Options** are passed as the first argument (an object in Script, designated
  initializers in C). **Children** follow.
- **Modifiers** are chained: `.padding()`, `.font()`, `.onTap()`, etc.
- Components read **design tokens** by default so they match the system theme
  ([../overview/design-language.md](../overview/design-language.md)).
- Every component here has a C equivalent — see [../api-reference/c/ui.md](../api-reference/c/ui.md).

## Example

```js
VStack({ spacing: 12, padding: 16 }, [
  Text("Profile").font("largeTitle"),
  HStack({ spacing: 12 }, [
    Image(user.avatar).frame(56, 56).cornerRadius("full"),
    VStack({ spacing: 2, align: "leading" }, [
      Text(user.name).font("title"),
      Text(user.email).font("callout").foreground("text.secondary"),
    ]),
    Spacer(),
    Button.icon("settings", openSettings),
  ]),
]);
```
