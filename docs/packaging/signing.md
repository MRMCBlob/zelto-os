# Signing

Every distributed `.zap` is signed. Signing proves who published an app and guarantees
the package wasn't tampered with. Update integrity is tied to the signing key.

## How it works

1. `zelto build` computes `MANIFEST.sha256` — a hash of every file in the package.
2. It signs that manifest with your **private key**, producing `SIGNATURE`
   ([zap-format.md](zap-format.md)).
3. On install, Zelto verifies the signature and every file hash before unpacking
   ([../platform/permissions.md](../platform/permissions.md)).

Signatures use Ed25519 keypairs.

## Create a signing key

```sh
zelto keys create --name "My Publisher"     # generates a keypair in ~/.zelto/keys
zelto keys list
```

Keep the **private key** secret and backed up. Losing it means you can no longer ship
updates that install over your existing app (a new key = a new identity for update
purposes).

## Sign

`zelto build --release` signs automatically with your default key. To sign explicitly or
re-sign:

```sh
zelto sign hello-1.0.0.zap --key my-publisher
```

## Verify

```sh
zelto verify hello-1.0.0.zap
```

Reports the signer, the key fingerprint, and whether all file hashes match.

## Updates & key continuity

An update must be signed with the **same key** as the installed app, and have a higher
`version`/`build`. A package signed with a different key is treated as a different
publisher and won't update the existing install
([zap-format.md](zap-format.md), [manifest.md](manifest.md)).

## Development vs. release

- **Dev builds** (`zelto run`, `zelto build`) use an automatic local dev key — fine for
  the simulator and your own device.
- **Release builds** (`--release`) and store submission require a real publisher key.

## Store submission

The store verifies the signature and may pin the publisher key to the app id on first
submission ([publishing.md](publishing.md)).

## Security notes

- Never commit private keys to source control.
- Use a dedicated, backed-up key per publisher identity.
- CI signing should use a key stored in a secrets manager, not in the repo.
