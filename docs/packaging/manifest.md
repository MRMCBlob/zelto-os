# Manifest (`zelto.toml`)

Every app has a `zelto.toml` at its root describing identity, entry point, permissions,
and capabilities. This is the **single source of truth** for app metadata; other docs
reference these field names.

## Full schema

```toml
[app]
id = "dev.example.hello"     # reverse-DNS, globally unique, immutable
name = "Hello"               # display name
version = "1.0.0"            # semver; must increase to update
build = 1                    # integer build number (increments per release)
icon = "assets/icon.png"     # app icon (≥ 512px square)
entry = "src/main.js"        # JS entry, or "lib" for a native (C) app
minOS = "1.0"                # minimum Zelto OS version (optional)

[permissions]               # see ../platform/permissions.md
network = false
notifications = false
camera = false
microphone = false
location = "off"             # off | when-in-use | always
contacts = false
biometrics = false

[capabilities]
background = []              # subset of: "audio", "location", "downloads"
share-targets = []           # MIME globs this app accepts, e.g. ["text/*"]

[links]                      # deep linking (../platform/ipc-and-intents.md)
schemes = []                 # custom URL schemes, e.g. ["myapp"]
hosts = []                   # https hosts that open the app

[[provides]]                 # app-to-app actions this app handles (repeatable)
action = "pickFile"

[[native]]                   # native C modules (repeatable)
name = "imgproc"
src = ["native/imgproc.c"]
```

## Required vs. optional

| Field | Required | Notes |
|---|---|---|
| `app.id` | yes | Immutable identity; reverse-DNS |
| `app.name` | yes | Display name |
| `app.version` | yes | Semver |
| `app.entry` | yes | `src/*.js` or `"lib"` |
| `app.icon` | yes | App icon |
| `app.build` | recommended | Integer, increments per release |
| everything else | no | Defaults shown above |

## Rules

- **`id` is permanent.** Changing it creates a different app. Use reverse-DNS you control.
- **`version` must increase** for an update to install over an existing one
  ([signing.md](signing.md)).
- **Declared ≠ granted.** Permissions here only let the app *request* access at runtime
  ([../platform/permissions.md](../platform/permissions.md)).
- **Unknown keys are rejected** by `zelto build` to catch typos.

## Validation

`zelto build` and `zelto doctor` validate the manifest (schema, icon size, semver,
permission names). Fix reported errors before packaging.

## The runtime manifest (`.app`)

`zelto.toml` is the *authoring* format. What actually ships in the image (and inside a
`.zap`) is a flat `key=value` file — `/usr/share/zelto/apps/<app>.app` — which the
launcher and `zsysd` scan at boot. It is deliberately trivial to parse: no TOML reader
in the boot path.

```ini
id=os.zelto.notepad
name=Notepad
subtitle=Persistent prefs + SQLite
exec=/usr/bin/zelto-notepad
icon=/usr/share/zelto/apps/icons/Notepad.png
color=2e9bff
permissions=notifications,network
share_targets=text/plain
links=zelto
no_snapshot=1
```

### `no_snapshot=` — opting out of window thumbnails

The App Switcher shows each backgrounded window as a picture of itself, taken by the
compositor the moment the window stopped being the foreground one
([compositor-internals.md](../contributing/compositor-internals.md)). An app that puts
something on screen it would rather not have photographed — a banking balance, a
password manager's vault, a medical record — declares `no_snapshot=1` and gets its icon
on the card instead.

The opt-out is enforced by *never taking* the picture, not by taking it and declining to
send it. There is then no copy of those pixels anywhere for a later bug to leak.

**Where it is enforced, and why there.** `zcomp` does not read manifests and does not
start now. `zsysd` already parses them — for `permissions=`, `share_targets=` and
`links=` — already merges the baked-in `/usr/share/zelto/apps` with the runtime-installed
directory, and already rebuilds the table when `zelto-install` sends it `{"op":"reload"}`.
Duplicating that inside the compositor would mean a second parser, a second directory
merge and a filesystem watch, all in the process that must never block. So `zcomp` asks,
over the same `zsysd.sock` it already uses for the media keys, with a read-only
`{"op":"snapshot_policy","app_id":"…"}`.

It asks **once per window, at map** — the point where it first learns the `app_id` — and
caches the answer on the toplevel. The capture itself happens inside a focus change, on
the frame an app-switch animation starts, and a blocking socket round-trip there would
put `zsysd`'s health on the compositor's frame budget.

It is a dedicated op rather than a `sys.*` setting because `settings_set` is ungated: any
client may write any key, so a deny-list published as a setting could be cleared by the
very app it restrains. A read-only op has no such write path.

An unreachable broker resolves to **allowed**, matching every other `zsysd` fallback in
the system. Denying on error would let a broker hiccup silently empty the App Switcher —
a visible breakage of a working feature, traded for protecting a flag almost nothing sets.

`exec=` is a **command, not just a path**: it is split on whitespace and executed
directly (no shell). That is what makes an interpreted app launchable by the same
machinery as a native one — a [Zelto Script](../zelto-script/runtime.md) app has no
binary of its own, so it names the shared runtime and its entry file:

```ini
id=os.zelto.jsdemo
name=JS Demo
exec=/usr/bin/zelto-script --id os.zelto.jsdemo /usr/share/zelto/scripts/jsdemo.js
```

Passing `--id` keeps one identity across the shell: the window's `app_id`, the manifest,
the switcher entry, and the permission grants all agree. Because there is no shell, a
path containing spaces cannot be expressed (see `system/common/exec_cmd.h`).

### `script=` — inside a package

The `exec=` above is what an app installed *as part of the image* looks like. A script
app that ships as a signed `.zap` does **not** write `exec=` at all. It declares only its
entry file, and the installer synthesises the command:

```ini
id=os.zelto.greeter
name=Greeter
version=1.0.0
script=script/greeter.js      # added by meta/mkzap.sh; the in-package path
```

`zelto-install` then writes the runtime manifest with
`exec=/usr/bin/zelto-script --id os.zelto.greeter /var/zelto/installed/os.zelto.greeter/greeter.js`.

The split matters for more than tidiness: **the package does not get to name its own
interpreter.** If a signed manifest could set `exec=` freely, it could point at any binary
on the device and the signature would faithfully attest to it. So the author supplies the
`.js` and the installer decides what runs it. The `.js` itself is covered by
`MANIFEST.sha256` and the Ed25519 signature exactly as a native binary is — a flipped byte
in the script is rejected at install, just like a tampered ELF
([zap-format.md](zap-format.md), [signing.md](signing.md)).

A script package also carries **no ABI**: with no compiled code in it, the same `.zap`
installs on the aarch64 device and in the x86_64 simulator.

## See also

- [zap-format.md](zap-format.md) — how the manifest is packaged.
- [../platform/permissions.md](../platform/permissions.md) — permission names.
