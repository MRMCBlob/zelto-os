# Interop: C and Zelto Script

Most app code is Zelto Script (fast to write); performance-critical code is C. The two
share the same UI tree and call each other directly. This page shows how to mix them.

## When to drop to C

- Heavy compute (image/audio/data processing, parsing, crypto).
- Tight per-frame work that must avoid GC/interpreter overhead.
- Wrapping an existing C/C++ library.

Everything else — UI, state, navigation, networking — is fine in Zelto Script.

## Native modules

A native module is a C library exposing functions to Script. Declare it and build it
into the package.

`zelto.toml`:

```toml
[[native]]
name = "imgproc"
src = ["native/imgproc.c"]
```

`native/imgproc.c`:

```c
#include <zelto/module.h>

// double blurAmount(bytes image, double radius)
static ZValue blur(ZContext *ctx, int argc, ZValue *argv) {
  ZBytes img = z_arg_bytes(ctx, argv, 0);
  double radius = z_arg_number(ctx, argv, 1);
  ZBytes out = do_blur(img, radius);          // your C code
  return z_value_bytes(ctx, out);
}

Z_MODULE("imgproc", {
  Z_EXPORT("blur", blur, 2),
});
```

Use it from Script:

```js
import { blur } from "native:imgproc";
const blurred = blur(imageBytes, 8.0);
```

Marshalling helpers (`z_arg_*`, `z_value_*`) convert between Script values and C types —
see [../api-reference/c/system.md](../api-reference/c/system.md).

## Calling Script from C

Native modules can call back into Script (e.g. callbacks):

```c
ZValue cb = argv[0];                  // a Script function passed in
z_call(ctx, cb, 1, (ZValue[]){ z_value_number(ctx, 42) });
```

Calls into Script must happen on the app loop thread; for work on another thread, post a
result back with `z_post(ctx, fn, value)`.

## Sharing the UI tree

C native modules can return view trees too, though typically UI stays in Script and C
returns data. A pure-C app uses the `libzelto` macros directly
([../api-reference/c/ui.md](../api-reference/c/ui.md)); the declarative model is identical.

## Memory & ownership

- Bytes/strings passed across the boundary are copied or reference-counted by the
  runtime; don't hold raw pointers past the call unless you retain them
  (`z_retain`/`z_release`).
- C errors surface to Script as exceptions via `z_throw(ctx, "message")`.

See [../api-reference/conventions.md](../api-reference/conventions.md) for memory and
error rules.

## Build

`zelto build` cross-compiles native modules for the target ABI (aarch64 on device,
host ABI for the simulator) and bundles the `.so` into the `.zap`
([../packaging/zap-format.md](../packaging/zap-format.md)).

## Next

- [../api-reference/c/system.md](../api-reference/c/system.md)
- [../zelto-script/runtime.md](../zelto-script/runtime.md)
