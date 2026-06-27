# Image

Displays a bundled asset, a remote image, or a system icon.

```js
Image("assets/photo.jpg")          // bundled asset
Image("https://…/pic.jpg")         // remote (cached)
Image.icon("settings")             // system icon set
```

## Options

`Image(source, options?)`

| Option | Type | Default | Notes |
|---|---|---|---|
| `fit` | `"cover" \| "contain" \| "fill" \| "none"` | `"cover"` | Scaling within its frame |
| `placeholder` | view | — | Shown while a remote image loads |
| `tint` | color | — | Recolor (for template icons) |
| `cache` | `"default" \| "reload" \| "none"` | `"default"` | Remote caching policy |

## Sizing

Images need a frame:

```js
Image(url).frame(120, 120).cornerRadius("md").fit("cover");
```

## Icons

```js
Image.icon("home").frame(24, 24).tint("accent");
```

Icons come from the system line-icon set ([../../overview/design-language.md](../../overview/design-language.md)).
For brand/company logos use real brand SVG assets, never the icon set or redrawn paths.

## Adaptive (light/dark)

```js
Image.adaptive("assets/logo-light.png", "assets/logo-dark.png");
```

## Loading & errors

```js
Image(url, { placeholder: Spinner(), onError: () => setBroken(true) });
```

## C

```c
Fit(Z_FIT_COVER, Frame(120, 120, Image("assets/photo.jpg")));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
