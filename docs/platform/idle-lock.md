# Idle & Lock

Zelto has a system idle/lock lifecycle: after a period of inactivity the screen
dims, then a lock screen takes over and blocks the running app, then the screen
goes "off". Any input wakes it; a swipe (or a passcode) unlocks it back to exactly
the app that was in front.

## States

```
   input activity
        │  (resets to Active)
        ▼
   ┌────────┐  dim_s   ┌────────┐  lock_s   ┌────────┐  off_s   ┌──────┐
   │ Active │─────────►│ Dimmed │──────────►│ Locked │─────────►│ Off  │
   └────────┘          └────────┘           └────────┘          └──────┘
        ▲                   │                   ▲   ▲               │
        └───────────────────┘  input           │   └───────────────┘  input
             (unlocked wake)                    │      (wake, stays locked)
                                    unlock (swipe / passcode)
```

| State | What the user sees | Blocks the app? |
|---|---|---|
| **Active** | Nothing — the lock surface is invisible. | No |
| **Dimmed** | A translucent scrim; the app is still faintly visible. | No (a tap wakes and reaches the app) |
| **Locked** | An opaque screen with a clock + date and a swipe/passcode. | **Yes** — whole-surface pointer + EXCLUSIVE keyboard |
| **Off** | Full black ("screen off"). | **Yes** |

All timeouts are measured from the last input on the seat. Once **Locked**, input
no longer returns to Active — it only wakes the lock screen from Off; the session
stays secure until the user unlocks.

## How it works

- **Activity** comes from the compositor via the standard **ext-idle-notify-v1**
  protocol: zcomp calls `wlr_idle_notifier_v1_notify_activity` on every pointer /
  keyboard event. It holds *no* idle/lock policy.
- **Policy** lives entirely in **zelto-lock**, an always-running OVERLAY layer app
  (above the shade). It registers idle notifications (`z_idle_notify`) at the dim
  and lock thresholds and drives the state machine; while locked it registers one
  more at the off threshold. On lock it grabs the keyboard
  (`z_layer_set_keyboard(app, true)`) and takes the whole input region; on unlock
  it drops both, and the compositor returns the keyboard to the front app.
- **Focus is preserved for free**: the lock is a *layer* surface, never a
  toplevel, so the foreground app is never backgrounded or closed — it is exactly
  the front window again the moment the lock dismisses.
- **"Screen off" is an opaque OVERLAY scrim, not DPMS.** A real display blank
  would make a screendump black; an opaque scrim keeps the lock UI and off state
  visible to the (headless) test harness. Deliberate, documented tradeoff.

## Settings (broker keys)

All configured through the zsysd settings broker (`sys.*`), persisted across
reboot, editable in the Settings app (Lock screen section). The whole lifecycle is
gated by `sys.lock_enabled` — when off (the default) the machine is inert.

| Key | Meaning | Default |
|---|---|---|
| `sys.lock_enabled` | Master switch for the idle/lock lifecycle. | `0` |
| `sys.idle_dim_s` | Seconds of inactivity before dimming. | `8` |
| `sys.idle_lock_s` | Seconds before locking. | `20` |
| `sys.idle_off_s` | Seconds before the screen goes off. | `120` |
| `sys.passcode` | 4-digit passcode; empty = swipe-only. | `""` |
| `sys.lock_now` | A counter; bumping it locks immediately (a shade/bar "lock now"). | `0` |

When `sys.lock_enabled` is on, the status bar shows a padlock glyph. A long-press
on the status bar (or the Settings "Lock now" button) locks on demand.

## SDK

```c
// Register an idle notification: on_idled fires after timeout_ms of no activity,
// on_resumed on the next activity. Cancel to re-arm with a new timeout.
ZIdle *z_idle_notify(ZApp *app, int timeout_ms, ZIdleCb on_idled,
                     ZIdleCb on_resumed, void *ud);
void   z_idle_cancel(ZIdle *idle);

// Grab / release EXCLUSIVE keyboard on a layer surface at runtime (a modal that
// is only sometimes modal — the lock screen).
void   z_layer_set_keyboard(ZApp *app, bool exclusive);
```
