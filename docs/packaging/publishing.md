# Publishing & Distribution

Ways to get a `.zap` onto users' devices: direct sideload, hosted download, and the Zelto
store.

## 1. Sideload

The simplest path — share the `.zap` and install it directly:

```sh
zelto install hello-1.0.0.zap --device
```

Or the user opens a `.zap` file on-device to install it. Sideloaded apps still require a
valid signature ([signing.md](signing.md)) and go through the same sandbox/permission
setup ([../platform/permissions.md](../platform/permissions.md)).

## 2. Hosted / self-distribution

Host the signed `.zap` anywhere (your site, a release page). Provide a `zelto://install`
link or a download; the system verifies the signature on install. Good for betas and
enterprise distribution.

## 3. Zelto store

Submit to the store for discovery and managed updates:

```sh
zelto publish                       # uploads the release .zap from the current app
zelto publish --channel beta        # release channel
```

`zelto publish`:
1. Builds a release `.zap` if needed (`--release`).
2. Verifies signature + manifest.
3. Uploads and creates/updates the store listing.

### First submission

- The publisher key is pinned to the `app.id` ([signing.md](signing.md)); future updates
  must use the same key.
- Provide listing metadata (description, screenshots, category) via `zelto publish` flags
  or the developer portal.

### Updates

Bump `version`/`build` ([manifest.md](manifest.md)), rebuild, and `zelto publish` again.
The store rolls the update to users; private app data is preserved
([zap-format.md](zap-format.md)).

### Channels

`stable`, `beta`, `internal` — publish to a channel and promote when ready.

## Review & policy

Store submissions are checked for signature validity, manifest correctness, and policy
(permissions justified, no deceptive metadata). Sideload/self-host bypass store review but
not signature verification.

## Versioning checklist

- [ ] `version` increased (semver)
- [ ] `build` incremented
- [ ] Signed with the same publisher key
- [ ] Tested on device ([../getting-started/run-on-device.md](../getting-started/run-on-device.md))

## See also

- [../tooling/cli.md](../tooling/cli.md) — `publish` flags.
- [signing.md](signing.md) · [zap-format.md](zap-format.md)
