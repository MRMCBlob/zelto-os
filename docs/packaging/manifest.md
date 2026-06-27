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

## See also

- [zap-format.md](zap-format.md) — how the manifest is packaged.
- [../platform/permissions.md](../platform/permissions.md) — permission names.
