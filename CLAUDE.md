# Zelto OS — working notes for agents

A phone OS in C: a wlroots compositor (`compositor/`), a declarative UI toolkit +
SDK (`sdk/`), the System UI and apps (`system/`), packaging and harnesses
(`meta/`), and the test suite (`test/`).

## Build

Everything builds in **WSL Ubuntu**, not the MSYS Bash tool (which has no meson,
no cross-gcc, no qemu).

```sh
ninja -C build-host          # the host x86_64 build — what the simulator runs
ninja -C build-arm64         # the cross build — what QEMU runs
bash test/run-tests.sh       # the suite (must be green before committing)
```

Invoke WSL by writing commands to a `.sh` file and running it — inline quoting
through MSYS is fragile:

```sh
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
wsl.exe -d Ubuntu bash /mnt/c/.../script.sh 2>&1 | tr -d '\0'
```

## VERIFY IN THE SIMULATOR FIRST

`meta/run-sim.sh` runs the real compositor, toolkit and System UI natively on
WSLg. A boot takes **seconds**; the QEMU equivalent takes **minutes** under TCG.
Use it for essentially everything:

```sh
HEADLESS=1 SHOT=out/x.png meta/run-sim.sh          # boot, screenshot, exit
SIM_SIZE=1280x800 ... meta/run-sim.sh              # a different panel geometry
SIM_APP=zelto-notepad ... meta/run-sim.sh          # launch an app at boot
ZELTO_DATA_DIR=/tmp/d ... meta/run-sim.sh          # point it at its own disk
```

A **second boot on the same `ZELTO_DATA_DIR` is a reboot**, which is how the
persistence tests work without an emulator (`test_settings_broker_sim.sh`).
Prefer a sim test in `test/` over a `meta/run-qemu.sh` harness: harnesses cannot
run from `test/run-tests.sh`, so nobody runs them.

### …but the simulator is NOT the target, and saying so cost 15 phases

It is faster and it is *mostly* the same, which is a dangerous combination. Do
not write "verified" on target-shaped claims you only checked in the sim.

- **The sim sets its own `ZELTO_FONT`.** P30 renamed the bundled face and left
  `meta/initramfs/init` pointing at the old name. Every libzelto client on the
  real target failed to open a font and QEMU **rendered zero glyphs from P30 to
  P45** — fifteen phases — because every phase in between was "verified in the
  sim". `test_bundled_font_path.sh` now pins it.
- **qemu-virt runs at 1280x800, the design targets 720x1440.** Anything
  proportional (the `Z_PT` numerator is `720/390`) is not validated by a QEMU
  frame. Use `SIM_SIZE` to check a geometry rather than assuming.
- **A QEMU frame can be silently incomplete.** Teal `#0a858c` in a screendump is
  `ZCOMP_BG` showing through an unpainted surface — a first-paint/screendump
  race that moves between boots. Do not diagnose it as a layout bug.

So: **sim by default; QEMU when the claim is about the image, the kernel, the
initramfs, packaging, or anything the sim substitutes for.** Boot QEMU at least
once a phase, and grep the serial log rather than trusting it.

## Verify by measuring, not by looking

This project's recurring bug is a number that is quietly wrong everywhere at
once, so nothing looks broken. Three separate ones shipped for 13+ phases.

- Check press/veil shots with `meta/pngdiff.py --expect-box` against a control at
  the **same state**, never by eye.
- **The shot catalogue has a noise floor**: every frame contains the status-bar
  clock, so no shot ever has a zero delta and `shots.sh` printing `ok` means only
  that a PNG exists. `meta/shots.sh` does **not** rebuild — it photographs
  whatever is in `build-host`, and it takes ~15 minutes, so let it finish before
  diffing.
- A shot can also photograph the wrong thing entirely:
  `11a-notification-center` shows an *empty* Notification Center.

## Metrics: points vs screen units

`1pt = 1.85 screen units` (720/390). The rule, learned three times:

> A number copied off a spec sheet is in **POINTS** and goes through `Z_PT()`.
> A number **derived from the screen** is already in screen units and must not.

And: **a reserve that names its parts in a comment must be an expression over
them**, never a literal — `KBD_H 300`, `LIB_TOP 116` and `BOTTOM_RESERVE 208`
were all declared totals that drifted from what they held. Pinned by
`test_reserves_derived.sh`, `test_radius_ladder.sh`, `test_type_scale_in_points.sh`
and `test_safe_areas_shared.sh`.

## Tests

One concern per file, `test/test_*.{c,sh}`. C tests `#include` the .c under test
and stub its externs. **`-k` takes a glob, not a substring**: `-k '*font*'`.

**Negative-test anything meant to fail.** Break the thing, watch the test fail
with the right message, restore. A lint that has never failed is a claim. Every
assertion about an absence needs a **positive control from the same run**, or it
passes on a boot that never reached the state.

## Driving the OS in a test: no coordinates

Tap coordinates rot. Five harnesses silently became unconditional `exit 0`s when
P40 deleted the app drawer they navigated. Drive through env hooks and assert on
the **log**:

`ZELTO_SETTINGS_SET` · `ZELTO_KBD_TYPE` · `ZELTO_NOTEPAD_AUTOSAVE` ·
`ZELTO_FETCH_AUTO` · `ZELTO_CONSENT_AUTO` · `ZELTO_PRESS_X/Y` · `ZELTO_SCROLL_TO`
· `zelto.seedsettings=` / `zelto.kbd=` / `zelto.actuate=` (kernel cmdline)

If an actuation has no log line, **add one** — an anonymous line cannot carry
"THIS thing happened to THAT app", which is why several guards went a whole phase
untested.

## Toolkit traps that are still live

- A fixed gap in a stack is `Frame(w, h, Rect(.color = z_rgba(0,0,0,0)))`, never
  `Frame(w, h, Spacer())` — a Spacer keeps its grow flag through Frame.
- A `ZStack` centres each child **at the child's own size** unless it has `Fill()`.
- `Frame` only sets a size: a `Text` made a Frame's direct child sits in its
  top-left corner.
- `Padding` insets **both** axes.
- `CornerRadius` rounds a node's own paint; `Clip(r, …)` masks its subtree.
- `WrapText` needs its column width at **build** time — a sibling that sizes to
  its own content leaves that width unknown exactly when it is needed.
- Hardware keys **cannot type into a `ZTextField`**: `kb_key` handles
  Escape/Backspace and Return/space only. Text arrives via text-input-v3
  `commit_string`, i.e. through the on-screen keyboard.

## Shell/tooling traps

- **The Bash tool mangles backslashes into python heredocs.** `'\0'` becomes a
  real NUL byte and `\n` becomes a real newline — both produce broken C. Use the
  Edit tool for anything containing an escape, or write the python to a file.
- MSYS python rewrites files as **CRLF**; harmless for `.c`, fatal for a shell
  script (`set: pipefail: invalid option name`).
- `grep -c` exits **1** when the count is zero and kills a `set -e` harness.
- Measure clock skew's **symptom** (does the build fail?), never back-date
  `meson.build` pre-emptively — under that workaround build-system edits are
  silently ignored.
