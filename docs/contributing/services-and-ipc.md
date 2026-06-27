# System Services & IPC

The daemons behind the shell: `zsysd` (lifecycle, permissions, packages) and the **APK
bridge** (Android integration), plus the IPC that ties everything together.

## IPC fabric

- **Wayland protocols** for UI-facing communication (compositor ↔ apps ↔ shell), including
  `wlr-layer-shell` and Zelto custom protocols
  ([compositor-internals.md](compositor-internals.md)).
- **D-Bus (or an equivalent socket RPC)** for service-to-service and app-to-service calls
  (lifecycle, permission requests, package ops, intents).

App-facing IPC is wrapped by `libzelto` / `zelto/platform` so app authors never touch
D-Bus directly ([../api-reference/c/platform.md](../api-reference/c/platform.md)).

## zsysd

The core system daemon. Responsibilities:

### Lifecycle coordination
Tracks app processes and drives state transitions (launch/resume/pause/stop/terminate),
coordinating with the compositor's focus and the app switcher
([../platform/app-lifecycle.md](../platform/app-lifecycle.md)).

### Permission broker
The single authority for permissions. Flow:

```
app: request("camera")
      │  (libzelto → zsysd over IPC)
      ▼
zsysd ── checks manifest declaration
      ── checks stored grant
      ── if prompt needed: asks System UI to show the dialog
      ── records the decision, returns status
```

Grants are bound to the installed package identity
([../platform/permissions.md](../platform/permissions.md)).

### Package manager
Installs/verifies/removes `.zap` packages: verifies signature + hashes, sets up the
sandbox and private data dir, and registers launcher entries, deep-link routes, and share
targets ([../packaging/zap-format.md](../packaging/zap-format.md),
[../packaging/signing.md](../packaging/signing.md)).

### App-to-app intents
Brokers typed `intents.request(...)` calls between apps, enforcing identity and
permissions ([../platform/ipc-and-intents.md](../platform/ipc-and-intents.md)).

## APK bridge

Integrates the Waydroid Android container into the shell:

- **Container control:** start/stop Waydroid, install/list/remove APKs.
- **Surface integration:** Android windows are Wayland surfaces; the bridge assigns
  decoration, placement, and lifecycle so they behave in the shell
  ([../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md)).
- **Permission mapping:** translates Android runtime permissions to/from Zelto permissions
  via the `zsysd` broker so users get one consistent prompt model.
- **Notifications:** forwards Android notifications into the Zelto shade.
- **Links/share:** routes deep links and share intents to Android apps
  ([../platform/apk-interop.md](../platform/apk-interop.md)).

## Other services (reused)

NetworkManager/iwd (network), logind/UPower (power/session), a notification daemon, and a
settings store. Zelto integrates rather than reimplements these
([../overview/architecture-overview.md](../overview/architecture-overview.md)).

## Security boundaries

- Apps are unprivileged and sandboxed; all sensitive operations go through `zsysd`, which
  enforces policy.
- The compositor and System UI are privileged; apps reach them only via Wayland and the
  brokered IPC.
- The Android container is isolated as a whole.

## See also

- [../platform/permissions.md](../platform/permissions.md) · [compositor-internals.md](compositor-internals.md)
- [../platform/apk-interop.md](../platform/apk-interop.md)
