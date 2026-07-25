# Simulator (internals & advanced use)

The user-facing guide is [../getting-started/simulator.md](../getting-started/simulator.md).
This page covers how the simulator works and advanced options.

## How it works

The simulator runs the real `zcomp` compositor as a **nested Wayland client** of your
desktop session, instead of driving DRM/KMS directly on a phone
([../contributing/compositor-internals.md](../contributing/compositor-internals.md)).

```
your desktop (Wayland/X11)
└── zcomp (nested) ── phone-sized window
    ├── minimal System UI (status bar, home gesture)
    ├── zsysd (permission broker, lifecycle)   [simulated]
    └── your app (libzelto / Zelto Script)
```

Because it's the same compositor and toolkit, layout, animation, gestures, navigation,
and permission prompts behave as on device. Only the kernel/HAL and the Android container
are absent.

The key mechanic: `zcomp` connects to your desktop compositor as **one** ordinary client
(the wlroots *wayland* backend) and creates its **own** Wayland socket. Every Zelto
surface — the status bar, launcher, keyboard, shade, and apps — connects to *that*
socket, so `zcomp` (not your desktop) provides them layer-shell, input-method,
foreign-toplevel, etc. Your desktop only has to host `zcomp`'s single window. That's why
the full System UI works nested even though a plain desktop compositor doesn't implement
wlr-layer-shell.

## Running it today (`meta/run-sim.sh`)

The `zelto run --simulator` CLI above is the intended front-end; the working
implementation right now is the `meta/run-sim.sh` script, which runs the host (x86_64)
build of `zcomp` + the System UI nested on your session. No VM, no aarch64 emulation — it
runs at native speed (contrast `meta/run-qemu.sh`, which boots the real aarch64 image).

```sh
# Build the host binaries + run in a window on your desktop (WSLg/Wayland/X11):
meta/run-sim.sh

# Run existing build-host/ artifacts without rebuilding:
SKIP_BUILD=1 meta/run-sim.sh

# Also auto-launch an app (name of a built binary, or a full path):
SIM_APP=zelto-notepad meta/run-sim.sh

# Launch an app N seconds INTO the run, not during boot:
SIM_LATE_APP="zelto-cards 6" meta/run-sim.sh
```

`SIM_APP` and `SIM_EXTRA` both spawn while the shell is still coming up, which is right
for "have this on screen" and useless for "make something happen once the system has
settled" — the window maps before the state exists. `SIM_LATE_APP` is a focus change at a
chosen moment, which is how a test reaches an edge that only fires on one. (The App
Switcher's window snapshot is taken at the active→inactive edge, so proving the lock
screen suppresses it needs that edge to land *after* the screen has locked — see
`test/test_capture_lock_suppression.sh`.)

The host build lives in `build-host/` (native meson build; the SDK needs
`libsqlite3-dev`). The compositor draws with the **pixman** software renderer by default
(`WLR_RENDERER=gles2` to try the GPU in windowed mode — the headless backend has no GPU
device). App storage goes to `$ZELTO_DATA_DIR` (a host temp dir) instead of the phone's
`/var/zelto`.

**App tiles.** On device, `build-initramfs.sh` copies the `.app` manifests into
`/usr/share/zelto/apps` (the launcher's hardcoded scan dir) and the binaries to
`/usr/bin`. Neither exists on your host, so the script instead emits the same manifest set
into `$ZELTO_DATA_DIR/apps/manifests` — the *other* directory the launcher scans — with
each `exec=` rewritten from `/usr/bin/zelto-X` to the matching `build-host/` binary. That
is what populates the launcher grid with the full app set (Widget is omitted, as on
device — it ships only inside `widget.zap`). If a launcher tile fails to launch, check that
binary built under `build-host/`; the script prints a `tile: … -> <path>` line per app.

The output is a **portrait handset** by default (720×1440). Override with `SIM_SIZE=WxH`:

```sh
SIM_SIZE=1080x2340 meta/run-sim.sh     # a taller phone
SIM_SIZE=1280x720  meta/run-sim.sh     # landscape / tablet
```

zcomp reads `ZCOMP_OUTPUT_SIZE` and sets that as the virtual output's custom mode, so the
whole System UI reflows to it (the window/headless surface is that size).

zcomp and every Zelto client run in a **private runtime dir** (`/tmp/zelto-sim/xdg`) so
zcomp's own socket is deterministically `wayland-0` there and never collides with (or
clobbers) the desktop's `wayland-0`. The windowed path reaches the desktop compositor by
its absolute socket path (falling back to WSLg's `/mnt/wslg/runtime-dir/wayland-0`). Point
`grim`/`wlrctl`/`wtype` at `XDG_RUNTIME_DIR=/tmp/zelto-sim/xdg WAYLAND_DISPLAY=wayland-0`
in both modes. WSLg's X11 (Xwayland) lacks DRI3/shm for wlroots' x11 backend, so Wayland
is the windowed path; `SIM_BACKEND=x11` forces X11 if you need it.

## Headless + screenshots + scripted input

For agents/CI, the simulator runs **headless** (no window, a virtual output) and is driven
exactly like `meta/run-qemu.sh`'s QMP harness — except the tooling is standard Wayland:

- **Screenshots:** `zcomp` advertises **wlr-screencopy**, so [`grim`](https://sr.ht/~emersion/grim/)
  captures a frame.
- **Input:** `zcomp` advertises **wlr-virtual-pointer** + **virtual-keyboard**, so
  [`wlrctl`](https://git.sr.ht/~brocellous/wlrctl) injects taps/clicks and `wlrctl`/`wtype`
  type text — the desktop analog of QMP `input-send-event`. Injected events flow through
  the same seat handlers as real hardware (see `compositor/src/seat.c`).

Install once: `apt install grim wlrctl wtype`.

```sh
# Boot headless, wait, grab a PNG, exit (the one-shot verification path):
HEADLESS=1 SHOT=out/sim-home.png meta/run-sim.sh

# Or drive it interactively: run headless in the background, then script it.
XDG_RUNTIME_DIR=/tmp/zelto-sim/xdg WAYLAND_DISPLAY=wayland-0 \
    HEADLESS=1 SIM_APP=zelto-notepad meta/run-sim.sh &
export XDG_RUNTIME_DIR=/tmp/zelto-sim/xdg WAYLAND_DISPLAY=wayland-0   # the sim's private socket
sleep 12
grim out/before.png
wlrctl pointer move -4000 -4000   # pin to origin (relative moves)
wlrctl pointer move 640 222       # to the note field on the 1280x720 output
wlrctl pointer click              # focus it -> the on-screen keyboard slides up
wlrctl keyboard type "hi"
grim out/after.png
```

### Gestures and the capture chord

`wlrctl` only clicks, so a gesture that has to press and hold — a drag, a long-press — is
scripted by `zcomp` itself, through the same seat path a finger takes. The screenshot chord
is scripted the same way, and for the same reason: there is no key combination the harness
can press.

```sh
ZCOMP_DRAG="x0 y0 x1 y1 [ms]"   # press, glide, release (pan / swipe)
ZCOMP_HOLD="x y [ms]"           # press, hold, release (long-press)
ZCOMP_INPUT_DELAY=ms            # when to start (default 6500 — after the app maps)
ZCOMP_SHOT_AT="ms [ms ...]"     # fire the capture chord at each moment
```

Controls are pressed by NAME, never by coordinate:

```sh
ZELTO_TAP_LABEL="Delete,Delete Photo"   # a comma-separated SEQUENCE
ZELTO_TAP_APP=os.zelto.photos           # scope it to one surface
ZELTO_TAP_AT=9000                       # when (ms; the app has to have mapped)
```

The sequence exists for confirmations: the second control did not exist when the
first tap was arranged, so it cannot be reached by two independent hooks. Each
step re-arms rather than firing in a loop, because the next control has to be
built before it can be found. Scope it with `ZELTO_TAP_APP` — without it every
surface in the boot gets the same hook, and a label two surfaces share is pressed
on whichever built first.


`ZCOMP_SHOT_AT` takes a **list** on purpose. A test asserting that no screenshot was written
while the screen was locked also passes in a boot that never managed to take one at all, so
two moments in one boot — one before the lock engages, one after — let the run carry its own
positive control. It enters through the same `zcomp_screenshot()` the chord calls, so the
lock guard is on the path under test rather than beside it.

Captured photos land in `$ZELTO_DATA_DIR/media/photos` (thumbnails in `media/thumbs`), the
shared library `system/common/photos.h` defines. `$ZELTO_PHOTOS_ROOT` overrides that root
for a test or a shot that needs a prepared library.

Both modes use the private `XDG_RUNTIME_DIR` (`/tmp/zelto-sim/xdg`) where `zcomp`'s socket
is deterministically `wayland-0` — point `grim`/`wlrctl` at that dir + socket. The note
field lands near `(360, y)` on the default 720×1440 portrait output; retune taps to your
`SIM_SIZE`.

## Device profiles

A profile defines resolution, scale factor, safe-area insets, and input type:

```sh
zelto simulator profiles
zelto run --simulator --profile tall-1080
```

Custom profile (project-local `zelto.sim.toml`):

```toml
[[profile]]
name = "my-device"
width = 1080
height = 2400
scale = 3
safeArea = { top = 96, bottom = 48 }
```

## Mocking device state

```sh
zelto simulator set battery 35
zelto simulator set network none      # offline
zelto simulator set location 52.52,13.40
zelto simulator rotate
```

Sensor and permission mocking let you test edge cases deterministically.

## Hot reload

Zelto Script edits reload in place, preserving navigation where possible. Native (C)
changes trigger a rebuild + app restart.

## What's not simulated

- The **Android container** — APK interop requires a device or Waydroid-capable host
  ([../getting-started/run-on-device.md](../getting-started/run-on-device.md)).
- Real **GPU/CPU performance** — profile on device for true numbers
  ([profiling.md](profiling.md)).
- Device-specific **HAL** behavior (camera/sensors quirks).

## Headless / CI mode

The intended CLI form:

```sh
zelto run --simulator --headless --screenshot out.png
```

Renders without a visible window — useful for screenshot tests in CI. See
[Headless + screenshots + scripted input](#headless--screenshots--scripted-input) above
for the working `meta/run-sim.sh` equivalent (`HEADLESS=1 SHOT=out.png meta/run-sim.sh`)
and how to drive taps/typing with `grim` + `wlrctl`.
