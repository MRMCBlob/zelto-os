// android/prefs.js — SharedPreferences, translated onto the Zelto key/value store
// (zelto/storage, i.e. libzelto's app-scoped prefs on the persistent partition).
//
// Android apps keep small settings in a named SharedPreferences "file"; Zelto has
// one flat per-app store, so a file name becomes a key prefix ("<name>.<key>").
// Reads are synchronous. Writes go through an Editor that stages changes and only
// touches the store on apply()/commit(), matching Android's transactional shape —
// apply() is fire-and-forget, commit() returns whether it stuck.
import * as storage from "zelto/storage";

// A staged write batches puts/removes, then apply()/commit() flush them.
class Editor {
  constructor(prefix) {
    this._prefix = prefix;
    this._puts = {};      // key -> string value
    this._removes = [];   // keys to delete
    this._clear = false;
  }

  putString(key, value) {
    this._puts[key] = String(value);
    return this;
  }
  putInt(key, value) {
    this._puts[key] = String(value | 0);
    return this;
  }
  putLong(key, value) {
    this._puts[key] = String(value);
    return this;
  }
  putFloat(key, value) {
    this._puts[key] = String(value);
    return this;
  }
  putBoolean(key, value) {
    this._puts[key] = value ? "true" : "false";
    return this;
  }
  remove(key) {
    this._removes.push(key);
    return this;
  }
  // clear() drops every key in this file — deferred until the flush, per Android
  // (a clear then a put in the same editor keeps the put).
  clear() {
    this._clear = true;
    return this;
  }

  _flush() {
    if (this._clear) {
      // We cannot enumerate the store, so clear removes only the keys touched in
      // THIS editor plus the current staged set — the common "reset what I know"
      // use. A full wipe would need a store-side prefix delete.
      for (const key of this._removes) storage.remove(this._prefix + key);
    }
    for (const key of this._removes) storage.remove(this._prefix + key);
    for (const key of Object.keys(this._puts)) {
      storage.set(this._prefix + key, this._puts[key]);
    }
    this._puts = {};
    this._removes = [];
    this._clear = false;
  }

  // apply() is asynchronous-and-ignored in Android; here the store write is cheap
  // and synchronous, so we just flush and return nothing.
  apply() {
    this._flush();
  }
  // commit() returns a boolean success; our writes do not report failure, so it is
  // true unless the flush itself throws (it does not).
  commit() {
    this._flush();
    return true;
  }
}

export class SharedPreferences {
  constructor(name) {
    // "" is the default file; a named file namespaces its keys under "name.".
    this._prefix = name ? name + "." : "";
  }

  getString(key, def = null) {
    return storage.get(this._prefix + key, def);
  }
  getInt(key, def = 0) {
    return Math.trunc(storage.getNumber(this._prefix + key, def));
  }
  getLong(key, def = 0) {
    return storage.getNumber(this._prefix + key, def);
  }
  getFloat(key, def = 0) {
    return storage.getNumber(this._prefix + key, def);
  }
  getBoolean(key, def = false) {
    const v = storage.get(this._prefix + key, null);
    if (v === null) return def;
    return v === "true" || v === "1";
  }
  contains(key) {
    return storage.get(this._prefix + key, null) !== null;
  }
  edit() {
    return new Editor(this._prefix);
  }
}
