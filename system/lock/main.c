// Zelto System UI — idle/lock lifecycle (P20).
//
// The phone's most fundamental state machine: idle -> dim -> lock -> (screen off)
// -> wake -> unlock. zelto-lock is an always-mapped OVERLAY layer-shell surface
// living ABOVE the shade, driven by ext-idle-notify (the compositor reports seat
// activity; we register per-threshold idle notifications) and configured through
// the zsysd settings broker (sys.lock_* keys). It owns ALL the policy — the
// compositor stays free of idle/lock logic and only pumps activity.
//
// STATES (all timeouts measured from the last input on the seat):
//   ACTIVE  — invisible, input-transparent (paints nothing, steals no taps).
//   DIMMED  — after sys.idle_dim_s: a translucent black scrim (the P19 dim path:
//             source-over alpha + z_full_repaint). Still input-transparent, so a
//             tap both wakes AND reaches the app (mild, phone-like).
//   LOCKED  — after sys.idle_lock_s: an OPAQUE security gate over everything. It
//             truly blocks the app beneath — whole-surface input region (pointer)
//             + EXCLUSIVE keyboard (z_layer_set_keyboard). Shows a clock + date
//             and a swipe-up-to-unlock affordance (a 4-digit passcode keypad when
//             sys.passcode is set). Unlock dismisses it and the previously-
//             foreground app is exactly restored (see FOCUS below).
//   OFF     — after sys.idle_off_s: a full-black scrim ("screen off"). Any input
//             wakes it back to LOCKED (if it was a locked session) or ACTIVE.
//
// FOCUS RESTORATION (the real risk, handled by construction). zelto-lock is a
// LAYER surface, never a toplevel, so it never enters the compositor's toplevel
// MRU list — the foreground app never gets backgrounded/closed under the lock and
// is exactly the front window again the instant we dismiss. The keyboard half:
// we grab EXCLUSIVE keyboard while locked and drop it on unlock; the compositor's
// layer_sync_keyboard / layer_release_keyboard (layer.c) then returns the
// keyboard to that same front toplevel. (The P8 consent modal proved this path.)
//
// "SCREEN OFF" = a full-black OVERLAY scrim, NOT real DPMS / wlr_output blank.
// The tradeoff is deliberate: a real blank would make the QMP screendump black so
// the harness could not verify the lock UI or the off state. An opaque scrim is
// visible to the screendump and cheap (reuses the P19 renderer). Documented.
//
// GATING. sys.lock_enabled gates the WHOLE lifecycle: when 0 (the default),
// zelto-lock arms no idle notifications and stays inert (invisible, no dim, no
// lock) — so the other phase harnesses, which idle for long stretches, are never
// disturbed. The LOCK harness flips it on. When on, the machine runs.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zelto/ui.h>

#include "common/wallpaper.h"

// Dim scrim alpha (DIMMED). Translucent black over the app — clearly darker but
// the app stays visible, like a phone's pre-lock dim. Premultiplied source-over.
#define DIM_ALPHA 120
// Swipe distance (px, upward) on the lock screen that dismisses it (or, with a
// passcode set, reveals the keypad).
#define UNLOCK_SWIPE 150.0f
#define MAX_PASS 15

typedef enum LockPhase {
    ST_ACTIVE = 0,
    ST_DIMMED,
    ST_LOCKED,
    ST_OFF,
} LockPhase;

typedef struct LockState {
    bool inited;
    bool subscribed;

    // Config, from the broker (sys.* keys).
    bool enabled;
    int64_t dim_s, lock_s, off_s;
    char passcode[MAX_PASS + 1];
    int64_t lock_now_seen;   // last sys.lock_now counter observed (manual lock)

    // State machine.
    LockPhase phase;
    bool locked;             // a secure session (LOCKED or OFF-after-lock)

    // Idle notifications. Unlocked regime: dim + lock. Locked regime: one off
    // timeout (blank the lock screen after inactivity, wake on input).
    ZIdle *n_dim, *n_lock, *n_lockoff;

    // Passcode entry (only when sys.passcode is set).
    bool entering;           // swipe done -> keypad shown
    char entry[8];
    int entry_len;
    bool wrong;              // last attempt mismatched

    // P25 wallpaper: same sys.wallpaper the home screen uses, shown behind the
    // lock UI under a heavier darkening scrim (legibility for the clock/keypad).
    // wp_ok records whether it decodes (else an opaque BG fallback).
    bool wp_ok;
    char wp_path[ZELTO_WALLPAPER_PATH_MAX];
} LockState;

// --- idle notification (dis)arming -----------------------------------------
static void on_dim_idled(ZApp *app, void *ud);
static void on_lock_idled(ZApp *app, void *ud);
static void on_unlocked_resume(ZApp *app, void *ud);
static void on_lockoff_idled(ZApp *app, void *ud);
static void on_lockoff_resume(ZApp *app, void *ud);

static void disarm_unlocked(LockState *s) {
    z_idle_cancel(s->n_dim);
    z_idle_cancel(s->n_lock);
    s->n_dim = s->n_lock = NULL;
}
static void disarm_locked(LockState *s) {
    z_idle_cancel(s->n_lockoff);
    s->n_lockoff = NULL;
}
static void disarm_all(LockState *s) {
    disarm_unlocked(s);
    disarm_locked(s);
}

// Arm the unlocked-regime timeouts: dim then lock. (There is no unlocked "off":
// with lock_enabled the machine always locks at lock_s < off_s; OFF only follows
// a lock, handled by the locked regime below.)
static void arm_unlocked(ZApp *app, LockState *s) {
    disarm_unlocked(s);
    int64_t dim = s->dim_s > 0 ? s->dim_s : 1;
    int64_t lock = s->lock_s > dim ? s->lock_s : dim + 1;
    s->n_dim = z_idle_notify(app, (int)(dim * 1000), on_dim_idled,
                             on_unlocked_resume, s);
    s->n_lock = z_idle_notify(app, (int)(lock * 1000), on_lock_idled, NULL, s);
}

// Arm the locked-regime off timeout (blank the lock screen after off_s of no
// input; wake on input). off_s is measured from the last activity on the seat,
// so creating it after the lock still fires at off_s total.
static void arm_locked(ZApp *app, LockState *s) {
    disarm_locked(s);
    int64_t off = s->off_s > s->lock_s ? s->off_s : s->lock_s + 1;
    s->n_lockoff = z_idle_notify(app, (int)(off * 1000), on_lockoff_idled,
                                 on_lockoff_resume, s);
}

// --- transitions ------------------------------------------------------------
static void enter_locked(ZApp *app, LockState *s) {
    disarm_unlocked(s);
    s->locked = true;
    s->phase = ST_LOCKED;
    s->entering = false;
    s->entry_len = 0;
    s->wrong = false;
    arm_locked(app, s);
    z_full_repaint(app);
    z_invalidate(app);
}

static void do_unlock(ZApp *app, LockState *s) {
    disarm_all(s);
    s->locked = false;
    s->phase = ST_ACTIVE;
    s->entering = false;
    s->entry_len = 0;
    s->wrong = false;
    // Hand the keyboard back to the front app (compositor releases the grab on
    // the interactivity-drop commit z_layer_set_keyboard(false) makes in body).
    if (s->enabled) {
        arm_unlocked(app, s);   // re-arm timeouts (input just happened)
    }
    z_full_repaint(app);
    z_invalidate(app);
}

// --- idle callbacks ---------------------------------------------------------
static void on_dim_idled(ZApp *app, void *ud) {
    LockState *s = ud;
    if (!s->enabled || s->locked) {
        return;
    }
    if (s->phase == ST_ACTIVE) {
        s->phase = ST_DIMMED;
        z_full_repaint(app);
        z_invalidate(app);
    }
}
static void on_lock_idled(ZApp *app, void *ud) {
    LockState *s = ud;
    if (!s->enabled || s->locked) {
        return;
    }
    enter_locked(app, s);
}
static void on_unlocked_resume(ZApp *app, void *ud) {
    LockState *s = ud;
    if (s->locked || s->phase == ST_ACTIVE) {
        return;
    }
    s->phase = ST_ACTIVE;
    z_full_repaint(app);
    z_invalidate(app);
}
static void on_lockoff_idled(ZApp *app, void *ud) {
    LockState *s = ud;
    if (!s->locked || s->phase == ST_OFF) {
        return;
    }
    s->phase = ST_OFF;
    z_full_repaint(app);
    z_invalidate(app);
}
static void on_lockoff_resume(ZApp *app, void *ud) {
    LockState *s = ud;
    if (!s->locked || s->phase == ST_LOCKED) {
        return;
    }
    // Wake the OFF lock screen back to the visible lock UI (still locked).
    s->phase = ST_LOCKED;
    s->entering = false;
    s->entry_len = 0;
    z_full_repaint(app);
    z_invalidate(app);
}

// --- broker config ----------------------------------------------------------
static void load_config(LockState *s) {
    s->enabled = z_setting_get_int("sys.lock_enabled", 0) != 0;
    s->dim_s = z_setting_get_int("sys.idle_dim_s", 8);
    s->lock_s = z_setting_get_int("sys.idle_lock_s", 20);
    s->off_s = z_setting_get_int("sys.idle_off_s", 120);
    s->lock_now_seen = z_setting_get_int("sys.lock_now", 0);
    snprintf(s->passcode, sizeof(s->passcode), "%s",
             z_setting_get_str("sys.passcode", ""));
    s->wp_ok = zelto_wallpaper_active(s->wp_path, sizeof(s->wp_path));
    fprintf(stderr,
            "zelto-lock: config enabled=%d dim=%llds lock=%llds off=%llds "
            "passcode=%s\n",
            (int)s->enabled, (long long)s->dim_s, (long long)s->lock_s,
            (long long)s->off_s, s->passcode[0] ? "set" : "none");
}

static void on_changed(ZApp *app, const char *key, const char *value, void *ud) {
    LockState *s = ud;
    if (strcmp(key, "sys.lock_enabled") == 0) {
        bool en = atoi(value) != 0;
        if (en == s->enabled) {
            return;
        }
        s->enabled = en;
        if (en) {
            s->phase = ST_ACTIVE;
            s->locked = false;
            arm_unlocked(app, s);
        } else {
            // Turn the whole lifecycle off: disarm, unlock, go inert.
            disarm_all(s);
            s->locked = false;
            s->phase = ST_ACTIVE;
        }
        z_full_repaint(app);
        z_invalidate(app);
    } else if (strcmp(key, "sys.idle_dim_s") == 0) {
        s->dim_s = atoll(value);
        if (s->enabled && !s->locked) {
            arm_unlocked(app, s);
        }
    } else if (strcmp(key, "sys.idle_lock_s") == 0) {
        s->lock_s = atoll(value);
        if (s->enabled && !s->locked) {
            arm_unlocked(app, s);
        }
    } else if (strcmp(key, "sys.idle_off_s") == 0) {
        s->off_s = atoll(value);
        if (s->locked) {
            arm_locked(app, s);
        }
    } else if (strcmp(key, "sys.passcode") == 0) {
        snprintf(s->passcode, sizeof(s->passcode), "%s", value ? value : "");
    } else if (strcmp(key, "sys.lock_now") == 0) {
        int64_t v = atoll(value);
        if (v != s->lock_now_seen) {
            s->lock_now_seen = v;
            if (s->enabled && !s->locked) {
                enter_locked(app, s);   // manual "lock now"
            }
        }
    } else if (strcmp(key, ZELTO_WALLPAPER_KEY) == 0) {
        // A pick in Settings: re-resolve so a locked screen shows it live.
        s->wp_ok = zelto_wallpaper_active(s->wp_path, sizeof(s->wp_path));
        z_full_repaint(app);
        z_invalidate(app);
    }
}

// --- lock-screen UI ---------------------------------------------------------
// Swipe up on the lock surface: dismiss (or reveal the passcode keypad).
static void on_lock_pan(ZApp *app, void *state, const ZPanEvent *e) {
    LockState *s = state;
    if (e->phase != Z_PAN_END || !s->locked || s->entering) {
        return;
    }
    if (e->translation_y < -UNLOCK_SWIPE) {
        if (s->passcode[0]) {
            s->entering = true;   // reveal the keypad; the code must still match
            s->entry_len = 0;
            s->wrong = false;
            z_full_repaint(app);
            z_invalidate(app);
        } else {
            do_unlock(app, s);
        }
    }
}

static void absorb(ZApp *app, void *state) {
    (void)app;
    (void)state;
}

// A keypad digit tap: append it and, at 4 digits, compare against sys.passcode.
static void on_digit(ZApp *app, void *state, void *data) {
    LockState *s = state;
    int d = (int)(intptr_t)data;
    if (s->entry_len < 4) {
        s->entry[s->entry_len++] = (char)('0' + d);
        s->entry[s->entry_len] = '\0';
        s->wrong = false;
    }
    if (s->entry_len == 4) {
        if (strcmp(s->entry, s->passcode) == 0) {
            do_unlock(app, s);
            return;
        }
        s->wrong = true;
        s->entry_len = 0;
        s->entry[0] = '\0';
    }
    z_invalidate(app);
}

static ZView key_digit(int d) {
    return Grow(1.0f,
        OnTapData(on_digit, (void *)(intptr_t)d,
            Background(Z_COLOR_SURFACE_2,
                CornerRadius(16.0f,
                    Frame(96.0f, 72.0f,
                        Foreground(Z_COLOR_TEXT_INV,
                            Font(Z_FONT_TITLE, Text("%d", d))))))));
}

static ZView keypad(LockState *s) {
    char dots[8] = "____";
    for (int i = 0; i < s->entry_len && i < 4; i++) {
        dots[i] = '*';
    }
    return VStack(
        Foreground(s->wrong ? Z_COLOR_DANGER
                            : Z_COLOR_TEXT,
            Font(Z_FONT_TITLE, Text(s->wrong ? "Wrong code" : "Enter passcode"))),
        Foreground(Z_COLOR_TEXT_INV, Font(Z_FONT_LARGE_TITLE, Text("%s", dots))),
        HStack(key_digit(1), key_digit(2), key_digit(3),
               .spacing = 12, .align = Z_ALIGN_CENTER),
        HStack(key_digit(4), key_digit(5), key_digit(6),
               .spacing = 12, .align = Z_ALIGN_CENTER),
        HStack(key_digit(7), key_digit(8), key_digit(9),
               .spacing = 12, .align = Z_ALIGN_CENTER),
        HStack(key_digit(0), .spacing = 12, .align = Z_ALIGN_CENTER),
        .spacing = 16, .align = Z_ALIGN_CENTER);
}

static ZView lock_screen(ZApp *app, LockState *s) {
    (void)app;
    char clock[16] = "--:--";
    char date[64] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(clock, sizeof(clock), "%H:%M", &tmv);
        strftime(date, sizeof(date), "%a %d %b", &tmv);
    }

    ZView content;
    if (s->entering) {
        content = keypad(s);
    } else {
        content = VStack(
            Foreground(Z_COLOR_TEXT_INV,
                Font(Z_FONT_LARGE_TITLE, Text("%s", clock))),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_TITLE, Text("%s", date))),
            Spacer(),
            Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_CALLOUT,
                    Text(s->passcode[0] ? "Swipe up for passcode"
                                        : "Swipe up to unlock"))),
            Rect(.color = Z_COLOR_TEXT_MUTED,
                 .width = 64, .height = 5, .radius = 3),
            .spacing = 16, .align = Z_ALIGN_CENTER);
    }

    // Backdrop: the shared wallpaper (cover-fit) under a heavy darkening scrim for
    // legibility, else an opaque BG fallback. z_full_repaint (set by body for the
    // LOCKED phase) makes the scrim's alpha blend correctly over the photo.
    ZView inner = Fill(ZStack(content, .align = Z_ALIGN_CENTER, .padding = 40));
    ZView backdrop = s->wp_ok
        ? Fill(ZStack(
              Fill(Cover(Image(s->wp_path))),
              Fill(Background(z_scrim(170), Fill(Spacer()))),
              inner,
              .align = Z_ALIGN_CENTER))
        : Background(Z_COLOR_BG, inner);

    // The whole surface takes the swipe (OnPan); a tap on the backdrop is absorbed
    // so nothing falls through. The keypad's digit buttons are OnTap targets nested
    // inside — tap vs. swipe is decided by slop.
    return OnPan(on_lock_pan, OnTap(absorb, backdrop));
}

// --- body -------------------------------------------------------------------
static ZView lock_body(ZApp *app, LockState *s) {
    if (!s->inited) {
        s->inited = true;
        load_config(s);
    }
    // Subscribe once (ctrl_fd is up by the first build, like the shade); arm the
    // lifecycle if it is already enabled at boot.
    if (!s->subscribed) {
        s->subscribed = true;
        z_settings_observe(app, on_changed, s);
        if (s->enabled) {
            arm_unlocked(app, s);
        }
    }

    switch (s->phase) {
    case ST_DIMMED:
        z_layer_set_keyboard(app, false);
        z_layer_set_input_none(app);   // mild dim: taps fall through + wake
        z_full_repaint(app);
        return Background(z_scrim(DIM_ALPHA), Fill(Spacer()));
    case ST_LOCKED:
        z_layer_set_keyboard(app, true);          // modal keyboard grab
        z_layer_set_input_region(app, 0, 0, 0, 0);// whole surface (block the app)
        z_full_repaint(app);
        return lock_screen(app, s);
    case ST_OFF:
        z_layer_set_keyboard(app, s->locked);
        z_layer_set_input_region(app, 0, 0, 0, 0);// whole surface: "screen off"
        z_full_repaint(app);
        return OnTap(absorb,
            Background(z_scrim(0xff), Fill(Spacer())));
    case ST_ACTIVE:
    default:
        z_layer_set_keyboard(app, false);
        z_layer_set_input_none(app);   // invisible + input-transparent
        return Fill(Spacer());
    }
}

// OVERLAY, above the shade (started last in init so it is the topmost overlay),
// anchored to all four edges over the WHOLE screen (margin_top 0 — the lock
// covers the status bar too, unlike the dim/shade). keyboard=false at startup;
// z_layer_set_keyboard grabs it only while locked.
Z_LAYER_APP(LockState, lock_body,
            .layer = Z_LAYER_OVERLAY,
            .anchor = Z_ANCHOR_TOP | Z_ANCHOR_BOTTOM | Z_ANCHOR_LEFT |
                      Z_ANCHOR_RIGHT,
            .exclusive_zone = 0,
            .keyboard = false)
