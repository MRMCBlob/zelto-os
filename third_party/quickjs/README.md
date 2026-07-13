# QuickJS (vendored)

The JavaScript engine [Zelto Script](../../docs/zelto-script/runtime.md) runs on.

| | |
|---|---|
| Upstream | <https://bellard.org/quickjs/> (Fabrice Bellard, Charlie Gordon) |
| Version | `2025-04-26` (see `VERSION`) |
| License | MIT (see `LICENSE`) |

## Why vendored

The engine is a handful of C files with no dependencies, and it must build for
both the aarch64 device image and the x86_64 host simulator from the same tree,
offline. A distro package would give us neither. Vendoring keeps the build
hermetic and the version pinned to something we have actually tested.

## What is here

Only the engine core:

```
cutils.c/h  dtoa.c/h  libregexp.c/h  libregexp-opcode.h
libunicode.c/h  libunicode-table.h  list.h
quickjs.c/h  quickjs-atom.h  quickjs-opcode.h
```

`quickjs-libc.c` is deliberately **not** vendored. It is upstream's host layer
(`std`/`os` modules: files, processes, workers), and an app sandbox must not have
it — Zelto Script's only host surface is the `zelto/*` bindings in
[`script/`](../../script), which go through libzelto and the permission broker.
The engine here is therefore pure computation: no I/O of its own.

## Updating

1. Download the new tarball from the upstream page and unpack it.
2. Copy the files listed above (plus `LICENSE` and `VERSION`) over this
   directory — no local patches, so this is a straight overwrite.
3. Set `quickjs_version` in `meson.build` to the new `VERSION` string (it is
   passed as `-DCONFIG_VERSION`).
4. Rebuild and run the script app's verification path
   (`meta/run-sim.sh` with `SIM_SCRIPT=system/apps/jsdemo/jsdemo.js`).

The library is built with warnings demoted to non-fatal and is exposed to
consumers via `-isystem`: upstream is not clean under this project's
`-Wall -Wextra -Wpedantic -Werror`, and neither patching it nor relaxing our own
flags is the right trade. Every line of Zelto's own code still compiles strict.
