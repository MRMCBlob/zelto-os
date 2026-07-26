# iOS parity — every iOS capability, and whether Zelto OS has it

**Baseline:** iOS 27 (announced WWDC, 8 June 2026; public beta; ships autumn 2026).
**Zelto OS as of:** 25 July 2026, branch `dev`, phase P53 — plus the **uncommitted
P54 theming work** in the working tree, which is marked as such in the two rows it
changes (§9, light/dark). Everything else is scored against committed code.

This is a gap register. It lists iOS capabilities at the smallest nameable unit —
`Pinch to zoom`, not "gestures" — regardless of how big or small the feature is,
and scores each one against what Zelto OS **actually implements in code today**.

Two rules that make it trustworthy:

1. **Scored against source, not docs.** `docs/README.md` says outright that this
   documentation "describes the target design". Several pages describe APIs with
   **zero implementation** — `docs/system-apis/biometrics.md` documents a
   `zelto/platform` module that does not exist; `docs/platform/permissions.md` and
   `docs/overview/architecture-overview.md` claim apps are "sandboxed with Linux
   namespaces + seccomp", and a repo-wide grep for `unshare|seccomp|setuid|chroot`
   returns **no hits in any source file**. Those score ❌ here.
2. **Every absence has a positive control.** Before any ❌ is asserted the same
   grep is run for a sibling that *does* exist. Example: `WL_SEAT_CAPABILITY_TOUCH`
   returns zero, `WL_SEAT_CAPABILITY_POINTER` returns `compositor/src/seat.c:476`
   — so "no touch" is a fact about the code, not about the search.

## Legend

| Mark | Meaning |
|---|---|
| ✅ | Present and comparable to the iOS behaviour |
| ✅+ | Present **and built more rigorously** than the iOS analogue — the threshold is *measured* rather than asserted. Used sparingly and literally. |
| 🟡 | Partial. The cell always says what specifically is missing. |
| ❌ | Absent from the code. |
| ⛔ | Deliberately declined, with the reason recorded in-source. |

`iOS` column: blank = long-standing; `26` / `27` = introduced in that release;
`27β` = announced for iOS 27 but not yet shipped (beta as of this writing, so it
could still be pulled); `hw` = needs specific hardware.

---

## Scorecard

**555 rows.** `Covered` = (✅ + ✅+ + ½·🟡) ÷ rows, with ⛔ rows excluded from the
denominator — a feature deliberately declined is not a gap. The counts are computed
from this file by a script, not typed by hand, so they cannot drift from the tables
below without the tables changing too.

| # | Section | ✅ | ✅+ | 🟡 | ❌ | ⛔ | Rows | Covered |
|---|---|---|---|---|---|---|---|---|
| 1 | Touch, gestures & input | 7 | 3 | 6 | 16 | 1 | **33** | 41% |
| 2 | System navigation & multitasking | 8 | 1 | 0 | 7 | 0 | **16** | 56% |
| 3 | Home Screen & App Library | 9 | 0 | 7 | 14 | 1 | **31** | 42% |
| 4 | Lock Screen, StandBy & Always-On | 4 | 1 | 2 | 15 | 2 | **24** | 27% |
| 5 | Notifications | 8 | 1 | 1 | 14 | 0 | **24** | 40% |
| 6 | Control Center & system controls | 6 | 0 | 4 | 21 | 0 | **31** | 26% |
| 7 | Text input & keyboard | 11 | 6 | 4 | 19 | 1 | **41** | 48% |
| 8 | UI toolkit (UIKit / SwiftUI equivalents) | 23 | 4 | 15 | 22 | 0 | **64** | 54% |
| 9 | Typography, colour & theming | 6 | 6 | 2 | 6 | 1 | **21** | 65% |
| 10 | Accessibility | 2 | 3 | 3 | 35 | 0 | **43** | 15% |
| 11 | Privacy & security | 5 | 3 | 2 | 29 | 0 | **39** | 23% |
| 12 | App model & distribution | 9 | 1 | 2 | 15 | 0 | **27** | 41% |
| 13 | System services & data | 6 | 0 | 1 | 17 | 0 | **24** | 27% |
| 14 | Hardware & device | 3 | 0 | 8 | 19 | 1 | **31** | 23% |
| 15 | Connectivity | 1 | 0 | 3 | 18 | 0 | **22** | 11% |
| 16 | Media | 6 | 0 | 2 | 14 | 0 | **22** | 32% |
| 17 | First-party app suite | 6 | 0 | 4 | 20 | 0 | **30** | 27% |
| 18 | Apple Intelligence, Siri & AI | 0 | 1 | 0 | 18 | 0 | **19** | 5% |
| 19 | Boot, updates & device management | 1 | 0 | 1 | 11 | 0 | **13** | 12% |
| | **Total** | **121** | **30** | **67** | **330** | **7** | **555** | **34%** |

Read the shape, not the total. The three sections above 50% — UI toolkit,
typography, text input — are the ones an app developer touches. The three below
15% — Apple Intelligence, connectivity, accessibility — are the ones a *user*
touches, and two of the three are blocked on a single missing foundation each
rather than on many missing features.

---

## Root-cause gaps

Most of the ❌ rows below are not independent. Six decisions each cascade into a
whole block of them. Ordered by how many rows each one unblocks:

1. **The seat never advertises `wl_touch`.** `compositor/src/seat.c:492` attaches
   `WLR_INPUT_DEVICE_TOUCH` to the `wlr_cursor` as a *pointer*, and
   `update_capabilities()` at `:476` only ever sets `WL_SEAT_CAPABILITY_POINTER`.
   The SDK states the assumption in `sdk/src/internal.h` — "a phone is
   single-touch". Every multi-finger interaction on iOS — pinch, rotate,
   two-finger scroll, three-finger undo, four-finger app switch, split-keyboard
   drag — is blocked behind this one line. **~14 rows.**
2. **`ZNode` carries no semantics.** `sdk/src/internal.h` defines the node struct
   with no role, label, value, trait, or hint field, and the compositor exposes no
   accessibility bus. The only identity mechanism in the toolkit is the *test*
   probe, which resolves a control by its handler pointer. VoiceOver, Switch
   Control, Voice Control, Hover Text, Braille, Assistive Access and Full Keyboard
   Access all need this and nothing else exists to build them on. **~14 rows.**
3. **No process isolation.** Everything runs as root in one namespace
   (`meta/initramfs/init`), app dirs under `<data>/apps/<app_id>/` are convention
   only (`sdk/src/storage.c:112` — "not per-app namespaces"), and zsysd trusts the
   `app_id` string each client sends about itself with no `SO_PEERCRED` check. On
   iOS the sandbox is the primitive every privacy guarantee rests on; here there
   is no counterpart, so the privacy section is structurally rather than
   incrementally short. **~20 rows.**
4. **No audio stack.** No ALSA, PipeWire, PulseAudio or `snd_pcm` anywhere —
   `sys.volume` and `sys.mute` are numbers with a HUD that control nothing.
   Playback, now-playing controls, ringtones, dictation, Siri, Live Listen,
   Sound Recognition, Music Haptics, AirPods, spatial audio. **~22 rows.**
5. **No radios.** No cellular, Wi-Fi, Bluetooth, NFC or UWB driver. The status
   bar's cellular bars are drawn from `system/common/glyphs.h` against a
   `sys.signal` key nothing writes. Calls, SMS, eSIM, AirDrop, Handoff, CarPlay,
   Find My, hotspot, Apple Pay. **~18 rows.**
6. **No output transform.** `compositor/src/output.c` sets enabled + mode and
   never calls `wlr_output_state_set_transform`; the accelerometer is never read
   for orientation. Portrait 720×1440 only — landscape, auto-rotate, rotation
   lock, external display. **~5 rows.**

There is also one **security defect** that is not a missing feature but a wrong
one: `sys.passcode` is an ordinary settings key. `setting_is_broker_owned()`
(`system/zsysd/main.c:278`) protects exactly two keys — `sys.camera_in_use` and
`sys.notif_count` — so any connected client can read the lock-screen passcode in
plaintext and rewrite it. It is also compared with `strcmp` and stored unhashed in
`settings.conf`. Tracked in §11.

---

## 1. Touch, gestures & input

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Tap | | ✅ | `OnTap` / `OnTapData`, `sdk/include/zelto/ui.h:1016` |
| Double tap | | ❌ | No recognizer. The keyboard hand-rolls one off `z_now_seconds()` for caps-lock (`system/keyboard/main.c`) |
| Long press | | ✅ | `OnLongPress`; composes with tap/pan — slop-cross becomes a pan, still-hold fires and suppresses the tap. `ui.h:1060` |
| Pan / drag | | ✅ | `OnPan`, `ZPanEvent` with translation, velocity and BEGIN/CHANGED/END phase. `ui.h:1031` |
| Swipe (directional recognizer) | | ❌ | No `OnSwipe`, no direction enum. Every directional swipe in the OS is hand-built on `OnPan` |
| Screen-edge pan (back gesture) | | 🟡 | Exactly one, hard-wired: the Navigator back-swipe, interruptible and 1:1 with the finger (`sdk/src/app.c`, `sdk/src/navigation.c`). Not a general edge recognizer apps can install |
| Fling / inertia | | ✅ | `advance_fling` plus gesture→spring velocity hand-off and mid-flight capture (`z_animated_grab`). `sdk/src/animation.c` |
| Rubber-band overscroll | | ✅+ | `z_rubber_band()` is iOS's own `b(x)=x·c·d/(x·c+d)` with c=0.55, inline, no libm. `ui.h:1175` |
| Pinch to zoom | | ❌ | Blocked on root-cause 1. `zwp_pointer_gestures` bound on neither side |
| Rotation gesture | | ❌ | Root-cause 1 |
| Two-finger scroll | | ❌ | Root-cause 1 |
| Three-finger swipe (undo/redo/copy) | | ❌ | Root-cause 1 |
| Four/five-finger app switch & pinch-home | iPad | ❌ | Root-cause 1 |
| Multi-touch generally | | ❌ | `compositor/src/seat.c:476` — pointer capability only |
| Shake to undo | | ❌ | No undo stack anywhere, and the accelerometer is synthetic |
| Drag and drop, within an app | | 🟡 | No DnD API. Buildable from `OnPan` + `z_animated_keyed` + `OffsetXYAnimated` — which is exactly what the launcher's bento reorder does (`system/launcher/main.c`) |
| Drag and drop, between apps | | ❌ | `wl_data_device` DnD events are explicit no-op stubs and no `start_drag` call exists (`sdk/src/app.c`) |
| Spring-loading (hover-to-open during drag) | | 🟡 | Not a DnD feature, but the same mechanic exists: cross-page drag uses an edge-gutter dwell timer, 52 units / 420 ms (`system/launcher/main.c`) |
| Press-and-hold to reorder | | ✅ | Launcher rearrange mode: lifted ghost, live spring reflow of siblings, remove badge, cross-page drag (`system/launcher/main.c`) |
| Haptic Touch / long-press preview | | 🟡 | Long-press exists; there is no preview/peek presentation and no haptic |
| 3D Touch (force) | dropped | ⛔ | Removed from iOS hardware too. Press *feedback* is visual: a spring-scaled `Z_COLOR_PRESS` veil (`sdk/include/zelto/gfx.h:411`) |
| Haptic feedback engine | | ❌ | Zero hits for `haptic`, `vibrat`, `rumble` or `taptic` repo-wide. No motor, no API |
| Hover (pointer / Pencil) | | 🟡 | The compositor tracks `hover_surface` only to promote it to the implicit pointer grab; no node ever gets a hover state |
| Pointer / mouse / trackpad | | 🟡 | Motion, buttons and vertical wheel yes; horizontal wheel ignored (`sdk/src/app.c`), no trackpad gestures |
| Hardware keyboard | | ✅ | xkbcommon both sides, `OnKey`, focusable nodes with a focus ring, Enter/Space activate, Escape/Backspace pops navigation |
| Full Keyboard Access | | ❌ | Focus ring exists; no system-wide keyboard-driven control of every element |
| Apple Pencil / stylus | | ❌ | `wlr_tablet_v2` not bound |
| Game controller | | ❌ | Not started |
| Back Tap (double/triple tap on the back) | | ❌ | Needs accelerometer pattern detection; the sensor is synthetic |
| Reachability (pull the top half down) | | ❌ | Not started |
| Implicit pointer grab (drag survives leaving the surface) | | ✅ | `compositor/include/zcomp/server.h` `grab_surface` — a real Wayland-side fix, not an iOS-named feature |
| Coordinate-free control resolution (test/automation) | | ✅+ | No iOS analogue. `z_probe_tap` resolves a control by its **handler pointer**, hit-tests its own centre and dispatches what the layout walk found, so no test carries a coordinate. `ui.h:2187` |
| Classified (non-hit-tested) touch targets | | ✅+ | No iOS analogue is public. A keyboard press is `argmax P(touch \| key)·P(key \| prefix)`, so the 'l' target reaches 19 units into the painted 'k' cap and the dead gutter is 0.0%. `system/keyboard/predict.h` |

## 2. System navigation & multitasking

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Swipe up to Home | | ✅ | Decided on **release velocity**, not distance: ≤ −900 px/s. `system/homebar/main.c` |
| Home indicator pill | | ✅ | 5 pt pill at 36% width, shadowed to read over any wallpaper, tightens under the finger. `system/homebar/main.c` |
| Swipe up and hold → App Switcher | | ✅ | Slow drag ≥ 200 px forks `zelto-recents`. `system/homebar/main.c` |
| Swipe sideways on the home bar → adjacent app | | ✅ | Launcher skipped from the ring. `system/homebar/main.c` |
| App Switcher card deck | | ✅ | Horizontal deck, 74%×62% cards, backdrop-blurred. `system/recents/main.c` |
| Live window thumbnails in the switcher | | ✅+ | Custom `zelto-toplevel-capture-v1`: the compositor photographs a window on the active→inactive **edge**, owns the buffer so it outlives the app, suppresses capture while a modal holds the screen, and honours a manifest `no_snapshot=1`. `compositor/src/capture.c`, `protocols/zelto-toplevel-capture-v1.xml` |
| Flick a card up to quit | | ✅ | 90 px or −700 px/s, fades as it leaves. `system/recents/main.c` |
| Back swipe inside an app | | ✅ | Navigator pop, interruptible, plus Escape/Backspace. `sdk/src/navigation.c` |
| System-wide back | | ❌ | Back is per-app (Navigator) only |
| Split View / Slide Over | iPad | ❌ | Every toplevel is sized to the full usable box; strict MRU stack (`compositor/src/toplevel.c`) |
| Stage Manager | iPad | ❌ | Not started |
| Picture in Picture | | ❌ | No video pipeline and no floating-window class |
| External display / mirroring | | ❌ | Each output gets a scene, but there is one shared `usable` box and no per-output policy |
| Force quit from the switcher | | ✅ | `z_task_close` via foreign-toplevel-management |
| App pinning / Guided Access | | ❌ | Not started |
| Multiple windows per app | | ❌ | One toplevel per app by design |

## 3. Home Screen & App Library

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Icon grid | | ✅ | Occupancy-packed bento grid, 4 columns of square cells. `system/launcher/main.c` |
| Multiple home pages | | ✅ | `MAX_PAGES 8`, pages are a pure function of the pack |
| Horizontal paging with fling snap | | ✅ | Velocity snap plus clamp-based rubber band. `system/launcher/main.c` |
| Page dots | | ✅ | One per page, plus a distinct quad mark for the App Library page |
| Free icon placement (iOS 18) | 26 | 🟡 | Placement is a persisted ordered CSV (`home.layout`), first-fit packed — you choose order, not an arbitrary empty slot |
| Folders | | ❌ | Not started |
| App icon badges | | ❌ | zsysd accepts `notify_badge` and only logs it — `"(forward Planned)"`, `system/zsysd/main.c` |
| Jiggle / wobble edit mode | | 🟡 | Edit mode exists with a lifted ghost, alignment raster, remove badges and a Done bar — but no wobble animation |
| Remove / delete app from Home | | 🟡 | Remove-from-Home yes (red × badge). Uninstall: no code path exists anywhere |
| Dock | | ✅ | 4 slots, persisted as `home.dock`, on a material plate, pinned across pages |
| Widgets on Home | | 🟡 | Three built-ins (clock, battery, notifications), self-refreshing, reorderable, span cells. The widget set is a fixed C table — no third-party widget protocol |
| Widget sizes (S/M/L/XL) | XL in 27 | 🟡 | Cells carry a `cw × ch` span, and widgets go **full-width** at accessibility text sizes. No published size family |
| Widget stacks / Smart Stack | | ❌ | Not started |
| Interactive widgets | 17 | ❌ | Widgets render; they take no input |
| Widget gallery / "Add Widget" | | ❌ | The set is compiled in |
| App Library | | ✅ | The last page(s) of the same carousel, name-sorted, paginated. `system/launcher/main.c` |
| App Library auto-categories | | ❌ | Flat alphabetical list, no category clustering |
| App Library search | | 🟡 | Live `TextField`, raises the keyboard, insets the page by `ZELTO_KBD_H`, matches name **or** app id. App names only — not a system search |
| Swipe-up app drawer | Android | ⛔ | Existed and was deleted; the job moved to the App Library page (`system/launcher/main.c` header) |
| Spotlight / system search | | ❌ | No system index. iOS 27 folds Spotlight into the Siri "Search/Ask" surface; neither exists |
| Today View | | ❌ | Not started |
| Icon tinting / dark / clear icons | 18–26 | ❌ | PNG icons rendered as-authored |
| App-open zoom transition | | 🟡 | Tapped tile drifts toward screen centre while the rest of home fades back — a continuity cue, not a true zoom. No scale primitive; compositor-owned zoom deferred (`system/launcher/main.c`) |
| Wallpaper | | ✅ | `Cover(Image)` + bottom vignette, live on `sys.wallpaper`. `system/apps/settings/main.c` picker |
| Separate Home / Lock wallpapers | | ❌ | One `sys.wallpaper` key shared by both |
| Dynamic / live wallpaper | | ❌ | Static images plus a banded gradient fallback |
| Depth-effect wallpaper | | ❌ | No subject segmentation |
| Photo Shuffle | | ❌ | Not started |
| AI wallpaper extension | 27β | ❌ | No generative model |
| Set a photo as wallpaper from Photos | | ✅ | Photos ▸ Set as Wallpaper writes `sys.wallpaper` |
| Toast / transient confirmation | | ✅ | Spring entrance, swipe-down dismiss ("Added to Home", "Home is full") |

## 4. Lock Screen, StandBy & Always-On

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Lock screen with clock + date | | ✅ | Display-size clock over the date, on wallpaper + scrim. `system/lock/main.c` |
| Compact top clock option | 27β | ❌ | Single fixed clock treatment |
| Swipe up to unlock | | ✅ | From the home-indicator zone only (200 units); plate lifts 1:1 and fades; velocity-vs-distance split shared with the home gesture |
| Passcode entry | | 🟡 | 4-digit round keypad, filling dots, "Wrong Passcode" in danger red. See §11 — the code is stored plaintext in a client-writable key |
| Alphanumeric passcode | | ❌ | 4 digits only |
| Failed-attempt lockout / escalating delay / erase-after-10 | | ❌ | `strcmp` with no counter, no delay, no wipe |
| Face ID | | ❌ | `docs/system-apis/biometrics.md` documents an API with no implementation |
| Touch ID | | ❌ | Same |
| Notifications on the Lock Screen | | ✅ | Up to 4 cards, tail summarised as "N more notifications". zsysd fans out to a **set** of sinks so the lock screen is a peer of the shade, not a copy |
| Interacting with lock-screen notifications | | ⛔ | Cards are deliberately non-interactive while locked (`system/lock/main.c`) |
| Notification display style (count/stack/list) | | ❌ | One list style |
| Lock Screen widgets | | ❌ | Not started |
| Lock Screen customisation gallery | | ❌ | Not started |
| Depth effect / subject layering | | ❌ | Not started |
| Camera & flashlight shortcut buttons | 18 | ❌ | No flashlight; camera is not reachable from the lock screen |
| Always-On Display | hw | ❌ | Not started |
| StandBy | 17 | ❌ | Not started |
| Live Activities | 16 | ❌ | Nothing persists on-screen past a banner |
| Dynamic Island | hw | ⛔ | Explicitly declined in `system/common/safe_areas.h` — the design has no cutout to work around |
| Emergency call / Medical ID from the lock screen | | ❌ | No telephony |
| Raise to Wake / Tap to Wake | | ❌ | No accelerometer wake path |
| Idle → dim → lock → screen-off state machine | | ✅+ | Four states driven by `ext-idle-notify` with all three delays user-settable, and the policy deliberately lives in the client rather than the compositor. Dim is input-transparent so a tap both wakes *and* reaches the app. `system/lock/main.c` |
| Real screen-off (DPMS) | | 🟡 | "Off" is an opaque black overlay, not DPMS — a deliberate call so a QMP screendump can still verify it (`system/lock/main.c:32`) |
| Lock now | | ✅ | Settings action row **and** long-press anywhere on the status bar, both bumping `sys.lock_now` |

## 5. Notifications

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Post a local notification | | ✅ | `notify_post`, permission-gated on `notifications`. `system/zsysd/main.c` |
| Heads-up banner | | ✅ | Overlay strip, spring slide+fade, up to 8. `system/shade/main.c` |
| Swipe a banner away | | ✅ | Swipe up hides the banner and keeps the notification in the panel |
| Pull a banner down to expand | | 🟡 | Dragging down opens the whole Notification Center, not that one notification |
| Notification Center | | ✅ | Full-bleed panel, clock + date, live cards, plus a dimmed "recently dismissed" history of 6 |
| Grouping / stacking by app | | ❌ | Flat list. This is the single most visible notification gap |
| Thread-level grouping | | ❌ | No thread identifier |
| Scrolling in the panel | | ❌ | `system/shade/main.c` — "Neither list scrolls yet (no Scroll here)" |
| Clear all | | ❌ | Individual dismissal only |
| Per-notification swipe actions (Clear / Options / View) | | ❌ | Not started |
| Notification actions (buttons) | | ✅ | One action button per card, routed back to the poster's mailbox — and the poster is **launched** if it is not running |
| Inline reply | | ❌ | Needs a text field inside a notification |
| Attached image | | ✅ | `zelto_notif_card_img`; the screenshot notification carries its own thumbnail |
| Tap to open (deep link) | | ✅ | `tap_route` → `z_open_url`, e.g. `zelto://photos` |
| App icon badges | | ❌ | `notify_badge` is accepted and logged only |
| Notification channels / per-app settings | | ❌ | `notify_channel` is parsed and logged, never stored or enforced |
| Priority tiers (passive / active / time-sensitive / critical) | 15 | ❌ | One tier |
| Scheduled Summary | 15 | ❌ | Not started |
| Notification summaries (AI) | 26 | ❌ | No model |
| Announce Notifications | | ❌ | No audio |
| Push notifications (APNs) | | ❌ | Local only. No push service, no server, no token |
| Survive a reboot | | ❌ | The store is in-memory (`system/zsysd/main.c`) — everything is lost on restart |
| Live notification count as a system fact | | ✅+ | `sys.notif_count` is **broker-owned**: `setting_is_broker_owned()` refuses a client write, so an app cannot forge the count kept about it |
| Notification Center on the Lock Screen | | ✅ | See §4 |

## 6. Control Center & system controls

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Pull down from the top-right for Control Center | | ✅ | Inset floating sheet, thick material + compositor blur. `system/shade/main.c` |
| Pull down from the top-left for Notification Center | | ✅ | Same surface, side decides which. `system/shade/main.c` |
| Airplane Mode | | ✅ | `sys.airplane` — really gates the network stack and replaces the bar's cellular glyph |
| Wi-Fi toggle | | 🟡 | `sys.wifi` gates `z_net_send` and the glyph. There is no radio behind it |
| Bluetooth toggle | | ❌ | No stack |
| Cellular data toggle | | ❌ | No modem |
| Signal strength / LTE-5G readout while on Wi-Fi | 27 | ❌ | `sys.signal` is decoration; nothing writes it |
| AirDrop | | ❌ | Root-cause 5 |
| Personal Hotspot | | ❌ | Root-cause 5 |
| Brightness slider | | ✅ | Tall slab slider, 1..5 quantised, → a scrim alpha of `(5−level)·48` painted by `system/dim/main.c`. Software scrim, not a backlight write |
| Volume slider | | 🟡 | `sys.volume` 0..10 with a HUD; nothing plays |
| Volume HUD on the hardware rocker | | ✅ | Left-edge capsule (deliberately not a centre-screen desktop OSD), self-dismisses after 1500 ms, swipe-up to dismiss. `system/volume/main.c` |
| Separate ringer / media / alarm volume | 27 | ❌ | One `sys.volume` |
| Silent / ringer switch | | 🟡 | `sys.mute` toggle exists and paints the HUD; nothing is silenced |
| Screen Mirroring | | ❌ | Root-cause 5 |
| Screen Recording | | ❌ | Screencopy exists for stills only |
| Rotation lock | | ❌ | Root-cause 6 — there is no rotation to lock |
| Flashlight | | ❌ | No torch |
| Focus / Do Not Disturb | | ❌ | Not started |
| Focus filters | 16 | ❌ | Not started |
| Now Playing tile | | ❌ | Root-cause 4 |
| Calculator / Timer / Camera shortcuts | | ❌ | Those apps do not exist |
| Low Power Mode | | ❌ | Battery level is read; there is no power policy |
| Custom Controls / third-party controls | 18 | ❌ | The six toggles are compiled in |
| Multi-page Control Center | 18 | ❌ | One page |
| Long-press a control to expand | | ❌ | Toggles are binary |
| Reorder / customise controls | | ❌ | Fixed 3×2 grid |
| Action Button | hw | 🟡 | Not the iOS feature, but the equivalent exists: compositor chords for Home, app-cycle, screenshot and the three volume keys (`compositor/src/seat.c`) |
| Camera Control | hw | ❌ | No such hardware |
| True Tone / Night Shift / auto-brightness | | ❌ | The `light` sensor streams and nothing reads it |
| Reduce Motion (as a system control) | | ✅ | In Control Center *and* Settings — `sys.reduce_motion` collapses every spring in the toolkit, in every process |

## 7. Text input & keyboard

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| On-screen keyboard | | ✅ | Single system input-method-v2 client. `system/keyboard/main.c` |
| QWERTY letters layer | | ✅ | |
| Numbers & symbols layer | | 🟡 | One symbols layer (`1234567890` / `@#$%&-+()/` / `.,?!'";:`). iOS has two |
| Number row | | ❌ | Not started |
| Shift: off / one-shot / caps lock | | ✅ | Caps lock on double-tap within 0.35 s, with lit-key rendering |
| Auto-capitalisation | | ✅+ | Sentence start is read from **text-input-v3 surrounding text**, not from an echo of the keyboard's own keystrokes — so it is right after a paste or a caret move too. Only when the field declares it |
| Long-press for accents | | ✅ | 0.5 s → popup row of Latin-1 alternates, slide to select, upper-cased when shifted by a *rule* rather than a second table |
| Key preview callout | | ✅+ | Shows what the **classifier** chose, not what is under the finger — so the preview can never disagree with what is typed |
| Repeating backspace | | ✅+ | Three cadences: 0.15 s, then 0.06 s after 1.2 s, then **whole words** after 2.0 s |
| Double-space → ". " | | ✅ | |
| Autocorrect | | ✅ | Damerau-Levenshtein over a 1620-word trie + a bigram backoff LM, memoised per word and model generation. Fires on **every** word boundary, not only space |
| Visible, revertible correction | | ✅+ | One backspace reverts, guarded against a disturbed tail, and the revert is what teaches the dictionary |
| Predictive suggestion strip | | ✅ | Three slots: dictionary guess / **the literal you typed** / second guess |
| Learned user dictionary | | ✅+ | Learns after 3 uncorrected boundaries or 1 revert. Counts **never hit disk**; nothing from a password field ever enters; clearing is an **epoch** the keyboard compares, so a clear is honoured even if the keyboard was not running when you asked |
| Contraction expansion ("dont" → "don't") | | ✅ | Through the same visible correction path |
| Text replacement / user shortcuts | | ❌ | Not started |
| Emoji keyboard | | ❌ | Root cause is the font: one face, no fallback chain, no `FT_LOAD_COLOR`/COLR/CBDT |
| Globe key / multiple languages | | ❌ | English only |
| Swipe / glide typing (QuickPath) | | ❌ | Not started |
| One-handed keyboard | | ❌ | Not started |
| Split keyboard | iPad | ❌ | Not started |
| Floating / undocked keyboard | | ❌ | Not started |
| Dictation | | ❌ | Root-cause 4 |
| Keyboard clicks / haptics | | ❌ | Root-cause 4 and no motor |
| Trackpad cursor mode (long-press space) | | ❌ | Not started |
| Third-party keyboards | | ❌ | One system input method, hard-wired |
| Password fields disable learning & adaptive targets | | ✅+ | Driven by the real `text-input-v3` content purpose — the classifier reverts to plain hit-testing, autocap is off, the suggestion strip is dropped from the exclusive zone, and nothing is learned |
| Text field | | 🟡 | `ZTextField`, single line, 256-byte cap, app-owned buffer. No multi-line text view |
| Caret | | ✅ | `sdk/src/view.c` |
| Selection by long-press, extend by drag | | ✅ | Word select on long-press, highlight run, two drag handles |
| Selection loupe / magnifier | | ❌ | Not started |
| Copy / cut / paste / select all | | ✅ | Floating `z_selection_bar` |
| System clipboard | | 🟡 | Text/plain only. Writes via core `wl_data_device`, reads via `wlr-data-control-v1` so the focus-less keyboard can paste; the async read is parked in the app loop |
| Universal Clipboard (cross-device) | | ❌ | No second device, no continuity |
| Look Up / Translate / Share in the selection menu | | ❌ | The bar carries editing commands only |
| Undo / redo | | ❌ | Only autocorrect's one-step revert |
| Spell check (red underline) | | ❌ | The dictionary exists; nothing marks text |
| Live Text (text in images) | | ❌ | No OCR |
| RTL / bidirectional text | | ❌ | One `hb_shape` over the whole string, no bidi reordering, no per-run direction, `Z_ALIGN_LEADING` == left |
| CJK / IME composition | | 🟡 | The relay forwards the IME's preedit to the text-input, but the SDK exposes **no client API to set or read preedit** |
| Hardware keyboard typing into a field | | ⛔ | Text arrives only via `commit_string`; the on-screen keyboard types for itself. Recorded in `CLAUDE.md` as a trap, not a bug |

## 8. UI toolkit (UIKit / SwiftUI equivalents)

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Declarative view tree with a reconciler | | ✅ | Keyed identity on `ZNode`, retained cells GC'd when a build stops asking. `sdk/src/reconcile.c` |
| VStack / HStack / ZStack | | ✅ | `ui.h:489`; `ZStack` centres children at their own size unless `Fill()` |
| Frame (fixed size) | | ✅ | `ui.h` — note a fixed height is an **inner** height when the node also has padding |
| Padding | | ✅ | Insets both axes |
| Spacer | | ✅ | |
| Flex grow | | ✅ | `Grow` divides the **slack** |
| Flex basis / `flex: N` | | ✅+ | `Share()` divides the **axis** — added after two shipped bugs where `Grow` siblings came out the width of their own labels (key caps, Control Center toggles) |
| Fill parent | | ✅ | `Fill()` |
| Clip to subtree | | ✅ | `Clip(radius)` masks descendants and nests/intersects; distinct from `CornerRadius`, which masks a node's own paint |
| Corner radius | | ✅+ | Continuous squircle at n=4, implemented twice — SDK and compositor — and required by test to agree, because the blurred backdrop must match the client's own corner |
| Shadow / elevation | | ✅ | SDF rounded-rect penumbra, three elevation tokens |
| Opacity | | 🟡 | Subtree alpha multiply; not group-correct for overlapping translucent children (documented) |
| Absolute offset | | ✅ | `OffsetXY`, `OffsetXYAnimated` |
| Scroll view | | ✅ | `Scroll`, retained state, `z_scroll_to(animated)` |
| Scroll indicators / scrollbars | | ❌ | Zero hits repo-wide |
| Paging scroll view | | 🟡 | The home carousel pages; there is no paging flag on `Scroll` |
| Pull to refresh | | ❌ | Not started |
| List / table | | 🟡 | Virtualised and keyed, but requires a **fixed** `row_height` |
| List sections, headers, footers | | ❌ | Flat rows |
| Swipe actions on a row | | ❌ | Not started |
| Row reorder | | 🟡 | Not in `List`; the launcher implements its own |
| Separators | | 🟡 | Hairlines are hand-drawn in Settings, not a list feature |
| Grouped inset list style | | ✅ | `system/common/app_chrome.h` `z_section` / `z_card` + the Settings screens |
| Navigation stack | | ✅ | `Navigator`, push/pop/depth, slide + cross-fade, interruptible back swipe |
| Named routes / deep navigation restore | | ❌ | One anonymous stack |
| Tab bar | | ❌ | Not started |
| Navigation bar / toolbar | | 🟡 | `z_screen_title` in `system/common/app_chrome.h` — a helper, not an SDK type |
| Sheet / modal presentation | | ❌ | Absent from the SDK. Hand-built three times: `system/chooser`, `system/share`, `system/consent` |
| Sheet detents / drag-to-dismiss | | 🟡 | The share sheet has a grabber, a spring entrance, drag-down dismiss at 90 px / 700 px/s and a blurred backdrop — but it is bespoke code, not reusable |
| Alert | | ❌ | The consent dialog is a bespoke exclusive-keyboard layer app |
| Action sheet | | ❌ | Not started |
| Popover | | ❌ | Not started |
| Menu / context menu | | ❌ | `OnLongPress` reports the press position "e.g. to place a menu" — there is no menu |
| Button | | ✅ | `ui.h:666` |
| Switch / toggle | | 🟡 | Not in the SDK. Settings hand-builds a real 51×31 pt switch with a sliding knob |
| Slider | | ✅ | Relative drag, `on_change` + `on_commit`, `.tall` Control-Center slab |
| Stepper | | 🟡 | Settings hand-builds a split minus/plus pill; not an SDK type |
| Segmented control | | ❌ | Not started |
| Picker / date picker | | ❌ | Not started |
| Progress view / activity indicator | | ❌ | Not started |
| Search bar | | 🟡 | A `TextField` used as one in the App Library |
| Page control (dots) | | 🟡 | Hand-built in the launcher |
| Widget (home-screen) | | ✅ | `Widget(.title,.body,.refresh_ms)` self-refreshing via `z_tick_every` |
| Image (raster) | | ✅ | PNG via libpng, aspect-fit, decode cached by path |
| Aspect-fill / centre-crop | | ✅ | `Cover()` |
| SVG / vector assets | | 🟡 | In-house stroker: line icons, single ink. No fills, gradients or transforms |
| SF Symbols equivalent | | ❌ | A hand-rolled glyph set in `system/common/glyphs.h`, not a named library with weights and scales |
| Vector mark primitive | | ✅ | `Stroke()` — round-capped AA polyline in the unit box, scales to any frame |
| PNG encode / thumbnailing | | ✅ | `z_image_write_png` with atomic rename and an explicit premultiplied flag; `z_image_box_scale` |
| Gradients | | 🟡 | Hand-painted bands (the wallpaper fallback, the vignette); no gradient node |
| Blur / vibrancy materials | | ✅+ | Custom `zelto-backdrop-v1`: the client declares a rect + corner radius, the compositor blurs the scene below at ¼ res, rate-limited under vsync, masked to the same squircle, and parks it beneath the surface. Degrades to tint-only. `compositor/src/backdrop.c` |
| Material tokens (thin/regular/thick) | | ✅ | Four plus a hairline edge, `sdk/include/zelto/gfx.h:224` |
| Liquid Glass | 26 | ❌ | The material system is the P44-era iOS look, not the 26/27 one |
| Springs | | ✅ | Three role tokens; `z_animated_*`, velocity injection, mid-flight capture |
| Keyframe / timeline / easing curves | | ❌ | Animation is scalar springs bound to `Offset` and `Opacity` only |
| View-controller transitions | | 🟡 | Only the Navigator's slide + cross-fade (with a freeze hook for screenshots) |
| Matched-geometry / hero transitions | | ❌ | Not started |
| Deterministic animation stepping (test) | | ✅+ | `z_anim_tick(dt)` and `ANIM_FRAMES` freeze-step — no iOS analogue; it is what makes the screenshot catalogue reproducible |
| Damage-tracked partial repaint | | ✅ | Per-rect, with `z_full_repaint()` as the escape hatch for big translated subtrees |
| Safe-area insets | | 🟡 | Real and enforced via layer-shell exclusive zones (`system/common/safe_areas.h`), but there is no `z_safe_area_insets()` an app can ask |
| Video view | | ❌ | Root-cause 4 |
| WebView | | ❌ | No web engine |
| PDF view | | ❌ | Not started |
| Map view | | ❌ | Location plumbing exists; no map |

## 9. Typography, colour & theming

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Semantic type scale | | ✅ | 11 steps + a display size, authored in **points** and converted once through `Z_PT` (185/100) |
| Dynamic Type | | ✅+ | 12 steps (7 standard + AX1–AX5) behind **one** seam, `z_font_units()`. The ladder is a constant **point offset**, not a multiplier — off Apple's own table every style gains exactly +6 pt, which reproduces "large steps move less" from one rule |
| Dynamic Type layout reflow | | ✅+ | `z_text_size_reflows()` — the break is at AX2 because it was **measured** (a stepper key ending at x=753 on a 720-unit screen), not asserted. Rows stack their control under a wrapped label past it |
| Declared opt-outs from Dynamic Type | | ✅+ | `z_text_scaling_disable()` for the status bar, home indicator and keyboard — each with the reason recorded (exclusive-zone contracts; caps are touch targets and it is the *letters* that grow) |
| Bold Text | | ✅ | One function in `set_weight`, so measure and paint cannot disagree |
| Variable font axes | | 🟡 | Real `wght` driven per weight token, clamped to the face range, with faux-bold embolden as fallback. Only `wght` — no `ital`, `opsz`, `wdth` |
| Font fallback chain | | ❌ | One `FT_Face`. This is why there is no emoji and no CJK |
| Line height from the face | | ✅+ | `z_line_height()` reads real ascent+descent. A `font × 1.31` estimate lasted one phase — the true ratio runs 1.3182 down to 1.2529 across the scale |
| Row-height floor | | ✅ | `z_row_h()` = max(44 pt, line + 2·vpad) |
| Text wrapping | | ✅ | `WrapText` honours hard breaks and paragraph gaps and breaks over-long words. Needs its column width at **build** time |
| Truncation / ellipsis | | ✅ | `EllipsizeText` cuts on a UTF-8 boundary and guarantees intrinsic width ≤ the box |
| Tracking / letter-spacing | | ⛔ | Deliberately not modelled |
| Rich text / attributed strings | | ❌ | One size, weight and colour per `Text` node |
| Dark mode | | ✅ | Every token is a runtime lookup — each `Z_COLOR_*` macro expands to `z_token(Z_TOKEN_*)`, and none of the 121 migrated call sites had to learn that an appearance exists. `sdk/include/zelto/gfx.h` |
| Light mode / automatic appearance | | 🟡 | **Landing now (P54, uncommitted).** `sdk/src/theme.c` holds both appearances as one two-column table — the form a palette is actually reviewed in — and `test/test_contrast_tokens.c` computes the WCAG table over **both** columns so neither can quietly go wrong. Not yet wired: there is no `sys.*` key, so nothing switches it |
| Tint / accent colour | | ❌ | `Z_COLOR_PRIMARY` is a token, so it varies by appearance — but it is not user-settable |
| Semantic colour tokens | | ✅+ | `sdk/include/zelto/gfx.h` — and the WCAG contrast ratios are **computed from the tokens** in `test/test_contrast_tokens.c`, which is how `TEXT_FAINT` was caught at 2.17:1 on a chip while the header claimed "all AA" |
| Increase Contrast | | ✅+ | Three tokens get a second, *minimally* brightened value chosen to clear AA on the darkest surface each is actually drawn on. This was the palette's first runtime seam; P54 generalised it to every token |
| Wide gamut / P3 | | ❌ | sRGB, 8-bit |
| HDR | | ❌ | Not started |
| Liquid Glass transparency slider | 27 | ❌ | Materials are unconditional |

## 10. Accessibility

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Larger Text / Dynamic Type | | ✅+ | See §9 — 12 steps with a measured reflow break |
| Bold Text | | ✅ | `sys.bold_text`, live |
| Increase Contrast | | ✅+ | `sys.increase_contrast`, WCAG-measured tokens |
| Reduce Motion | | 🟡 | Every spring collapses to a jump, including the nav transition and the fling — but it is read **once at startup**, not live, and the key has four raw string literals instead of a `#define` |
| Reduce Transparency | | ❌ | Materials and backdrops are unconditional |
| Button Shapes | | ❌ | Not started |
| Differentiate Without Colour | | ❌ | Not started |
| On/Off Labels | | ❌ | Not started |
| Colour Filters / colour blindness | | ❌ | No output colour pipeline |
| Invert Colours (smart/classic) | | ❌ | Not started |
| Display brightness / auto-brightness / white point | | 🟡 | A five-step software scrim only |
| Larger touch targets | | 🟡 | A 44 pt floor exists as `min_h` on the **outer** box, plus a touch-target audit harness. Not user-adjustable |
| VoiceOver | | ❌ | Root-cause 2 — `ZNode` has no role, label, value, trait or hint field, and there is no accessibility bus |
| VoiceOver rotor / gestures / braille output | | ❌ | Root-cause 2 |
| Image Explorer (AI image descriptions) | 27β | ❌ | Root-cause 2 plus no model |
| Braille Access / braille displays | 26–27 | ❌ | Root-cause 2 |
| Zoom / screen magnification | | ❌ | No compositor output scaling |
| Magnifier app | | ❌ | Not started |
| Live Recognition (ask about the viewfinder) | 27β | ❌ | The camera is synthetic and there is no model |
| Hover Text | | ❌ | Root-cause 2 |
| Spoken Content / Speak Screen / Speak Selection | | ❌ | Root-cause 4 |
| Audio Descriptions | | ❌ | Root-cause 4 |
| Switch Control | | ❌ | Root-cause 2 |
| Voice Control | | ❌ | Root-causes 2 and 4 |
| AssistiveTouch | | ❌ | Not started |
| Dwell Control | | ❌ | Not started |
| Eye Tracking | 18 | ❌ | No front-camera tracking |
| Touch Accommodations (hold duration, ignore repeat) | | ❌ | Press timing is fixed in the SDK |
| Assistive Access (simplified shell) | 17 | ❌ | Not started |
| Guided Access | | ❌ | Not started |
| Live Captions | | ❌ | Root-cause 4 |
| Auto-generated captions for any video | 27β | ❌ | Root-cause 4 |
| Sound Recognition | | ❌ | Root-cause 4 |
| Background Sounds | | ❌ | Root-cause 4 |
| Headphone Accommodations / hearing aid features | hw | ❌ | Root-cause 4 |
| Mono Audio / balance | | ❌ | Root-cause 4 |
| Music Haptics | 18 | ❌ | No motor |
| Vehicle Motion Cues | 18 | ❌ | Sensors are synthetic |
| Personal Voice / Live Speech | 17 | ❌ | Root-cause 4 |
| Sign-language interpreter API in calls | 27β | ❌ | No calls |
| Accessibility Nutrition Labels (App Store) | 26 | ❌ | No store catalogue |
| Accessibility settings pane | | ✅ | Settings ▸ Accessibility — text sample card, 12-step Text Size slider with A/A legend, Bold Text, Increase Contrast, Reduce Motion |
| Automated accessibility audit | | ✅+ | No iOS analogue in-tree. The AX audit boots each surface at declared text-size baselines and reads three probe counters — `overflowing` must be zero; `clipped` and `sideways` are read as a **delta** against the default size, because a scroll and a carousel make an absolute zero impossible |

## 11. Privacy & security

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Per-app sandbox | | ❌ | Root-cause 3. `docs/platform/permissions.md` claims "Linux namespaces + seccomp"; no source file contains either |
| Per-app data isolation | | ❌ | `sdk/src/storage.c:112` — app dirs are "not per-app namespaces". Any app can read any other app's prefs, SQLite DBs and photos by path |
| Verified app identity | | ❌ | Every request carries its own `app_id` as a string; `SO_PEERCRED` is never used, so any app can act as any other |
| Runtime permission prompt | | ✅ | Overlay modal with an exclusive keyboard grab; "Don't Allow" on top, "Allow" nearest the thumb; **flick-down = Deny** (the safe default). `system/consent/main.c` |
| Undeclared permission is auto-denied | | ✅+ | `decide()` in `system/zsysd/main.c` never prompts for an undeclared permission — declaration is a gate *before* the human is asked, so an app cannot bury a prompt the manifest never disclosed |
| Grant re-checked at the point of use | | ✅ | `camera_open` re-runs `perm_status` rather than trusting the client, and streaming subscriptions re-check per tick |
| Allow Once | | ❌ | Binary allow/deny |
| While Using the App | | 🟡 | The only tier there is — and only as a side effect: `sdk/src/app.c` unsubscribes sensor and GPS streams when the app backgrounds |
| Always (background) tier | | ❌ | No background execution to grant |
| Ask Next Time | | ❌ | Not started |
| Precise vs approximate location | | ❌ | One fix, full precision |
| Permission review & revocation UI | | ❌ | No verb, no pane. The only way to revoke is to reboot — grants are in-memory (`system/zsysd/main.c`) |
| Grants survive a reboot | | ❌ | Deliberate for now, but it means every boot re-prompts for everything |
| Camera in-use indicator | | ✅+ | Green dot in the status bar from `sys.camera_in_use`, which is **broker-owned** — a client write is refused, so the app being indicated cannot switch off its own indicator. There is a red-team hook (`ZELTO_CAMERA_SUPPRESS`) whose job is to prove it fails |
| Microphone in-use indicator | | ❌ | No microphone |
| Location in-use indicator | | ❌ | Not published |
| Recently-used-sensor dot / Privacy Report | | ❌ | Not started |
| Screenshot suppression while a modal is up | | ✅ | Decided in the compositor, not the client (`compositor/src/seat.c`) |
| Window-snapshot opt-out (`FLAG_SECURE` equivalent) | | ✅ | `no_snapshot=1` in the manifest, resolved once at map through zsysd |
| Passcode | | 🟡 | Works, but see the next three rows |
| Passcode stored hashed | | ❌ | Plaintext in `settings.conf` |
| Passcode not readable by apps | | ❌ | `sys.passcode` is an ordinary settings key; `setting_is_broker_owned()` protects only `sys.camera_in_use` and `sys.notif_count`, so **any app can read and rewrite it** |
| Biometric authentication (LocalAuthentication) | | ❌ | Documented in `docs/system-apis/biometrics.md`, implemented nowhere |
| Keychain / secure credential store | | ❌ | Referenced by that same doc; no such component exists |
| Secure Enclave | | ❌ | No hardware and no software stand-in |
| Data Protection / encryption at rest | | ❌ | Plain ext4 on `/dev/vda`, no LUKS |
| Advanced Data Protection / E2E cloud | | ❌ | No cloud |
| App Tracking Transparency | | ❌ | Not started |
| Private Relay / Hide My Email | | ❌ | No accounts, no network service |
| Mail Privacy Protection | | ❌ | No mail |
| Lockdown Mode | | ❌ | Not started |
| Stolen Device Protection | | ❌ | Not started |
| Secure Boot / verified boot / dm-verity | | ❌ | Package signing is the only signature check in the system |
| Package signing & integrity | | ✅ | `.zap` = signed ZIP; Ed25519 detached signature over `MANIFEST.sha256` plus a per-file SHA-256 check, verified against one bundled root key. `system/installer/main.c` |
| Signed manifests cannot redirect `exec=` | | ✅+ | The **installer**, not the package, synthesises the exec line for script apps — so a signed manifest cannot be made to point at an arbitrary binary |
| Per-publisher signing keys | | ❌ | Single trusted root (`meta/keys/trusted.pub`) |
| Communication Safety / sensitive-content warning | | ❌ | Not started |
| Screen Time & parental controls | 27 adds more | ❌ | Not started |
| MDM / supervision | | ❌ | Not started |

## 12. App model & distribution

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| App bundle format | | ✅ | `.zap` — signed ZIP with `zelto.toml`, a native binary **or** a script entry, an icon and a hash manifest. `meta/mkzap.sh` |
| Code signing & verification at install | | ✅ | See §11 |
| Install | | ✅ | Verify → atomic unpack to `installed/<id>/` → register the manifest → `{"op":"reload"}` to zsysd. `system/installer/main.c` |
| Update, with downgrade refused | | ✅ | Same id + higher `version=` replaces; lower is refused; private data preserved |
| Uninstall | | ❌ | No code path exists |
| Offload / app size accounting | | ❌ | Not started |
| App Store catalogue, search, purchase | | ❌ | `system/apps/store/main.c` is two buttons that install a locally staged good `.zap` and a deliberately tampered one |
| In-app purchase / StoreKit | | ❌ | No accounts, no payments |
| Alternative marketplaces / sideloading | EU | ✅ | Sideloading is the only distribution model there is |
| App lifecycle states | | 🟡 | `Z_LC_ACTIVE` / `INACTIVE` / `STOPPED` off the xdg `activated` state. No suspend, no termination notice, no state restoration |
| Background App Refresh | | ❌ | `docs/guides/background-tasks.md` documents a capability model with no implementation |
| Background audio / location / processing | | ❌ | Root-cause 4; a backgrounded app is actively *de*-provisioned (its sensor streams are cut) |
| Silent / remote push wake | | ❌ | No push |
| Jetsam / memory-pressure termination | | ❌ | Nothing kills an app |
| Share sheet (as a source) | | ✅ | `z_share()` → chooser. Share **always** shows the chooser even for one candidate; `open_url` with one handler skips it |
| Share targets (as a destination) | | ✅ | `share_targets=text/plain,image/*` MIME globs, delivered to the app's mailbox with launch-and-queue if it is not running |
| Share sheet actions (Copy, Save to Files, Print, Markup) | | ❌ | Targets only, no action row |
| Suggested contacts row in the share sheet | | ❌ | No contacts |
| URL schemes | | ✅ | `links=zelto,myapp` |
| Universal Links (verified domains) | | ❌ | No domain verification |
| Default app selection | | 🟡 | First matching manifest wins; the chooser appears when >1. No user-settable default and no memory of a choice |
| App Intents / Shortcuts / Siri exposure | | ❌ | Intents here are share + open-url only, not an action vocabulary |
| Automations | | ❌ | Not started |
| App Clips | | ❌ | Not started |
| Extensions (share, action, widget, keyboard, notification-service) | | ❌ | No extension point of any kind |
| App icons from the bundle | | ✅ | An `assets/icon.png` inside the `.zap`, cached PNG decode (`sdk/src/image.c`) |
| Script apps as first-class citizens | | ✅+ | A QuickJS app carries the same manifest, permissions, intents and launcher tile as a C app, and the broker **cannot tell them apart**. Its `.js` is hash-covered like an ELF and the package is architecture-independent |

## 13. System services & data

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Brokered system daemon | | ✅ | zsysd — seven duties over one unix socket: permissions, intents, notifications, settings, power, sensors/location, camera in-use |
| Settings store with live fan-out | | ✅ | `settings_get` / `set` / `subscribe`; persisted TAB-separated and fsync'd; pushed to a set of subscribers. 25 `sys.*` keys, defaults centralised in `system/common/settings_defaults.h` |
| App prefs (UserDefaults) | | ✅ | `z_prefs_*`, app-scoped, fsync'd on write |
| Files on disk | | ✅ | `z_file_write/read/delete`, documents + cache dirs |
| SQLite | | ✅ | Vendored and real: `z_db_open/exec/run/query` scoped to one app's documents dir |
| Media library | | ✅ | `<data>/media/photos` + `thumbs`; identity is the filename stem and the **index is the directory itself** — deliberately not SQLite, because two writers would mean two sources of truth that can disagree (`system/common/photos.h`) |
| Files app / document picker | | ❌ | No picker and no document-provider API |
| iCloud Drive / cross-device sync | | ❌ | No accounts |
| Backup & restore | | ❌ | Persistence exists; nothing backs it up |
| Trash / undelete | | ❌ | Delete is immediate |
| Storage quotas & cache eviction | | ❌ | The cache dir is labelled evictable and nothing ever evicts it |
| Handoff | | ❌ | Root-cause 5 |
| Universal Clipboard | | ❌ | Root-cause 5 |
| AirDrop | | ❌ | Root-cause 5 |
| Continuity Camera / Sidecar | | ❌ | Root-cause 5 |
| Nearby Interaction (UWB) | | ❌ | No radio |
| Find My | | ❌ | Root-cause 5 |
| Wallet & Apple Pay | | ❌ | No NFC, no accounts |
| HealthKit | | 🟡 | A step-counter sensor type streams; there is no health store and no app |
| HomeKit | | ❌ | Not started |
| Screen Time | | ❌ | Not started |
| Journaling Suggestions | | ❌ | Not started |
| Translate | | ❌ | Not started |
| Measure / LiDAR | | ❌ | No depth sensor |

## 14. Hardware & device

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Camera capture | | 🟡 | Real stack, **synthetic source**. Frames are generated in-process and each one carries its sequence number in the top-left pixel's red channel so the plumbing is testable. The seam is one function, `camera_source_fill()` (`sdk/src/camera.c`) |
| Multiple lenses / zoom | | ❌ | One synthetic stream |
| Video recording | | ❌ | Stills only |
| ProRAW / ProRes / Cinematic / Night mode | | ❌ | Not started |
| Front camera / selfie | | ❌ | One stream |
| Microphone | | ❌ | Root-cause 4 — no permission, no API, no code |
| Speaker / audio routing | | ❌ | Root-cause 4 |
| Haptic engine | | ❌ | No motor |
| Accelerometer, gyroscope, magnetometer | | 🟡 | Types exist and stream at a clamped [1,60] Hz; values are static per `ZELTO_SIM_*` env so screenshots stay deterministic |
| Orientation, gravity, linear acceleration, rotation vector | | 🟡 | Same — 11 sensor types total |
| Ambient light sensor | | 🟡 | Streams; **nothing reads it** (hence no auto-brightness) |
| Proximity sensor | | 🟡 | Streams; nothing reads it (no screen-off during calls — there are no calls) |
| Barometer / pressure | | 🟡 | Streams synthetic |
| Pedometer / step counter | | 🟡 | The one synthetic sensor that actually advances |
| LiDAR / depth | | ❌ | Not started |
| GPS / location | | 🟡 | Streams + one-shot `location_get` with lat/lng/accuracy/altitude/speed/bearing, from `ZELTO_SIM_LOCATION` (default Berlin). No geocoding, geofencing or significant-change |
| Face ID / Touch ID hardware | | ❌ | See §11 |
| NFC | | ❌ | Not started |
| Ultra Wideband | | ❌ | Not started |
| Battery level & charging state | | ✅ | Real `/sys/class/power_supply/*/capacity` read with a fake-drain fallback behind `ZELTO_FAKE_BATTERY`; published as `sys.battery_pct` / `sys.battery_charging` |
| Low-battery warning | | ✅ | One-shot notification at ≤20% **plus** an automatic brightness nudge to level 2 |
| Battery health / cycles / time remaining | | ❌ | Level and charging only |
| Low Power Mode | | ❌ | Not started |
| Wireless / MagSafe charging | | ❌ | Not started |
| Screen brightness (real backlight) | | ❌ | A software scrim, not a sysfs write |
| ProMotion / variable refresh | | ❌ | Not started |
| Rotation / auto-rotate | | ❌ | Root-cause 6 |
| Display scale / HiDPI | | ⛔ | No output scale is ever set — all metrics are raw device units by design, `1 pt = 1.85 units` |
| GPU acceleration for apps | | ❌ | The compositor uses GLES2 (pixman fallback); **apps are entirely software-rendered** (`sdk/src/render.c`). No Metal/Core Animation equivalent, no shaders, no hardware video decode |
| Thermal management | | ❌ | Not started |
| Persistent storage | | ✅ | virtio-blk ext4 at `/var/zelto`, mounted by init with a loud tmpfs fallback and a 2 s `sync` loop |

## 15. Connectivity

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Cellular data | | ❌ | Root-cause 5 |
| Phone calls / SMS | | ❌ | Root-cause 5 |
| SIM / eSIM / dual SIM | | ❌ | Root-cause 5 |
| Same number on two iPhones | 27 | ❌ | Root-cause 5 |
| Signal-strength indicator | | 🟡 | Drawn from `sys.signal`, which defaults to 4 bars and which **nothing ever writes** |
| Airplane mode | | ✅ | Really gates `z_net_send` / `z_ws_open` and swaps the bar glyph |
| Wi-Fi | | ❌ | No scanning, SSIDs, wpa_supplicant or association — `sys.wifi` is a boolean |
| Bluetooth | | ❌ | Not started |
| Personal Hotspot | | ❌ | Not started |
| HTTP client | | 🟡 | Hand-rolled non-blocking HTTP/1.x driven from the app's own poll loop, so it never blocks the renderer. Permission-gated with async consent resume. Max 8 concurrent |
| WebSocket | | 🟡 | HTTP/1.1 upgrade, unfragmented masked text frames. Binary and fragmented frames are a documented stub |
| DNS | | ❌ | Numeric IPv4 hosts only — you cannot fetch a hostname (`sdk/src/net.c:13`) |
| TLS / HTTPS | | ❌ | Everything is cleartext |
| HTTP/2, HTTP/3, keep-alive | | ❌ | `Connection: close`, read to EOF |
| IPv6 | | ❌ | Not started |
| VPN | | ❌ | Not started |
| Proxy configuration | | ❌ | Not started |
| Captive portal detection | | ❌ | Not started |
| Reachability / network path monitor | | ❌ | The andemu `ConnectivityManager` synthesises connectivity from `sys.wifi` and `sys.airplane` — it reports "connected" from two toggles, not from whether a packet can leave |
| AirPlay | | ❌ | Root-causes 4 and 5 |
| CarPlay | | ❌ | Root-cause 5 |
| Satellite SOS / Messages | | ❌ | Root-cause 5 |

## 16. Media

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Photo library browsing | | ✅ | Grid of the shared library, re-listed when the app becomes visible. `system/apps/photos/main.c` |
| Full-screen photo viewer | | ✅ | Swipe to the next photo |
| Delete a photo | | ✅ | With a confirmation step |
| Share a photo | | ✅ | Into the real share sheet |
| Set as wallpaper | | ✅ | |
| Albums / collections | | ❌ | One flat directory |
| Favourites, keywords, star ratings | 27 for ratings | ❌ | Not started |
| People / faces / Memories | | ❌ | No model |
| Photo editing (crop, adjust, filters) | | ❌ | Not started |
| Clean Up / Extend / Reframe | 26–27 | ❌ | No model |
| Slideshows from any album | 27 | ❌ | Not started |
| Video capture & playback | | ❌ | Root-cause 4 |
| Screenshot | | ✅ | `Print` chord → screencopy → normalise ARGB/ABGR and y-invert → shared library → a notification posted **directly over the socket** under the Photos identity, carrying the thumbnail. `system/shot/main.c` |
| Screenshot preview & markup | | ❌ | Saved straight to the library |
| Full-page / scrolling capture | | ❌ | Not started |
| Screen recording | | ❌ | Not started |
| Audio playback | | ❌ | Root-cause 4 |
| Now Playing / lock-screen media controls | | ❌ | Root-cause 4 |
| Media keys | | 🟡 | The compositor binds `XF86AudioRaise/Lower/Mute` — they move `sys.volume` and raise the HUD, and nothing plays |
| Spatial audio / AirPods features | | ❌ | Root-cause 4 |
| Ringtones & alert sounds | | ❌ | Root-cause 4 |
| Camera app | | 🟡 | Live preview, shutter writes a still into the shared library, frame and shot counters. Consent is **chained** — the stream opens from the grant reply rather than beside it, so there is no window where a preview runs un-granted |

## 17. First-party app suite

| iOS app | iOS | Zelto | Where / gap |
|---|---|---|---|
| Settings | | ✅ | Root is a short list of doors, each carrying its subject's state in the detail column; six panes (Network, Display & Sound, Wallpaper, Lock Screen, Keyboard, Accessibility). Real 51×31 pt switches, split `[−\|+]` steppers, hairlines inset to the text column, and a full accessibility reflow. `system/apps/settings/main.c` |
| Photos | | 🟡 | See §16 |
| Camera | | 🟡 | See §16 |
| Phone | | ❌ | Root-cause 5 |
| Messages | | ❌ | Root-cause 5 |
| Mail | | ❌ | Not started |
| Safari | | ❌ | No web engine |
| Music | | ❌ | Root-cause 4 |
| Maps | | ❌ | Location plumbing only |
| Calendar | | ❌ | Not started |
| Clock / alarms / timers / stopwatch | | ❌ | Only SDK `z_after` timers and a clock **widget** exist |
| Weather | | ❌ | Not started |
| Notes | | 🟡 | Two apps split the job: `zelto-notepad` (SQLite notes + prefs counter, survives reboot) and `zelto-notes` (a share/deep-link **target**) |
| Reminders | | ❌ | Not started |
| Files | | ❌ | Not started |
| Health | | ❌ | Not started |
| Wallet | | ❌ | Not started |
| Home | | ❌ | Not started |
| App Store | | 🟡 | `zelto-store` — two buttons that exercise the install pipeline, one valid `.zap` and one tampered |
| FaceTime | | ❌ | Root-causes 4 and 5 |
| Contacts | | ❌ | `docs/system-apis/contacts.md` documents an API with zero implementation |
| Shortcuts | | ❌ | Not started |
| Journal | | ❌ | Not started |
| Passwords | | ❌ | No keychain |
| Freeform / Books / Podcasts / Fitness / Compass / Voice Memos / Calculator / Tips | | ❌ | Not started |
| App Switcher (`zelto-recents`) | | ✅ | See §2 |
| Share sheet (`zelto-chooser`) | | ✅ | See §12 |
| Consent dialog (`zelto-consent`) | | ✅ | See §11 |
| Screenshot service (`zelto-shot`) | | ✅ | See §16 |
| Sensors / Fetch / Cards / Pinger / Share / JS Demo / andemu Demo | | ✅ | Demo apps that exist to exercise a subsystem end to end, each the smallest client that can prove one broker path works |

## 18. Apple Intelligence, Siri & AI

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Siri (voice assistant) | | ❌ | Root-cause 4 |
| Siri AI conversational overhaul & standalone app | 27β | ❌ | No model |
| Personal context (mail, messages, photos, files) | 27β | ❌ | No model, and no isolation to make it safe if there were |
| Onscreen awareness | 27β | ❌ | Not started |
| App actions via assistant | 27β | ❌ | No action vocabulary (see App Intents, §12) |
| Search / Ask replacing Spotlight | 27β | ❌ | No search index at all |
| Writing Tools | 26 | ❌ | No model |
| Image Playground / Genmoji | 26–27 | ❌ | No model |
| Visual Intelligence / "Siri Mode" camera | 26–27 | ❌ | Synthetic camera, no model |
| Clean Up / Extend / Reframe in Photos | 26–27 | ❌ | No model |
| Notification & mail summaries | 26 | ❌ | No model |
| Live Translation | 26 | ❌ | No model |
| Auto-generated captions | 27β | ❌ | Root-cause 4 |
| Natural-language calendar & reminder entry | 27β | ❌ | Those apps do not exist |
| Shortcut generation from a description | 27β | ❌ | No Shortcuts |
| Safari tab organisation / prompt-built extensions | 27β | ❌ | No browser |
| On-device model + Private Cloud Compute | 26 | ❌ | No inference runtime of any kind |
| Foundation Models framework for third-party apps | 26 | ❌ | Not started |
| Predictive text (statistical, on-device) | | ✅+ | Not "AI" in Apple's sense, but it *is* the same job done honestly: a prefix trie backed off to a letter-pair table, sharper with a longer prefix — P('p'\|"hel") is 0.67 against 0.007 for the bare bigram — feeding one argmax that also decides whether to grow the space bar |

## 19. Boot, updates & device management

| iOS feature | iOS | Zelto | Where / gap |
|---|---|---|---|
| Boot to a shell | | ✅ | BusyBox static arm64 + `/init` + `zcomp` in an initramfs, GLES2 with a pixman fallback and a rescue shell. `meta/initramfs/init` |
| Service manager | | 🟡 | init is a shell script with `&` and literal `sleep`s; ordering is by measured delay, not dependency |
| Setup Assistant / first-run onboarding | | ❌ | Boot goes straight to the launcher |
| Language & region selection | | ❌ | Not started |
| Account sign-in | | ❌ | No accounts |
| OTA software update | | ❌ | No A/B slots, no update client, no delta, no rollback |
| Rapid Security Response | | ❌ | Not started |
| Recovery / DFU mode | | ❌ | Not started |
| Factory reset / Erase All Content | | ❌ | Not started |
| Device migration / restore from backup | | ❌ | Not started |
| Secure / verified boot | | ❌ | See §11 |
| About / storage / software-version pane | | ❌ | Settings has six panes, none of them General |
| MDM / supervision | | ❌ | Not started |

---

## Reading this honestly

**What is strong.** The parts of a phone OS that are *hard to get right and easy to
fake* are the parts that are real here: the permission broker with its
declaration-before-prompt gate and broker-owned keys, the intent router, the signed
package pipeline, the idle/lock state machine, the compositor-side backdrop blur and
window-snapshot protocols, and the accessibility work in §9–10 — where Dynamic Type,
the reflow threshold, the contrast tokens and the line-height ratio were each
**measured** rather than asserted, which is more than the iOS documentation
publishes about its own.

**What is missing is mostly structural, not incremental.** The six root causes above
account for the large majority of the ❌ rows. Adding `wl_touch` unblocks a whole
gesture family at once; adding semantics to `ZNode` unblocks the entire assistive-
technology column; process isolation is the precondition for almost everything in
§11. None of these is a feature to schedule — each is a foundation to lay before the
features above it can be honest.

**The synthetic-hardware seams are marked, not hidden.** `camera_source_fill()`,
the sensor and location synthesisers and `read_sysfs_battery()` are each one
function a device port replaces. That is the right shape. But it means the hardware
column of this table is a *port* away, not a *feature* away — and until that port
exists, no row in §14 or §15 should be read as working on a real phone.

---

## Sources

iOS baseline researched 25 July 2026:

- [Apple — iOS](https://www.apple.com/os/ios/)
- [Apple — Accessibility](https://www.apple.com/accessibility/)
- [Apple Newsroom — accessibility features preview](https://www.apple.com/newsroom/2025/05/apple-unveils-powerful-accessibility-features-coming-later-this-year/)
- [MacRumors — iOS 27 roundup](https://www.macrumors.com/roundup/ios-27/)
- [MacRumors — 20 New Things Your iPhone Can Do in iOS 27](https://www.macrumors.com/2026/07/20/new-things-iphone-can-do-ios-27/)
- [MacRumors — iOS 27 public beta features](https://www.macrumors.com/guide/ios-27-public-beta-features/)
- [Tom's Guide — iOS 27 announced at WWDC 2026](https://www.tomsguide.com/phones/iphones/ios-27-is-official-all-the-new-upgrades-and-features-announced-at-wwdc-2026)
- [TechRadar — 21 iOS 27 features not mentioned in the keynote](https://www.techradar.com/phones/ios/here-are-21-new-features-in-ios-27-that-apple-didnt-have-time-to-mention-during-its-wwdc-2026-keynote)
- [AppleInsider — Apple Intelligence accessibility features in OS 27](https://appleinsider.com/articles/26/05/19/apple-intelligence-powered-accessibility-features-in-os-27-detailed-ahead-of-wwdc)
- [BGR — 8 new iOS 27 accessibility features](https://www.bgr.com/2177707/cool-new-ios-27-accessibility-features-look-forward-to-fall/)

## Keeping this current

Two failure modes, both worth guarding:

1. **iOS moves.** Re-run the research against each autumn release and re-mark the
   `27β` rows as shipped or dropped.
2. **The citations rot.** Every non-❌ row names a file. When a file moves, this
   document lies quietly — which is the exact failure this repo has been bitten by
   before. Check the paths, not the prose.
