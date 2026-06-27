# Stacks (VStack / HStack / ZStack / Spacer)

Stacks are the primary layout primitive. See [../../guides/layout.md](../../guides/layout.md)
for the full layout model.

## VStack — vertical

```js
VStack({ spacing: 12, align: "leading", padding: 16 }, [
  Text("Title").font("title"),
  Text("Body").font("body"),
]);
```

## HStack — horizontal

```js
HStack({ spacing: 8, align: "center" }, [
  Image.icon("star"),
  Text("Favorite"),
]);
```

## ZStack — overlapping (depth)

```js
ZStack({ align: "center" }, [
  Image(cover).fit("cover"),
  Text("Overlay").foreground("#fff"),
]);
```

## Options

| Option | Type | Applies to | Notes |
|---|---|---|---|
| `spacing` | number \| token | V/H | Gap between children |
| `align` | `leading`/`center`/`trailing` | all | Cross-axis alignment |
| `justify` | `start`/`center`/`end`/`between` | V/H | Main-axis distribution of free space |
| `padding` | number \| object \| token | all | Inner padding |

## Spacer

A flexible gap that consumes free space along the main axis:

```js
HStack({}, [ Text("Left"), Spacer(), Text("Right") ]);
```

Use `.grow(n)` on children for weighted distribution instead of/with `Spacer`.

## Nesting

Stacks nest freely to build any layout:

```js
HStack({ spacing: 12 }, [
  Avatar(),
  VStack({ spacing: 2, align: "leading" }, [
    Text(name).font("body"),
    Text(subtitle).font("caption").foreground("text.secondary"),
  ]),
  Spacer(),
  Button.icon("chevron-right", open),
]);
```

## C

```c
VStack(.spacing = 12, .align = Z_ALIGN_LEADING,
  Text("Title"),
  Spacer(),
  Text("Body"));
```

See [../../api-reference/c/ui.md](../../api-reference/c/ui.md).
