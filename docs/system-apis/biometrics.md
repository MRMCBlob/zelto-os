# Biometrics

Authenticate the user with fingerprint/face (where the device supports it), and gate
secure-store items behind biometric verification.

## Authenticate

```js
import { biometrics } from "zelto/platform";

const ok = await biometrics.authenticate("Unlock your notes");
if (ok) showNotes();
```

`authenticate(reason)` shows the system prompt and resolves to a boolean. The `reason`
string is shown to the user.

## Capability check

```js
const cap = await biometrics.available();
// cap = { supported, type: "fingerprint" | "face" | "none", enrolled }
```

Fall back to a passcode/app password when `supported` is false or `enrolled` is false.

## Gate secret access

Bind a stored secret to biometric verification via the secure store
([../guides/storage.md](../guides/storage.md)):

```js
import { secure } from "zelto/storage";

await secure.set("auth_token", token, { requireBiometric: true });
const token = await secure.get("auth_token");   // prompts biometrics; null if denied
```

## Security notes

- Biometric matching happens in the platform's secure subsystem; your app receives only a
  success/failure result, never biometric data.
- Always provide a non-biometric fallback (device passcode / app login).
- Don't store secrets in plain `prefs`/`files` — use `secure`
  ([../api-reference/script/storage.md](../api-reference/script/storage.md)).

## Availability

Biometric hardware support depends on the device and its port; many early Zelto target
devices may expose only a passcode fallback.
