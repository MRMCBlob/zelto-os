# Storage

Zelto gives each app a private, sandboxed data area plus several storage APIs: key-value
preferences, files, a SQLite database, and a secure keystore.

> App storage is per-app and isolated; other apps cannot read it
> ([../platform/permissions.md](../platform/permissions.md)).

## Preferences (key-value)

For small settings and flags:

```js
import { prefs } from "zelto/storage";

await prefs.set("theme", "dark");
const theme = await prefs.get("theme", "light");   // default if unset
await prefs.remove("theme");
```

Values are JSON-serializable. Don't store large blobs or secrets here.

## Files

```js
import { files } from "zelto/storage";

await files.write("notes/today.txt", "hello");
const text = await files.read("notes/today.txt");
const list = await files.list("notes/");
await files.delete("notes/today.txt");
```

Paths are relative to the app's private directory. Use `files.cache(...)` for data the
system may evict under pressure, and `files.documents(...)` for user data that persists.

## Database (SQLite)

For structured/relational data:

```js
import { openDatabase } from "zelto/storage";

const db = await openDatabase("app.db");
await db.exec(`CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT, done INT)`);
await db.run(`INSERT INTO todos (text, done) VALUES (?, ?)`, ["Buy milk", 0]);
const rows = await db.query(`SELECT * FROM todos WHERE done = ?`, [0]);
```

`db.run` for writes, `db.query` for reads, `db.exec` for schema, `db.transaction(fn)` for
atomic batches.

## Secure store

For tokens, passwords, and keys — backed by the system keystore (and biometrics where
available):

```js
import { secure } from "zelto/storage";

await secure.set("auth_token", token);
const token = await secure.get("auth_token");
await secure.set("auth_token", token, { requireBiometric: true });
```

See [../system-apis/biometrics.md](../system-apis/biometrics.md).

## Choosing a store

| Need | Use |
|---|---|
| A few settings/flags | `prefs` |
| Files, downloads, media | `files` |
| Structured / queryable data | SQLite |
| Secrets, credentials | `secure` |

## Migrations & versioning

Track a schema/data version in `prefs` and migrate on launch:

```js
const v = await prefs.get("schemaVersion", 0);
if (v < 1) { await db.exec(...); await prefs.set("schemaVersion", 1); }
```

## C API

```c
z_prefs_set_str("theme", "dark");
ZDatabase *db = z_db_open("app.db");
z_db_run(db, "INSERT INTO todos(text) VALUES(?)", z_args("Buy milk"));
```

See [../api-reference/c/system.md](../api-reference/c/system.md).

## Shared media

Everything above is scoped to **your app**. Images the user creates are not: screenshots
and camera stills go into one shared library that several apps write to and several read
from, under `<data>/media/`. See [photo-library.md](photo-library.md) for where it lives,
what a photo's identity is, and why the index is the directory rather than a database.

## Next

- [networking.md](networking.md)
- [photo-library.md](photo-library.md)
- [../system-apis/media.md](../system-apis/media.md)
