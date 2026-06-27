# Clipboard

Read and write the system clipboard. API: `zelto/platform`
([../api-reference/script/permissions.md](../api-reference/script/permissions.md)).

```js
import { clipboard } from "zelto/platform";

await clipboard.setText("copied!");
const text = await clipboard.getText();
```

## Rich content

```js
await clipboard.set({ text, url, image });    // multiple representations
const data = await clipboard.get();           // { text?, url?, image? }
await clipboard.hasText();                     // boolean
await clipboard.clear();
```

## Privacy

- Reading the clipboard may surface a system "pasted from …" indicator; read only in
  response to an explicit paste action, not on every screen.
- Don't poll the clipboard in the background.

## Paste UI

`TextField` provides paste through the standard edit menu automatically
([../ui/components/textfield.md](../ui/components/textfield.md)); use the API directly
only for custom paste affordances.
