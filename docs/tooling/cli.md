# The `zelto` CLI

The command-line tool for the whole app workflow: scaffold, run, build, sign, publish,
and manage devices. Install: [../getting-started/install-sdk.md](../getting-started/install-sdk.md).

```sh
zelto <command> [args] [--flags]
zelto help [command]
```

## Project

| Command | Description |
|---|---|
| `zelto new <name>` | Scaffold a new app (`--template basic\|tabs\|c`) |
| `zelto doctor` | Check toolchain, simulator deps, device link |

## Run & build

| Command | Description |
|---|---|
| `zelto run --simulator` | Build + launch in the desktop simulator (hot reload) |
| `zelto run --device` | Build, install, and launch on a connected device |
| `zelto run --device --watch` | Reinstall on source changes |
| `zelto build` | Produce a debug `.zap` |
| `zelto build --release` | Optimized, signed `.zap` for distribution |

Run flags: `--profile <name>` (simulator device profile), `--port`, `--verbose`.

## Simulator

| Command | Description |
|---|---|
| `zelto simulator profiles` | List device profiles |
| `zelto simulator set <key> <value>` | Mock state: `battery`, `network`, `location` |
| `zelto simulator rotate` | Toggle orientation |

See [simulator.md](simulator.md).

## Devices & APKs

| Command | Description |
|---|---|
| `zelto devices` | List connected devices |
| `zelto install <pkg.zap> --device` | Install a package |
| `zelto apk install <app.apk>` | Install an Android app into the container |
| `zelto apk list` / `zelto apk remove <id>` | Manage Android apps |

See [../getting-started/run-on-device.md](../getting-started/run-on-device.md).

## Signing & publishing

| Command | Description |
|---|---|
| `zelto keys create --name <n>` | Create a signing keypair |
| `zelto keys list` | List keys |
| `zelto sign <pkg.zap> --key <name>` | Sign/re-sign a package |
| `zelto verify <pkg.zap>` | Verify signature + hashes |
| `zelto publish [--channel <c>]` | Publish to the store |

See [../packaging/signing.md](../packaging/signing.md),
[../packaging/publishing.md](../packaging/publishing.md).

## Debug & profile

| Command | Description |
|---|---|
| `zelto debug` | Attach the remote inspector |
| `zelto logs --device` | Stream app logs (`--filter <pattern>`) |
| `zelto profile --device --frames` | Capture frame/GPU/memory profile |

See [debugging.md](debugging.md), [profiling.md](profiling.md).

## Global flags

`--help`, `--version`, `--verbose`, `--json` (machine-readable output for CI).

## Exit codes

`0` success · `1` general error · `2` usage error · `3` build failure · `4` device/IO
error. Useful in CI scripts.
