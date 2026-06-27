# TabView / TabBar

A bottom tab bar that switches between top-level sections. Each tab keeps its own
navigation stack ([../../guides/navigation.md](../../guides/navigation.md)).

```js
import { TabView } from "zelto/ui";

TabView([
  { title: "Home", icon: "home", view: HomeScreen },
  { title: "Search", icon: "search", view: SearchScreen },
  { title: "Profile", icon: "user", view: ProfileScreen },
]);
```

## Tab item fields

| Field | Type | Notes |
|---|---|---|
| `title` | string | Label under the icon |
| `icon` | string | System icon name |
| `view` | component | The tab's root screen |
| `badge` | number \| string | Optional badge |

## Controlled selection

```js
const [tab, setTab] = useState(0);
TabView(tabs, { selected: tab, onSelect: setTab });
```

Tapping the active tab again pops its stack to root (standard behavior).

## Badges

```js
{ title: "Inbox", icon: "mail", view: InboxScreen, badge: unread || null }
```

## Styling

The bar uses a translucent, blurred surface and the accent token for the selected tab,
per the design language ([../../overview/design-language.md](../../overview/design-language.md)).
It respects the bottom safe-area inset automatically.

## C

```c
TabView(
  Tab("Home", "home", HomeScreen),
  Tab("Search", "search", SearchScreen),
  Tab("Profile", "user", ProfileScreen));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
