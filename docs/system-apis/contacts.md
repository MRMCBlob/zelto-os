# Contacts

Read (and, with permission, write) the user's contacts. Requires the `contacts`
permission.

> Declare `contacts = true` in `zelto.toml` ([../packaging/manifest.md](../packaging/manifest.md)).

## Pick a contact (no broad permission)

For a single contact, the picker avoids requesting full access:

```js
import { contacts } from "zelto/platform";

const c = await contacts.pick();    // user selects one → { name, phones, emails } | null
```

## Query (requires permission)

```js
const granted = await contacts.requestAccess();
if (granted) {
  const all = await contacts.list({ fields: ["name", "phones", "emails"] });
  const found = await contacts.search("Jane");
}
```

## Contact shape

```js
{
  id, name, firstName, lastName,
  phones: [{ label, number }],
  emails: [{ label, address }],
  avatar?,                          // image handle
}
```

## Writing (planned)

```js
await contacts.create({ name, phones });   // Planned — requires write permission
```

## Privacy

Prefer `contacts.pick()` over full `list()` whenever you only need one contact — it needs
no blanket permission and is the recommended pattern
([../platform/permissions.md](../platform/permissions.md)).

## Availability

Contact storage availability depends on the platform's PIM provider on the device.
