# The `.zap` Package Format

A `.zap` is the distributable unit of a Zelto app: a signed archive containing the
manifest, compiled code, assets, and native modules.

## Layout

A `.zap` is a ZIP archive with this structure:

```
hello-1.0.0.zap
├── zelto.toml              # the manifest (../packaging/manifest.md)
├── bundle/
│   └── app.qbc             # Zelto Script compiled to QuickJS bytecode
├── native/                 # native C modules, per ABI (if any)
│   └── aarch64/
│       └── imgproc.so
├── assets/                 # images, fonts, data bundled with the app
│   ├── icon.png
│   └── ...
├── MANIFEST.sha256         # content hashes of every file above
└── SIGNATURE               # signature over MANIFEST.sha256
```

- **`bundle/app.qbc`** — your Script modules, bundled and precompiled to bytecode so the
  app doesn't parse source at launch ([../zelto-script/runtime.md](../zelto-script/runtime.md)).
- **`native/<abi>/`** — native modules built per target ABI (`aarch64` for devices; the
  host ABI is used by the simulator) ([../guides/interop-c-and-script.md](../guides/interop-c-and-script.md)).
- **`MANIFEST.sha256` + `SIGNATURE`** — integrity and authenticity
  ([signing.md](signing.md)).

A pure-C app has no `bundle/` and ships its UI in `native/`.

### Today: a script app ships its source

Until `zelto build` exists to produce `bundle/app.qbc`, a Zelto Script package carries its
entry file as plain source and the manifest names it with `script=`:

```
greeter-1.0.0.zap
├── zelto.toml              # id, name, version, permissions… + script=script/greeter.js
├── script/
│   └── greeter.js          # the entry module (source; bytecode is Planned)
├── MANIFEST.sha256
└── SIGNATURE
```

Two properties follow, and both are the point:

- **No ABI.** A script package contains no compiled code, so the same `.zap` installs on
  the aarch64 device and in the x86_64 simulator. It runs on the shared Zelto Script
  runtime already present on the system, which is why it ships no binary of its own.
- **The code is signed like any other code.** The `.js` is listed in `MANIFEST.sha256` and
  covered by the signature, so a flipped byte in the script is rejected at install exactly
  as a tampered ELF is. Source is not a weaker link than a binary here.

The package deliberately does **not** carry `exec=` — `zelto-install` synthesises the
command that runs the entry, so a signed manifest cannot nominate its own interpreter
([manifest.md](manifest.md)). Build one with `meta/mkzap.sh <manifest> <entry.js> <out.zap>`
(the dev stand-in for `zelto build`).

## Building

```sh
zelto build            # debug build → hello-1.0.0.zap (unsigned/dev-signed)
zelto build --release  # optimized + signed for distribution
```

`zelto build`:
1. Validates `zelto.toml`.
2. Bundles + compiles Script to `app.qbc`.
3. Cross-compiles native modules per ABI.
4. Collects assets.
5. Writes `MANIFEST.sha256` and signs it ([signing.md](signing.md)).

See [../tooling/cli.md](../tooling/cli.md).

## Installation

On install, Zelto:
1. Verifies the `SIGNATURE` against `MANIFEST.sha256`.
2. Verifies each file hash.
3. Reads the manifest, sets up the sandbox + private data dir, and registers launcher
   entries, links, and share targets.

See [../getting-started/run-on-device.md](../getting-started/run-on-device.md).

## Updates

An installed app is updated by a `.zap` with the **same `app.id`**, a **higher
`version`/`build`**, and a signature from the **same key**
([signing.md](signing.md)). Private data is preserved across updates.

## Size & assets

- Keep `assets/` lean; load large media remotely where possible.
- Provide a single high-res `icon.png`; the system derives sizes.

## See also

- [manifest.md](manifest.md) · [signing.md](signing.md) · [publishing.md](publishing.md)
