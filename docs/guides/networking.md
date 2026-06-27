# Networking

Zelto apps make network requests with an async `fetch`-style API plus WebSocket support.
All networking is asynchronous and requires the `network` permission.

> Declare `network = true` in `zelto.toml` ([../packaging/manifest.md](../packaging/manifest.md)).

## HTTP requests

```js
import { fetch } from "zelto/net";

async function loadUser(id) {
  const res = await fetch(`https://api.example.com/users/${id}`);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return await res.json();
}
```

`fetch(url, options)` options: `method`, `headers`, `body`, `timeout`, `signal` (for
cancellation). The response has `ok`, `status`, `headers`, and `.json()` / `.text()` /
`.bytes()`.

```js
await fetch(url, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify({ name }),
  timeout: 10000,
});
```

## Using data in a component

Combine with state + effect ([state-management.md](state-management.md)):

```js
function UserView({ id }) {
  const [user, setUser] = useState(null);
  const [error, setError] = useState(null);

  useEffect(() => {
    const ac = new AbortController();
    fetch(`https://api.example.com/users/${id}`, { signal: ac.signal })
      .then(r => r.json()).then(setUser).catch(setError);
    return () => ac.abort();      // cancel on unmount
  }, [id]);

  if (error) return Text("Failed to load").foreground("text.secondary");
  if (!user) return Spinner();
  return Text(user.name).font("title");
}
```

## WebSockets

```js
import { WebSocket } from "zelto/net";

const ws = new WebSocket("wss://example.com/feed");
ws.onMessage((msg) => append(msg));
ws.onClose(() => reconnectLater());
ws.send(JSON.stringify({ subscribe: "prices" }));
```

## Connectivity

```js
import { network } from "zelto/net";

const status = network.status();      // { online, type: "wifi"|"cellular"|"none" }
network.onChange((s) => setOnline(s.online));
```

Handle offline gracefully; test with `zelto simulator set network none`
([../getting-started/simulator.md](../getting-started/simulator.md)).

## Downloads & uploads

```js
import { download, upload } from "zelto/net";

const file = await download(url, { onProgress: p => setPct(p) });   // to app storage
await upload(url, fileHandle, { onProgress: p => setPct(p) });
```

Downloaded files land in app storage — see [storage.md](storage.md).

## C API

```c
ZNetRequest *req = z_net_get("https://api.example.com/x");
z_net_send(req, on_response, userdata);   // async; callback on the app loop
```

See [../api-reference/c/system.md](../api-reference/c/system.md).

## Notes

- Requests run off the UI thread; callbacks/`await` resume on the app loop.
- Respect the user's connection (metered/cellular); avoid large background transfers
  without a declared background capability ([background-tasks.md](background-tasks.md)).

## Next

- [storage.md](storage.md)
- [background-tasks.md](background-tasks.md)
