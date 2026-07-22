# Zelto OS — agent notes

A phone OS in C: wlroots compositor (`compositor/`), UI toolkit + SDK (`sdk/`),
System UI and apps (`system/`), packaging/harnesses (`meta/`), tests (`test/`).

## Build

Builds in **WSL Ubuntu**, not the MSYS Bash tool. Write commands to a `.sh` file
and run `wsl.exe -d Ubuntu bash /mnt/c/...` (inline quoting through MSYS is
fragile; set `MSYS_NO_PATHCONV=1`).

```sh
ninja -C build-host      # host x86_64 — what the simulator runs
ninja -C build-arm64     # cross build — what QEMU runs
bash test/run-tests.sh   # must be green before committing
```

## Verify in the simulator

`meta/run-sim.sh` runs the real compositor/toolkit/System UI natively. A boot is
**seconds**; QEMU is **minutes** under TCG.

```sh
HEADLESS=1 SHOT=out/x.png meta/run-sim.sh
# SIM_SIZE=1280x800 · SIM_APP=zelto-notepad · ZELTO_DATA_DIR=/tmp/d
```

A second boot on the same `ZELTO_DATA_DIR` **is a reboot** — that's how the
persistence tests work without an emulator. Prefer a sim test in `test/` over a
`run-qemu.sh` harness: harnesses can't run from `test/run-tests.sh`, so nobody
runs them (five rotted into unconditional `exit 0`s that way).

**The sim is not the target.** "Same results" is the belief that let the real
target render **zero glyphs from P30 to P45** — the sim sets its own
`ZELTO_FONT`. Also: a QEMU screendump can be silently incomplete (teal `#0a858c`
= `ZCOMP_BG` through an unpainted surface — `meta/teal.py` counts it, and
`FRAMES=N` takes repeat dumps of one settled screen to tell a first-paint race
from a capture race). So: sim by default, QEMU when the claim is about the
**image** (initramfs, packaging, kernel, net). Boot QEMU once a phase and grep
the serial log. Never write "verified on target" for a sim-only run.
qemu-virt ran at **1280x800** from P6 to P45 — QEMU's default virtio-gpu EDID —
against a 720x1440 design, so no on-target frame had ever shown the metrics at
the geometry they were computed for. It is `xres`/`yres` on `virtio-gpu-pci` now
(`OUTW`/`OUTH`, one definition in `run-qemu.sh`), not a physical panel.

## Measure, don't look

The recurring bug here is a number quietly wrong *everywhere at once*, so nothing
looks broken. Three shipped for 13+ phases.

- **Ask the layout where things are — never write a coordinate.** `ZELTO_PROBE_TAPS=1`
  dumps every laid-out tappable and every string, per process, with the frames;
  `ZELTO_PROBE_AT=<ms>` moves the dump to just before the capture (surfaces whose
  content arrives later — the lock screen — are empty at their first settled
  build). `z_probe_tap()` resolves a control by its HANDLER, hit-tests its own
  centre and dispatches what the walk found. That is how a surface gets driven
  without coordinates to rot.
- Check press/veil shots with `meta/pngdiff.py --expect-box` against a control at
  the same state.
- The shot catalogue has a **noise floor** (the status-bar clock), so no frame
  ever has a zero delta and `shots.sh` printing `ok` means only "a PNG exists".
  Worse, **a shot of the wrong screen has a perfectly healthy delta** — so every
  shot declares `EXPECT` / `MUSTNOT` (matched against `<process>|<string>` for
  what is ON SCREEN) or a `NOMARKER` reason. It does **not** rebuild, and takes
  ~15 min — let it finish before diffing, and **never edit a shell script while
  it is running** (bash re-reads from the file offset; it died mid-run this way).
- `1pt = 1.85 units` (720/390). A number off a spec sheet is **points** → `Z_PT()`;
  a number derived from the screen is already units. And **a reserve that names
  its parts must be an expression over them**, never a literal (`KBD_H 300`,
  `LIB_TOP 116`, `BOTTOM_RESERVE 208` all drifted).
- **Never estimate a text metric — `z_line_height(app, size)` asks the face.** A
  `font * 1.31` fudge lasted one phase: the true ratio runs 1.3182 down to 1.2529
  across the scale, so it over-reserved at the top and under-reserved at Caption.

## Tests

One concern per file. `-k` takes a **glob**: `-k '*font*'`.
**Negative-test anything meant to fail** — break it, watch it fail with the right
message, restore. Any assertion about an *absence* needs a positive control from
the same run.

**Never drive a test by tap coordinates** — they rot. Use env hooks and assert on
the log: `ZELTO_SETTINGS_SET` · `ZELTO_KBD_TYPE` · `ZELTO_NOTEPAD_AUTOSAVE` ·
`ZELTO_FETCH_AUTO` · `ZELTO_CONSENT_AUTO` · `ZELTO_PRESS_X/Y` · `ZELTO_SCROLL_TO`
· cmdline `zelto.seedsettings=` / `zelto.kbd=` / `zelto.actuate=`. If an actuation
has no log line, add one.

## Traps

- Fixed gap = `Frame(w,h, Rect(.color=z_rgba(0,0,0,0)))`, never `Spacer()`.
- `ZStack` centres children at their own size unless `Fill()`; `Frame` only sets
  a size; `Padding` insets both axes.
- **A `Frame`'s fixed height is an INNER height when the node also has padding** —
  `measure()` does `h = fixed_h + 2*padding`, so passing the outer height makes
  the node a whole padding taller than everything computed from that number.
- **`Grow` divides the SLACK, `Share` divides the AXIS.** Two `Grow(1)` siblings
  come out equal only if they hold the same thing, so a grid cell built with
  `Grow` is as wide as its own label (keyboard caps, Control Center toggles).
- **`Text` never wraps and never truncates** — it measures to one line however
  long, and `arrange` then clamps its FRAME while the glyphs paint straight
  through. Prose → `WrapText`; an identifier in a fixed row → `EllipsizeText`.
  `ZELTO_PROBE_TAPS` reports any Text needing more room than its box.
- `CornerRadius` rounds a node's own paint, `Clip(r,…)` masks its subtree.
- `WrapText` needs its column width at **build** time.
- Hardware keys **cannot type into a `ZTextField`** — text arrives only via
  text-input-v3 `commit_string` (the on-screen keyboard).
- **The Bash tool mangles backslashes into python heredocs**: `'\0'` becomes a
  NUL byte, `\n` a real newline. Use Edit, or write the python to a file.
- MSYS python rewrites files **CRLF** — fatal for shell scripts.
- `grep -c` exits 1 on zero matches and kills a `set -e` harness.
- Clock skew: measure the **symptom**, never back-date `meson.build` (under that
  workaround build-system edits are silently ignored).
