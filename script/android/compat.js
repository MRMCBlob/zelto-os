// android/compat.js — the andemu graceful contract.
//
// The whole point of the andemu route: an emulated Android app must not crash
// when it reaches for something Zelto does not (yet) map. So an unsupported
// service or sensor returns an Android-idiomatic empty (null Location, null
// getDefaultSensor, empty list), a denied permission returns the same empties
// with no callback fired, and an unknown method logs one line and returns a
// benign default — never a thrown exception, never a rejected promise a listener
// won't catch.
//
// `__ANDEMU__` is set by the host when an app is launched with --android; it only
// tunes whether unsupported calls warn (dev) or stay silent.

export const STRICT =
  typeof globalThis.__ANDEMU_SILENT__ !== "undefined" ? false : true;

export function warnUnsupported(what) {
  if (STRICT) console.warn(`andemu: unsupported ${what}`);
}

// A no-op stub object: any method call warns once and returns `fallback` (null by
// default), any property read returns null. Lets `getSomething(x).doThing()` run
// harmlessly when `getSomething` had nothing real to return.
export function stub(name, fallback) {
  const fb = fallback === undefined ? null : fallback;
  return new Proxy(
    {},
    {
      get(_t, prop) {
        if (prop === "then") return undefined; // never look like a thenable
        if (prop === Symbol.toPrimitive || prop === "toString" ||
            prop === Symbol.toStringTag) {
          return () => `[andemu stub ${name}]`;
        }
        return function () {
          warnUnsupported(`${name}.${String(prop)}()`);
          return fb;
        };
      },
    }
  );
}
