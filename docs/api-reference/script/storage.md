# Zelto Script API: `zelto/storage`

Per-app preferences, files, SQLite, and secure storage. Guide:
[../../guides/storage.md](../../guides/storage.md).

## `prefs` — key/value

```js
import { prefs } from "zelto/storage";
await prefs.set(key, value);          // JSON-serializable value
await prefs.get(key, fallback?);
await prefs.remove(key);
await prefs.keys();
```

## `files`

```js
import { files } from "zelto/storage";
await files.write(path, data);        // string | Uint8Array
await files.read(path);               // → string
await files.readBytes(path);          // → Uint8Array
await files.list(dir);                // → string[]
await files.delete(path);
await files.exists(path);             // → boolean
files.documents(rel);                 // persistent path
files.cache(rel);                     // evictable path
```

Paths are relative to the app's private directory.

## Database (SQLite)

```js
import { openDatabase } from "zelto/storage";
const db = await openDatabase(name);

await db.exec(sql);                    // schema / DDL
await db.run(sql, params?);            // INSERT/UPDATE/DELETE → { changes, lastId }
await db.query(sql, params?);          // SELECT → row[]
await db.transaction(async (tx) => {   // atomic batch
  await tx.run(...); await tx.run(...);
});
await db.close();
```

Parameters are positional (`?`) or named (`:name`):

```js
await db.run("INSERT INTO todos(text, done) VALUES(?, ?)", ["Buy milk", 0]);
```

## `secure` — keystore

```js
import { secure } from "zelto/storage";
await secure.set(key, value, { requireBiometric? });
await secure.get(key);                 // null if absent/denied
await secure.remove(key);
```

Backed by the system keystore; `requireBiometric` gates reads behind biometrics
([../../system-apis/biometrics.md](../../system-apis/biometrics.md)).
