# Sheet / Modal / Alert

Presentations layered over the current screen. See
[../../guides/navigation.md](../../guides/navigation.md) for the navigation model.

## Sheet (bottom sheet)

A draggable panel with detents (resting heights).

```js
const [open, setOpen] = useState(false);

Sheet({
  open,
  onClose: () => setOpen(false),
  detents: ["medium", "large"],
}, SheetBody());
```

| Option | Type | Default | Notes |
|---|---|---|---|
| `open` | boolean | — | Presentation state |
| `onClose` | `() => void` | — | Called on dismiss |
| `detents` | array | `["large"]` | `"small"`/`"medium"`/`"large"` or fractions `0..1` |
| `dismissable` | boolean | `true` | Drag-/scrim-to-dismiss |
| `grabber` | boolean | `true` | Show drag handle |

Top corners use `radius.lg` per the design language
([../../overview/design-language.md](../../overview/design-language.md)).

## Modal (full screen)

```js
Modal({ open, onClose: () => setOpen(false) }, EditScreen());
```

Presents full-screen with a slide-up transition; provide your own close affordance (e.g.
a nav bar with a Done button).

## Alert

System-styled confirmation:

```js
Alert({
  open: confirming,
  title: "Delete item?",
  message: "This cannot be undone.",
  actions: [
    { title: "Cancel", role: "cancel" },
    { title: "Delete", role: "destructive", onTap: doDelete },
  ],
});
```

## Dialog

A custom-content centered dialog when an `Alert` is too rigid:

```js
Dialog({ open, onClose }, CustomDialogBody());
```

## Choosing

| Need | Use |
|---|---|
| Contextual panel, partial height, draggable | `Sheet` |
| Full-screen task/flow | `Modal` |
| Yes/no confirmation | `Alert` |
| Custom centered content | `Dialog` |

> Avoid blocking system dialogs in tight loops; drive presentation from state.

## C

```c
Sheet(.open = s->open, .on_close = on_close, .detents = Z_DETENT_MEDIUM | Z_DETENT_LARGE,
  SheetBody());
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
