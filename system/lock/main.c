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

#include "common/glyphs.h"
#include "common/notif_card.h"
#include "common/wallpaper.h"

// Dim scrim alpha (DIMMED). Translucent black over the app — clearly darker but
// the app stays visible, like a phone's pre-lock dim. Premultiplied source-over.
#define DIM_ALPHA 120
// Swipe distance (px, upward) from the home indicator that unlocks (or, with a
// passcode set, reveals the keypad), and the release velocity that does the same
// on a shorter flick — the same velocity-vs-distance split system/homebar uses
// for the home gesture, because it is the same gesture from the same place.
#define UNLOCK_SWIPE 150.0f
#define UNLOCK_FLICK (-900.0f)
// How far up from the bottom edge a drag must START to be the unlock gesture. A
// swipe anywhere used to unlock, which was fine when the screen held nothing but
// a clock; now there are notification cards to swipe on, and a gesture that
// unlocks from anywhere would eat every one of those. The zone is the home
// indicator and its reach.
#define UNLOCK_ZONE 200.0f
// The lock plate lifts 1:1 with the finger and fades as it goes, over this much
// travel — the drag is the animation, so the unlock is never a jump cut.
#define UNLOCK_FADE 320.0f
#define MAX_PASS 15

// Lock-screen layout. The clock sits HIGH (iOS puts it just under the status bar,
// not centred) because everything below it — notifications, and eventually a
// widget row — is content that grows downward from it.
#define LOCK_TOP 96.0f      // from the top edge to the padlock
#define LOCK_SIDE ((float)Z_SPACE_L)   // side inset for the whole plate
#define LOCK_MAX_CARDS 4    // cards shown before the list is summarised

// A fixed gap: NOT Frame(w, h, Spacer()), which keeps its grow flag and eats the
// stack's spare space.
static ZView lgap(float h) {
    return Frame(1.0f, h, Rect(.color = z_rgba(0, 0, 0, 0)));
}

typedef enum LockPhase {
    ST_ACTIVE = 0,
    ST_DIMMED,
    ST_LOCKED,
    ST_OFF,
} LockPhase;

// One notification held for display. The pushed ZShownNotification strings are
// valid only during the callback, so copy them.
#define LOCK_MAX_NOTIFS 8
typedef struct LockNotif {
    bool used;
    int64_t id;
    char app_id[96];
    char title[128];
    char body[192];
} LockNotif;

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

    // P41: notifications on the lock screen. We are a SECOND zsysd notification
    // sink alongside the shade (the sink is a set, not last-wins — see the store
    // notes in system/zsysd/main.c), so a posted notification lands here as a card
    // and a drop removes it from both surfaces at once. Nothing is stored across
    // a boot: the store is in-memory, so a lock screen shows what has arrived.
    bool subscribed_notifs;
    LockNotif notifs[LOCK_MAX_NOTIFS];

    // The unlock drag. `lift` is the plate's live vertical translation (<=0 as it
    // rises); the whole plate fades out over UNLOCK_FADE of travel, so the drag IS
    // the transition. `unlocking` latches at the drag's begin from whether the
    // finger started in the home-indicator zone.
    ZAnimated *lift;
    bool unlocking;
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

// --- notification sink ------------------------------------------------------
// zsysd pushed a notification: store it (replacing one with the same id) so it
// appears as a card. We are one of several sinks — the shade gets the same push.
static void on_notif_show(ZApp *app, const ZShownNotification *n, void *ud) {
    LockState *s = ud;
    LockNotif *slot = NULL;
    for (int i = 0; i < LOCK_MAX_NOTIFS; i++) {
        if (s->notifs[i].used && s->notifs[i].id == n->id) {
            slot = &s->notifs[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < LOCK_MAX_NOTIFS; i++) {
            if (!s->notifs[i].used) {
                slot = &s->notifs[i];
                break;
            }
        }
    }
    if (!slot) {
        return;   // full: the newest is dropped, not the oldest (no churn)
    }
    slot->used = true;
    slot->id = n->id;
    snprintf(slot->app_id, sizeof(slot->app_id), "%s", n->app_id ? n->app_id : "");
    snprintf(slot->title, sizeof(slot->title), "%s", n->title ? n->title : "");
    snprintf(slot->body, sizeof(slot->body), "%s", n->body ? n->body : "");
    // Only repaint if the card is actually on screen. While unlocked this surface
    // draws nothing at all, and a full-surface damage on an always-mapped
    // full-screen overlay for every notification the phone receives is a real cost
    // for no pixels. The store is still updated, so the next lock shows it.
    if (s->phase == ST_LOCKED) {
        z_full_repaint(app);
        z_invalidate(app);
    }
}

// zsysd dropped a notification (cancelled, or acted on somewhere else): take its
// card off the lock screen too.
static void on_notif_hide(ZApp *app, int64_t id, void *ud) {
    LockState *s = ud;
    for (int i = 0; i < LOCK_MAX_NOTIFS; i++) {
        if (s->notifs[i].used && s->notifs[i].id == id) {
            s->notifs[i].used = false;
            if (s->phase == ST_LOCKED) {   // see on_notif_show
                z_full_repaint(app);
                z_invalidate(app);
            }
            return;
        }
    }
}

static int notif_count(LockState *s) {
    int n = 0;
    for (int i = 0; i < LOCK_MAX_NOTIFS; i++) {
        if (s->notifs[i].used) {
            n++;
        }
    }
    return n;
}

// --- lock-screen UI ---------------------------------------------------------
// THE UNLOCK GESTURE. Swipe up FROM THE HOME INDICATOR — the same place, and the
// same velocity-vs-distance decision, as the home gesture in system/homebar. The
// plate tracks the finger 1:1 and fades as it rises, so what you are doing to the
// screen is visible the whole way up rather than only at the end; a short drag
// springs back. A drag that starts above the zone is not this gesture at all (it
// belongs to the notification list), so it is ignored rather than half-tracked.
static void on_lock_pan(ZApp *app, void *state, const ZPanEvent *e) {
    LockState *s = state;
    if (!s->locked || s->entering || !s->lift) {
        return;
    }
    if (e->phase == Z_PAN_BEGIN) {
        float h = (float)z_app_height(app);
        s->unlocking = h < 1.0f || e->y >= h - UNLOCK_ZONE;
        if (s->unlocking) {
            z_animated_grab(s->lift);
        }
        return;
    }
    if (!s->unlocking) {
        return;
    }
    if (e->phase == Z_PAN_CHANGED) {
        // Upward only: the lock screen has nothing below it to pull down to.
        z_animated_set(s->lift, e->translation_y < 0.0f ? e->translation_y : 0.0f);
        z_invalidate(app);
        return;
    }
    // Z_PAN_END: a flick or a long-enough drag resolves; anything else falls back.
    s->unlocking = false;
    bool go = e->translation_y < -UNLOCK_SWIPE || e->velocity_y < UNLOCK_FLICK;
    if (!go) {
        z_animated_spring_velocity(s->lift, 0.0f, Z_SPRING_STANDARD,
                                   e->velocity_y);
        z_invalidate(app);
        return;
    }
    z_animated_set(s->lift, 0.0f);   // the plate is leaving; reset for next time
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

// A passcode key: a ROUND translucent button, which is what a phone's passcode
// keypad has looked like for a decade. The circle is not decoration — it is the
// shape that says "one of a set of equivalent targets", where a rounded rectangle
// says "a row in a list".
#define KEY_D 92.0f
#define KEY_GAP 20.0f

static ZView key_digit(int d) {
    return OnTapData(on_digit, (void *)(intptr_t)d,
        Frame(KEY_D, KEY_D,
            CornerRadius(KEY_D * 0.5f,
                Background(Z_COLOR_MATERIAL_THICK,
                    ZStack(
                        Foreground(Z_COLOR_TEXT,
                            Font(Z_FONT_TITLE, Text("%d", d))),
                        .align = Z_ALIGN_CENTER)))));
}

// The 0 key's row is padded with FIXED-WIDTH empty cells, never Spacers or a
// Grow: a grow-weighted row redistributes by child count, so a row holding one
// key would centre it at a different width than the three-key rows above and the
// column would visibly shift.
static ZView key_blank(void) {
    return Frame(KEY_D, KEY_D, Rect(.color = z_rgba(0, 0, 0, 0)));
}

static ZView keypad(LockState *s) {
    // The entry indicator is four dots that FILL as you type, not underscores
    // turning into asterisks — a row of glyphs that change identity reads as text,
    // and this is a progress display.
    ZStackOpts dots = {.spacing = Z_SPACE_M, .align = Z_ALIGN_CENTER};
    for (int i = 0; i < 4; i++) {
        bool on = i < s->entry_len;
        dots.children[i] = Frame(16.0f, 16.0f,
            Rect(.color = on ? Z_COLOR_TEXT : z_rgba(0xf2, 0xf2, 0xf7, 0x4d),
                 .radius = 8.0f));
    }

    ZStackOpts rows = {.spacing = KEY_GAP, .align = Z_ALIGN_CENTER};
    int r = 0;
    rows.children[r++] = HStack(key_digit(1), key_digit(2), key_digit(3),
                                .spacing = KEY_GAP, .align = Z_ALIGN_CENTER);
    rows.children[r++] = HStack(key_digit(4), key_digit(5), key_digit(6),
                                .spacing = KEY_GAP, .align = Z_ALIGN_CENTER);
    rows.children[r++] = HStack(key_digit(7), key_digit(8), key_digit(9),
                                .spacing = KEY_GAP, .align = Z_ALIGN_CENTER);
    rows.children[r++] = HStack(key_blank(), key_digit(0), key_blank(),
                                .spacing = KEY_GAP, .align = Z_ALIGN_CENTER);

    return VStack(
        Foreground(s->wrong ? Z_COLOR_DANGER : Z_COLOR_TEXT,
            Weight(Z_WEIGHT_SEMIBOLD,
                Font(Z_FONT_CALLOUT,
                     Text(s->wrong ? "Wrong Passcode" : "Enter Passcode")))),
        lgap(10.0f),
        z_stack(Z_AXIS_HORIZONTAL, &dots),
        lgap(36.0f),
        z_stack(Z_AXIS_VERTICAL, &rows),
        .spacing = 0, .align = Z_ALIGN_CENTER);
}

// The clock block: date over a very large time, LEADING-aligned and high on the
// screen. Centring it and shrinking it to Large Title made the lock screen look
// like a splash screen; the time is the content, so it gets display size, and it
// sits where content starts rather than floating in the middle of the wallpaper.
static ZView lock_clock(void) {
    char clock[16] = "--:--";
    char date[64] = "";
    time_t t = time(NULL);
    struct tm tmv;
    if (localtime_r(&t, &tmv)) {
        strftime(clock, sizeof(clock), "%H:%M", &tmv);
        strftime(date, sizeof(date), "%A %d %B", &tmv);
    }
    return VStack(
        Weight(Z_WEIGHT_MEDIUM,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_BODY, Text("%s", date)))),
        Weight(Z_WEIGHT_BOLD,
            Foreground(Z_COLOR_TEXT, Font(Z_FONT_DISPLAY, Text("%s", clock)))),
        .spacing = Z_SPACE_2XS, .align = Z_ALIGN_CENTER);
}

// The stack of notification cards under the clock. They are the SAME card the
// heads-up banner and the Notification Center draw (system/common/notif_card.h),
// non-interactive here: acting on a notification from a locked screen would be a
// security hole, and iOS makes you unlock first too. Past LOCK_MAX_CARDS the tail
// is summarised rather than run off the bottom of the screen.
static ZView lock_notifs(ZApp *app, LockState *s) {
    int total = notif_count(s);
    if (total == 0) {
        return NULL;
    }
    // The cards are given an EXPLICIT width — the plate's inner width — rather
    // than being left to size to their text. A notification card that is as wide
    // as its longest line makes the stack a ragged column whose edge moves with
    // the content, and on a lock screen that column is the only structure there is.
    float cw = (float)z_app_width(app) - 2.0f * LOCK_SIDE;
    if (cw < 120.0f) {
        cw = 720.0f - 2.0f * LOCK_SIDE;   // before the first configure
    }
    ZStackOpts col = {.spacing = Z_SPACE_S, .align = Z_ALIGN_CENTER};
    int k = 0, shown = 0;
    for (int i = 0; i < LOCK_MAX_NOTIFS && shown < LOCK_MAX_CARDS; i++) {
        if (!s->notifs[i].used) {
            continue;
        }
        col.children[k++] = Frame(cw, 0.0f,
            zelto_notif_card(app, cw, s->notifs[i].app_id, s->notifs[i].title,
                             s->notifs[i].body, NULL, false));
        shown++;
    }
    if (total > shown) {
        col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
            Weight(Z_WEIGHT_MEDIUM,
                Font(Z_FONT_FOOTNOTE,
                     Text("%d more notification%s", total - shown,
                          total - shown == 1 ? "" : "s"))));
    }
    return z_stack(Z_AXIS_VERTICAL, &col);
}

static ZView lock_screen(ZApp *app, LockState *s) {
    ZView content;
    if (s->entering) {
        // Entering a passcode: the keypad IS the screen. The clock stays, small,
        // so the screen does not change identity under the finger.
        content = VStack(
            lgap(LOCK_TOP - 40.0f),
            lock_clock(),
            Spacer(),
            keypad(s),
            lgap(40.0f),
            .spacing = 0, .align = Z_ALIGN_CENTER);
    } else {
        ZStackOpts col = {.spacing = 0, .align = Z_ALIGN_CENTER};
        int k = 0;
        col.children[k++] = lgap(LOCK_TOP);
        // The padlock: the one mark that says the screen is SECURED rather than
        // merely asleep. Same glyph the status bar and Control Center use.
        col.children[k++] = zelto_glyph_lock(26.0f, Z_COLOR_TEXT);
        col.children[k++] = lgap(14.0f);
        col.children[k++] = lock_clock();
        ZView cards = lock_notifs(app, s);
        if (cards) {
            col.children[k++] = lgap(40.0f);
            col.children[k++] = cards;
        }
        col.children[k++] = Spacer();
        // The home indicator, and only the home indicator. The old screen printed
        // "Swipe up to unlock" above it — a label explaining the pill it is drawn
        // next to. The pill is the affordance; the sentence is what you write when
        // you do not trust it. The passcode case keeps ONE word of warning,
        // because there the swipe does not finish the job.
        if (s->passcode[0]) {
            col.children[k++] = Foreground(Z_COLOR_TEXT_MUTED,
                Font(Z_FONT_FOOTNOTE, Text("Swipe up for passcode")));
            col.children[k++] = lgap(14.0f);
        }
        col.children[k++] = Rect(.color = Z_COLOR_TEXT, .width = 140,
                                 .height = 5, .radius = 3);
        col.children[k++] = lgap(12.0f);
        content = z_stack(Z_AXIS_VERTICAL, &col);
    }

    // THE UNLOCK DRAG. The whole plate — clock, cards, indicator — rises with the
    // finger and fades over UNLOCK_FADE of travel, so the gesture shows its own
    // progress and a half-committed drag reads as half-done. The wallpaper does
    // NOT move: it belongs to the phone, not to the lock screen sitting on it.
    float lift = s->lift ? z_animated_get(s->lift) : 0.0f;
    if (lift > 0.0f) {
        lift = 0.0f;
    }
    float fade = 1.0f + lift / UNLOCK_FADE;   // lift is <= 0
    if (fade < 0.0f) {
        fade = 0.0f;
    }
    // NOTE the inner Fill on `content`. A depth stack sizes each child to its own
    // content, so without it the column shrink-wraps its children and the Spacer()
    // that is supposed to push the home indicator to the bottom edge has nothing to
    // expand into — the indicator ends up tucked under the last card, mid-screen.
    ZView plate = Opacity(fade, OffsetXY(0.0f, lift,
        Fill(ZStack(Fill(content), .align = Z_ALIGN_CENTER,
                    .padding = LOCK_SIDE))));

    // Backdrop: the shared wallpaper (cover-fit) under a darkening scrim for
    // legibility, else an opaque BG fallback. z_full_repaint (set by body for the
    // LOCKED phase) makes the scrim's alpha blend correctly over the photo. The
    // scrim is lighter than it was (170 -> 130): the plate's own material cards
    // and the display-weight clock carry their own contrast now, and a wallpaper
    // you cannot see is a wallpaper you did not pick.
    ZView backdrop = s->wp_ok
        ? Fill(ZStack(
              Fill(Cover(Image(s->wp_path))),
              Fill(Background(z_scrim(130), Fill(Spacer()))),
              plate,
              .align = Z_ALIGN_CENTER))
        : Background(Z_COLOR_BG, plate);

    // The whole surface takes the swipe (OnPan decides from where it began); a tap
    // on the backdrop is absorbed so nothing falls through. The keypad's digit
    // buttons are OnTap targets nested inside — tap vs. swipe is decided by slop.
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
    // The second notification sink (the shade is the other). Subscribed on the
    // first build for the same reason: ctrl_fd exists by now.
    if (!s->subscribed_notifs) {
        s->subscribed_notifs = true;
        z_notify_subscribe(app, on_notif_show, on_notif_hide, s);
        // Headless test hook: ZELTO_LOCK_NOTIFS=<n> fabricates n cards directly in
        // the sink, so the lock screen's notification layout is deterministically
        // shot-verifiable without the flaky post -> consent -> grant -> deliver
        // dance across three processes (mirrors the shade's ZELTO_BANNER_DEMO).
        const char *ln = getenv("ZELTO_LOCK_NOTIFS");
        int want = (ln && ln[0]) ? atoi(ln) : 0;
        static const char *demo[][3] = {
            {"os.zelto.pinger", "Ping", "You have a new ping"},
            {"os.zelto.notes", "Shopping list", "3 items left to buy"},
            {"os.zelto.fetch", "Download finished", "release-notes.txt"},
            {"os.zelto.store", "Update available", "Notepad 1.2 is ready"},
            {"os.zelto.cards", "Card added", "Zelto OS design tokens"},
        };
        for (int i = 0; i < want && i < LOCK_MAX_NOTIFS; i++) {
            const char **d = demo[i % 5];
            s->notifs[i].used = true;
            s->notifs[i].id = 9000 + i;
            snprintf(s->notifs[i].app_id, sizeof(s->notifs[i].app_id), "%s", d[0]);
            snprintf(s->notifs[i].title, sizeof(s->notifs[i].title), "%s", d[1]);
            snprintf(s->notifs[i].body, sizeof(s->notifs[i].body), "%s", d[2]);
        }
    }
    // The unlock drag's spring, allocated unconditionally every rebuild so its
    // retained identity stays stable. ZELTO_LOCK_DRAG=<px, negative = up> pins the
    // plate mid-lift so the gesture is shot-verifiable with no injected input.
    s->lift = z_animated_value(app, 0.0f);
    const char *ld = getenv("ZELTO_LOCK_DRAG");
    if (ld && ld[0]) {
        z_animated_pin(s->lift, (float)atof(ld));
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
