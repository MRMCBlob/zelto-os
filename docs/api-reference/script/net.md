# Zelto Script API: `zelto/net`

HTTP, WebSockets, transfers, and connectivity. Requires the `network` permission
([../../packaging/manifest.md](../../packaging/manifest.md)). Guide:
[../../guides/networking.md](../../guides/networking.md).

## `fetch(url, options?) → Promise<Response>`

```js
import { fetch } from "zelto/net";
const res = await fetch(url, { method, headers, body, timeout, signal });
```

Options:

| Field | Type | Notes |
|---|---|---|
| `method` | string | Default `"GET"` |
| `headers` | object | Request headers |
| `body` | string \| bytes | Request body |
| `timeout` | number (ms) | Abort after timeout |
| `signal` | AbortSignal | Cancellation |

Response:

| Member | Type | Notes |
|---|---|---|
| `ok` | boolean | `status` in 200–299 |
| `status` | number | HTTP status |
| `headers` | object | Response headers |
| `.json()` | `Promise<any>` | Parse JSON |
| `.text()` | `Promise<string>` | Body as text |
| `.bytes()` | `Promise<Uint8Array>` | Raw body |

## Cancellation

```js
const ac = new AbortController();
fetch(url, { signal: ac.signal });
ac.abort();
```

## WebSocket

```js
import { WebSocket } from "zelto/net";
const ws = new WebSocket(url);
ws.onOpen(fn); ws.onMessage(fn); ws.onClose(fn); ws.onError(fn);
ws.send(data);
ws.close();
```

## Transfers

```js
import { download, upload } from "zelto/net";
const handle = await download(url, { onProgress: p => …, to: "downloads/file" });
await upload(url, fileHandle, { onProgress: p => … });
```

Files land in app storage ([storage.md](storage.md)).

## Connectivity

```js
import { network } from "zelto/net";
network.status();            // { online, type: "wifi"|"cellular"|"none" }
network.onChange(fn);        // returns unsubscribe
```
