# Navigation

Zelto navigation is **stack-based**, with tabs, sheets, and modals layered on top. It
integrates the system back gesture and supports shared-element transitions.

## Navigation stack

Wrap your app in a `Navigator` and push/pop screens:

```js
import { Navigator, useNavigation } from "zelto/ui";

export default function App() {
  return Navigator({ root: HomeScreen });
}

function HomeScreen() {
  const nav = useNavigation();
  return Button("Open details", () => nav.push(DetailScreen, { id: 42 }));
}

function DetailScreen({ id }) {
  const nav = useNavigation();
  return VStack({ padding: 16 }, [
    Text(`Item ${id}`).font("title"),
    Button("Back", () => nav.pop()),
  ]);
}
```

- `nav.push(Component, props)` — push a screen.
- `nav.pop()` / `nav.popTo(Component)` / `nav.popToRoot()`.
- `nav.replace(Component, props)` — swap the current screen.

Pushes animate with the standard spring; the edge-swipe-back gesture pops automatically.

## Navigation bar

Screens declare a title and bar items:

```js
DetailScreen.options = {
  title: "Details",
  largeTitle: true,
  trailing: () => Button.icon("share", share),
};
```

See [../ui/components/navbar.md](../ui/components/navbar.md).

## Tabs

```js
import { TabView } from "zelto/ui";

TabView([
  { title: "Home", icon: "home", view: HomeScreen },
  { title: "Search", icon: "search", view: SearchScreen },
  { title: "Profile", icon: "user", view: ProfileScreen },
]);
```

Each tab has its own navigation stack. See [../ui/components/tabbar.md](../ui/components/tabbar.md).

## Sheets & modals

```js
const [showSheet, setShowSheet] = useState(false);

return VStack({}, [
  Button("Show", () => setShowSheet(true)),
  Sheet({ open: showSheet, onClose: () => setShowSheet(false), detents: ["medium", "large"] },
    SheetContent()),
]);
```

- `Sheet` — bottom sheet with drag-to-dismiss and detents.
- `Modal` — full-screen modal presentation.
- `Alert` / `Dialog` — system-styled prompts.

See [../ui/components/sheet.md](../ui/components/sheet.md).

## Passing & returning data

Pass data forward via props. Return data via a callback prop or a shared store
([state-management.md](state-management.md)):

```js
nav.push(PickColorScreen, { onPick: (c) => setColor(c) });
```

## Deep links

Map URLs/intents to screens so links and other apps can navigate in:

```js
Navigator({
  root: HomeScreen,
  routes: {
    "/item/:id": (params) => [DetailScreen, { id: params.id }],
  },
});
```

Incoming links (including from Android apps) are delivered here. See
[../platform/ipc-and-intents.md](../platform/ipc-and-intents.md).

## Back guard

Intercept back/pop to confirm unsaved changes:

```js
useBackGuard(() => !dirty);   // false cancels the pop
```

## Shared-element transitions

Tag matching views in the source and destination screens:

```js
// list:    Image(url).sharedElement(`photo-${id}`)
// detail:  Image(url).sharedElement(`photo-${id}`)
```

The navigator animates the element between screens. See [animation.md](animation.md).

## C

A `Navigator` owns the screen stack; screens are `ZView fn(ZApp *app, void *props)`.
It takes `app` first (it owns persistent stack state). Push from a tap handler,
passing a pointer that outlives the screen; pop with `z_nav_pop`, the Escape /
Backspace key, or a left-edge swipe.

```c
static ZView detail_screen(ZApp *app, void *props) {
    Item *it = props;
    return VStack(Text("Item %d", it->id), Button(go_back, "Back"), .padding = 16);
}
static void go_back(ZApp *app, void *state) { z_nav_pop(z_navigation(app)); }

static void open_item(ZApp *app, void *state, void *data) {        // OnTapData
    z_nav_push(z_navigation(app), detail_screen, data);           // data = the Item*
}

static ZView body(ZApp *app, void *state) {
    return Navigator(app, .root = list_screen);
}
```

Pushes/pops animate with the standard spring as a horizontal slide (the screen
below parallaxes). See [animation.md](animation.md) and
[../api-reference/c/ui.md](../api-reference/c/ui.md).

## Next

- [../ui/components/navbar.md](../ui/components/navbar.md)
- [../platform/app-lifecycle.md](../platform/app-lifecycle.md)
