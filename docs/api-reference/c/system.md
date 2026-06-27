# C API: System (`<zelto/system.h>`)

Storage, networking, notifications, and value marshalling for native code and modules.
Conventions (errors, async, memory): [../conventions.md](../conventions.md).

## Preferences

```c
bool        z_prefs_set_str(const char *key, const char *value);
const char *z_prefs_get_str(const char *key, const char *fallback);
bool        z_prefs_set_int(const char *key, int64_t value);
int64_t     z_prefs_get_int(const char *key, int64_t fallback);
bool        z_prefs_remove(const char *key);
```

## Files

Paths are relative to the app's private directory.

```c
bool     z_file_write(const char *path, const void *data, size_t len);
ZBytes   z_file_read(const char *path);          // .ok == false on failure
bool     z_file_delete(const char *path);
ZList   *z_file_list(const char *dir);
char    *z_path_documents(const char *rel);      // persistent
char    *z_path_cache(const char *rel);          // evictable
```

## Database (SQLite)

```c
ZDatabase *z_db_open(const char *name);          // NULL on failure
bool       z_db_exec(ZDatabase *db, const char *sql);
bool       z_db_run(ZDatabase *db, const char *sql, ZArgs args);
ZRows     *z_db_query(ZDatabase *db, const char *sql, ZArgs args);
bool       z_db_transaction(ZDatabase *db, ZAction fn);
void       z_db_close(ZDatabase *db);
```

`z_args(...)` builds the parameter list:

```c
z_db_run(db, "INSERT INTO todos(text, done) VALUES(?, ?)", z_args("Buy milk", 0));
```

Iterate rows:

```c
ZRows *r = z_db_query(db, "SELECT id, text FROM todos", z_args());
while (z_rows_next(r)) {
  int64_t id = z_rows_int(r, 0);
  const char *text = z_rows_str(r, 1);
}
z_rows_free(r);
```

## Secure store

```c
bool   z_secure_set(const char *key, const char *value, ZSecureOpts opts);
char  *z_secure_get(const char *key);            // NULL if absent/denied
```

`ZSecureOpts{ .require_biometric = true }` gates access behind biometrics
([../../system-apis/biometrics.md](../../system-apis/biometrics.md)).

## Networking

Requires the `network` permission. Async; callbacks run on the app loop.

```c
ZNetRequest *z_net_get(const char *url);
ZNetRequest *z_net_request(const char *method, const char *url);
void  z_net_set_header(ZNetRequest *r, const char *k, const char *v);
void  z_net_set_body(ZNetRequest *r, const void *data, size_t len);
void  z_net_send(ZNetRequest *r, ZNetCallback cb, void *ud);
void  z_net_cancel(ZNetRequest *r);

typedef void (*ZNetCallback)(ZNetResponse *res, void *ud);
// res->status, res->ok, ZBytes z_net_body(res), z_net_header(res, "…")
```

WebSockets:

```c
ZWebSocket *z_ws_open(const char *url);
void z_ws_on_message(ZWebSocket *ws, ZWsMessageCb cb, void *ud);
void z_ws_send(ZWebSocket *ws, const void *data, size_t len);
void z_ws_close(ZWebSocket *ws);
```

## Notifications

Requires the `notifications` permission.

```c
void z_notify_define_channel(const char *id, const char *name, ZImportance imp);
ZNotification *z_notify_new(const char *title, const char *body);
void z_notify_set_channel(ZNotification *n, const char *channel_id);
void z_notify_add_action(ZNotification *n, const char *id, const char *title);
int64_t z_notify_post(ZNotification *n);         // returns notification id
void z_notify_cancel(int64_t id);
void z_notify_set_badge(int count);
```

## Value marshalling (native modules)

Used when exposing C to Zelto Script
([../../guides/interop-c-and-script.md](../../guides/interop-c-and-script.md)):

```c
double      z_arg_number(ZContext *ctx, ZValue *argv, int i);
const char *z_arg_string(ZContext *ctx, ZValue *argv, int i);
ZBytes      z_arg_bytes(ZContext *ctx, ZValue *argv, int i);
ZValue      z_value_number(ZContext *ctx, double v);
ZValue      z_value_string(ZContext *ctx, const char *s);
ZValue      z_value_bytes(ZContext *ctx, ZBytes b);
ZValue      z_call(ZContext *ctx, ZValue fn, int argc, ZValue *argv);
void        z_throw(ZContext *ctx, const char *message);
void        z_post(ZContext *ctx, ZValue fn, ZValue arg);   // from a worker thread
```

## See also

- [ui.md](ui.md) · [platform.md](platform.md) · [gfx.md](gfx.md)
- Guides: [storage](../../guides/storage.md), [networking](../../guides/networking.md),
  [notifications](../../guides/notifications.md).
