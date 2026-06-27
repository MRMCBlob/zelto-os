# Permissions & Security Model

Zelto apps are sandboxed and request access to sensitive capabilities explicitly. This
page is the source of truth for permission names and the security model.

## Model

- Each app runs in its own **sandboxed process** (Linux namespaces + seccomp) with a
  **private data directory** no other app can read.
- Capabilities are **declared** in the manifest and **granted at runtime** by the user
  through a system prompt brokered by `zsysd`.
- The compositor and System UI are privileged; apps reach the system only through Wayland
  protocols and the `zsysd` IPC broker
  ([../overview/architecture-overview.md](../overview/architecture-overview.md)).

## Declaring permissions

In `zelto.toml` ([../packaging/manifest.md](../packaging/manifest.md)):

```toml
[permissions]
network = true
notifications = true
camera = false
location = "when-in-use"     # off | when-in-use | always

[capabilities]
background = ["audio"]
share-targets = ["text/*"]
```

Declaring a permission does **not** grant it — it makes the app *able to request* it.

## Permission names

| Name | Gates |
|---|---|
| `network` | HTTP / WebSocket / transfers |
| `notifications` | Posting notifications |
| `camera` | Camera capture / preview |
| `microphone` | Audio recording |
| `location` | Location (`when-in-use` / `always`) |
| `contacts` | Reading/searching contacts |
| `biometrics` | Biometric authentication |

Some APIs need no permission because they use a **picker** that implies user intent
(image picker, contact picker, share sheet).

## Requesting at runtime

```js
import { permissions } from "zelto/platform";

const status = await permissions.status("camera");   // "granted"|"denied"|"prompt"
if (status !== "granted") {
  const result = await permissions.request("camera"); // shows the system prompt
}
```

C: `z_perm_status` / `z_perm_request`
([../api-reference/c/platform.md](../api-reference/c/platform.md)).

### Best practices

- **Request in context** — when the user triggers the feature, not at launch.
- **Explain first** if the reason isn't obvious.
- **Degrade gracefully** when denied; never block the whole app on an optional permission.

## Background capabilities

`[capabilities] background` enables specific long-running modes (`audio`, `location`,
`downloads`); undeclared modes are denied
([../guides/background-tasks.md](../guides/background-tasks.md)).

## Package integrity

`.zap` packages are **signed**; signatures and the manifest are verified on install, and
the granted permission set is bound to the installed package
([signing](../packaging/signing.md)).

## Android apps

APK permissions are mapped to Zelto permissions by the APK bridge so the user sees one
consistent prompt model; the Android container is isolated as a whole
([apk-interop.md](apk-interop.md)).

## See also

- [../packaging/manifest.md](../packaging/manifest.md) · [../packaging/signing.md](../packaging/signing.md)
- [../contributing/services-and-ipc.md](../contributing/services-and-ipc.md) — the broker.
