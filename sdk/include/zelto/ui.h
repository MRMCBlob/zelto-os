// C API: UI (<zelto/ui.h>)
//
// The native declarative UI toolkit. An app is a Wayland client linking
// libzelto; its body() returns a ZView tree that the framework lays out and
// renders into its surface. See docs/api-reference/c/ui.md and
// docs/guides/declarative-ui.md.
//
// MVP surface: View/Stack/Rect/Text/Spacer nodes, stack+flex layout, a software
// renderer, and HarfBuzz/FreeType text. Animation, gestures, lists, navigation,
// and the rest of docs/api-reference/c/ui.md are Planned.
#ifndef ZELTO_UI_H
#define ZELTO_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zelto/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles. A ZView is an arena-allocated node, cheap to create and
// thrown away each rebuild; never freed by the app.
typedef struct ZNode *ZView;
typedef struct ZApp ZApp;

// Persistent (non-arena) handles. Unlike views, these survive rebuilds: the
// framework keeps them in per-screen retained storage so an animated value or a
// scroll position carries across body() calls. Allocated by call order within
// the building screen (the first z_animated_value() call in a screen body maps
// to the same cell every rebuild — a stable identity à la React hooks). See
// docs/contributing/sdk-internals.md ("View tree vs. scene graph").
typedef struct ZAnimated ZAnimated;   // a spring-backed scalar
typedef struct ZScroll ZScroll;       // a scroll position + fling state
typedef struct ZNav ZNav;             // the navigation stack

// ---------------------------------------------------------------------------
// Callbacks & actions.
//
// A ZAction is a tap/activation handler; a ZKeyAction handles a key press. Both
// receive the live ZApp and the app's persistent state struct (the same pointer
// passed to body), so they can mutate state and call z_invalidate. They are
// invoked from the app loop on input, NOT during body(), so they take app/state
// as parameters rather than capturing them.
//
// Handlers are ordinary named functions in C; the JS/Script binding's inline
// closures (docs/guides/gestures.md) cannot be expressed as a portable C macro
// under the project's strict ISO C settings (no GNU statement-expressions /
// nested functions). Define a static function and pass it:
//
//   static void on_tap(ZApp *app, void *state) {
//       App *s = state; s->count++; z_invalidate(app);
//   }
//   ...  Button(on_tap, "Count: %d", s->count)
typedef void (*ZAction)(ZApp *app, void *state);
typedef void (*ZKeyAction)(ZApp *app, void *state, uint32_t keysym);

// Like ZAction but also receives a per-view data pointer bound at build time —
// the way a data-driven row carries its item into the tap handler (closures
// being unavailable in strict C). The data must outlive the tap (point into
// stable state). See OnTapData below.
typedef void (*ZTapAction)(ZApp *app, void *state, void *data);

// Maximum children collected by a single stack literal (see the options trick
// below). Plenty for hand-written UI; List handles large data sets (Planned).
#define Z_MAX_CHILDREN 32

// Cross-axis alignment within a stack.
typedef enum ZAlign {
    Z_ALIGN_LEADING = 0,
    Z_ALIGN_CENTER,
    Z_ALIGN_TRAILING,
} ZAlign;

// Stack direction.
typedef enum ZAxis {
    Z_AXIS_VERTICAL = 0,   // VStack: top -> bottom
    Z_AXIS_HORIZONTAL,     // HStack: leading -> trailing
    Z_AXIS_DEPTH,          // ZStack: back -> front (overlap)
} ZAxis;

// Font size tokens (logical px). A themed type scale is Planned.
typedef enum ZFont {
    Z_FONT_CAPTION = 12,
    Z_FONT_BODY = 16,
    Z_FONT_CALLOUT = 20,
    Z_FONT_TITLE = 28,
    Z_FONT_LARGE_TITLE = 40,
} ZFont;

// ---------------------------------------------------------------------------
// Stacks.
//
// Children are listed first (positionally); options follow as designated
// initializers. This ordering is what C makes robust: positional initializers
// fill `children[0..]` (the first struct member), and the `.field = value`
// options that follow can be given in any subset/order. Unused trailing child
// slots zero-initialize to NULL, which terminates the list. E.g.
//
//   VStack(
//       Text("Title"),
//       Rect(.color = Z_COLOR_PRIMARY, .width = 80, .height = 80),
//       .spacing = 12, .align = Z_ALIGN_LEADING, .padding = 16);
// ---------------------------------------------------------------------------
typedef struct ZStackOpts {
    ZView children[Z_MAX_CHILDREN];      // MUST be first (positional children)
    float spacing;
    ZAlign align;
    float padding;
    float grow;                          // flex weight if nested in a stack
} ZStackOpts;

ZView z_stack(ZAxis axis, const ZStackOpts *opts);

#define VStack(...) z_stack(Z_AXIS_VERTICAL, &(ZStackOpts){__VA_ARGS__})
#define HStack(...) z_stack(Z_AXIS_HORIZONTAL, &(ZStackOpts){__VA_ARGS__})
#define ZStack(...) z_stack(Z_AXIS_DEPTH, &(ZStackOpts){__VA_ARGS__})

// A flexible gap that eats free main-axis space.
ZView z_spacer(void);
#define Spacer() z_spacer()

// ---------------------------------------------------------------------------
// Content.
// ---------------------------------------------------------------------------

// A solid (optionally rounded) coloured box.
typedef struct ZRectOpts {
    ZColor color;
    float width, height;   // 0 = size to parent / flex
    float radius;
    float grow;
} ZRectOpts;

ZView z_rect(const ZRectOpts *opts);
#define Rect(...) z_rect(&(ZRectOpts){__VA_ARGS__})

// A line of text (printf-style).
ZView z_text(const char *fmt, ...);
#define Text(...) z_text(__VA_ARGS__)

// A tappable control: a rounded, padded label that runs `on_tap` when tapped or
// activated from the keyboard (Enter/Space while focused). printf-style label.
//   Button(on_tap, "Count: %d", s->count);   // on_tap is a static ZAction
ZView z_button(ZAction on_tap, const char *fmt, ...);
#define Button(action, ...) z_button(action, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Modifiers. Each wraps a ZView and returns it (apply outermost-last).
// ---------------------------------------------------------------------------
ZView Background(ZColor color, ZView view);
ZView Foreground(ZColor color, ZView view);
ZView Padding(float all, ZView view);
ZView Frame(float width, float height, ZView view);
ZView CornerRadius(float radius, ZView view);
ZView Font(ZFont size, ZView view);
ZView Grow(float weight, ZView view);
// Expand this view to fill its parent's inner box. A depth stack (ZStack)
// otherwise centres each child at its own content size; Fill is how a
// full-screen layer (e.g. a wallpaper, or a slide-up sheet) covers the stack.
ZView Fill(ZView view);

// Gesture / input modifiers. OnTap makes any view tappable; OnKey makes it a
// keyboard target (keys are delivered to the first focusable view in the tree).
ZView OnTap(ZAction action, ZView view);
ZView OnKey(ZKeyAction action, ZView view);
// Tap handler that carries a data pointer (e.g. the list item a row stands for).
ZView OnTapData(ZTapAction action, void *data, ZView view);

// ---------------------------------------------------------------------------
// Gestures: pan / drag.
//
// A pan recognizer fires as the pointer drags across a view. A press that moves
// less than the slop threshold stays a tap (OnTap); once it crosses the slop it
// becomes a pan and the tap is cancelled. Inside a Scroll/List vertical drags
// scroll automatically; OnPan is for custom drags (swipe-to-dismiss, sliders).
// translation_* is the delta since the gesture began; velocity_* is in px/s
// (used for fling). Handlers are named functions, like ZAction.
// ---------------------------------------------------------------------------
typedef enum ZPanPhase {
    Z_PAN_BEGIN = 0,
    Z_PAN_CHANGED,
    Z_PAN_END,
} ZPanPhase;

typedef struct ZPanEvent {
    float x, y;                       // current pointer pos (surface-local)
    float translation_x, translation_y;
    float velocity_x, velocity_y;     // px/s, valid at Z_PAN_END
    ZPanPhase phase;
} ZPanEvent;

typedef void (*ZPanHandler)(ZApp *app, void *state, const ZPanEvent *e);

ZView OnPan(ZPanHandler handler, ZView view);

// Long-press: fires when a press is held in place past a time threshold without
// crossing the pan slop. It composes with OnTap and OnPan on the same subtree —
// moving past the slop first becomes a pan (no long-press), a quick release
// stays a tap, and a still hold past the threshold fires the long-press and
// suppresses the tap that the release would otherwise have produced. `data` is a
// per-view pointer bound at build time (like OnTapData — point it at stable
// state); `x`/`y` are the surface-local press position (e.g. to place a menu).
typedef void (*ZLongPressHandler)(ZApp *app, void *state, void *data, float x,
                                  float y);

ZView OnLongPress(ZLongPressHandler handler, void *data, ZView view);

// ---------------------------------------------------------------------------
// Scrolling.
//
// Scroll wraps a single content child and clips it to the viewport, translating
// it by a persistent offset that responds to the pointer wheel and to vertical
// pan drags (with momentum/spring settling on release). For long, data-driven
// content use List, which builds only the rows in (and just around) the viewport
// — virtualised by a fixed row_height — and keys rows so the reconciler reuses
// them across scrolls. Both take `app` first (they allocate a retained ZScroll
// cell from the current screen).
// ---------------------------------------------------------------------------
typedef struct ZScrollOpts {
    ZView children[Z_MAX_CHILDREN];   // MUST be first; only children[0] (content) is used
    ZAxis axis;                       // Z_AXIS_VERTICAL (default) or _HORIZONTAL
} ZScrollOpts;

ZView z_scroll_view(ZApp *app, const ZScrollOpts *opts);
#define Scroll(appp, ...) z_scroll_view(appp, &(ZScrollOpts){__VA_ARGS__})

// A row's stable identity and its view, given the item and its index. `data` is
// the array base and `stride` its element size, so any contiguous array works:
// item i lives at (const char *)data + i*stride.
typedef uint64_t (*ZKeyFn)(const void *item, int index);
typedef ZView (*ZRowFn)(ZApp *app, const void *item, int index);

typedef struct ZListOpts {
    const void *data;
    size_t stride;
    int count;
    float row_height;                 // fixed row height (required for virtualisation)
    ZKeyFn key;
    ZRowFn row;
} ZListOpts;

ZView z_list(ZApp *app, const ZListOpts *opts);
#define List(appp, ...) z_list(appp, &(ZListOpts){__VA_ARGS__})

// Programmatic scroll handle (e.g. scroll-to-top). z_scroll allocates/fetches
// the current screen's next retained scroll cell, matching the one a Scroll/List
// built in the same position consumes.
ZScroll *z_scroll(ZApp *app);
void z_scroll_to(ZScroll *sc, float x, float y, bool animated);

// ---------------------------------------------------------------------------
// Animation.
//
// Motion is spring-based. An animated value is a persistent scalar you bind to a
// transform with Offset(); z_animated_spring drives it toward a target and the
// framework advances it on the surface frame callback (continuous repaint while
// in flight, idle once settled). z_with_animation runs a state change under a
// chosen spring (the implicit-animation default for values created within).
// ---------------------------------------------------------------------------
typedef enum ZSpring {
    Z_SPRING_STANDARD = 0,            // default: settles smoothly
    Z_SPRING_SNAPPY,                  // stiffer, quicker
} ZSpring;

ZAnimated *z_animated_value(ZApp *app, float initial);
void z_animated_set(ZAnimated *v, float to);        // jump (no animation)
void z_animated_spring(ZAnimated *v, float to);      // spring toward `to`
float z_animated_get(const ZAnimated *v);            // current value

void z_with_animation(ZApp *app, ZSpring spring, ZAction change);

// Bind an animated value to a horizontal translation: the subtree is shifted by
// (z_animated_get(x), y). Use for gesture-driven drags and screen transitions.
ZView Offset(ZAnimated *x, float y, ZView view);

// ---------------------------------------------------------------------------
// Navigation.
//
// A Navigator owns a stack of screens and slides between them with the standard
// spring. A screen is a function returning a view; it reads the stack through
// z_navigation(app) and receives the props passed at push time (props must
// outlive the screen — pass a pointer into stable app state). Back is the system
// gesture (edge-swipe) or the Escape/Backspace key; both pop the top screen.
// ---------------------------------------------------------------------------
typedef ZView (*ZScreenFn)(ZApp *app, void *props);

typedef struct ZNavOpts {
    ZScreenFn root;
} ZNavOpts;

ZView z_navigator(ZApp *app, const ZNavOpts *opts);
#define Navigator(appp, ...) z_navigator(appp, &(ZNavOpts){__VA_ARGS__})

ZNav *z_navigation(ZApp *app);
void z_nav_push(ZNav *nav, ZScreenFn screen, void *props);
void z_nav_pop(ZNav *nav);

// ---------------------------------------------------------------------------
// App entry + lifecycle.
// ---------------------------------------------------------------------------

// Body function: returns the view tree for the current state.
typedef ZView (*ZBodyFn)(ZApp *app, void *state);

// Implementation entry used by Z_APP. Connects to Wayland, runs the loop.
int z_app_main(void *state, ZBodyFn body, const char *title);

// Like z_app_main but also sets the xdg app_id (used by Z_APP_ID). The System UI
// launcher sets "os.zelto.launcher" so the compositor's Home chord can find it.
int z_app_main_id(void *state, ZBodyFn body, const char *title,
                  const char *app_id);

// Z_APP(StateType, bodyFn) generates main(). bodyFn has signature:
//   ZView bodyFn(ZApp *app, StateType *state);
#define Z_APP(T, BODYFN)                                                  \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        return z_app_main(&z__state, z__body_trampoline, #BODYFN);        \
    }

// Like Z_APP but with an explicit xdg app_id string.
#define Z_APP_ID(T, BODYFN, APP_ID)                                       \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        return z_app_main_id(&z__state, z__body_trampoline, #BODYFN,      \
                             (APP_ID));                                   \
    }

// ---------------------------------------------------------------------------
// Layer-shell role (System UI).
//
// A normal app maps an xdg_toplevel and is window-managed by the compositor. A
// layer-shell app (the status bar, a launcher background, a notification shade)
// instead anchors itself to a screen edge in a fixed layer with an optional
// exclusive zone that reserves space the compositor keeps clear of app windows.
// Same declarative body()/layout/paint/input loop; only the surface role differs.
// ---------------------------------------------------------------------------
typedef enum ZLayer {
    Z_LAYER_BACKGROUND = 0,   // wallpaper, behind everything
    Z_LAYER_BOTTOM,           // below app windows
    Z_LAYER_TOP,              // above app windows (status bar)
    Z_LAYER_OVERLAY,          // above everything (shade, lock screen)
} ZLayer;

// Anchor bitmask (matches the wlr-layer-shell anchor edges). Anchoring to two
// opposite edges stretches the surface along that axis.
enum {
    Z_ANCHOR_TOP = 1,
    Z_ANCHOR_BOTTOM = 2,
    Z_ANCHOR_LEFT = 4,
    Z_ANCHOR_RIGHT = 8,
};

typedef struct ZLayerOpts {
    ZLayer layer;             // which layer to live in
    uint32_t anchor;          // Z_ANCHOR_* bitmask (0 = centered)
    int32_t exclusive_zone;   // px reserved from the app area (-1 = ignore others)
    int32_t width, height;    // desired size; 0 on an axis = size from anchors
    bool keyboard;            // grab EXCLUSIVE keyboard focus (a modal dialog)
    // Margins (px) inset from the anchored edges, e.g. margin_top to float a
    // surface below the status bar instead of over it.
    int32_t margin_top, margin_right, margin_bottom, margin_left;
} ZLayerOpts;

// Run an app as a layer-shell surface. Returns when the surface is closed.
int z_layer_app_main(void *state, ZBodyFn body, const char *title,
                     const ZLayerOpts *opts);

// Z_LAYER_APP(StateType, bodyFn, layerOptsLiteral) generates main() for a
// layer-shell System-UI app, e.g.
//   Z_LAYER_APP(BarState, bar_body,
//       .layer = Z_LAYER_TOP, .anchor = Z_ANCHOR_TOP | Z_ANCHOR_LEFT |
//       Z_ANCHOR_RIGHT, .exclusive_zone = 40, .height = 40);
#define Z_LAYER_APP(T, BODYFN, ...)                                       \
    static ZView z__body_trampoline(ZApp *app, void *state) {             \
        return BODYFN(app, (T *)state);                                   \
    }                                                                     \
    int main(void) {                                                      \
        static T z__state;                                                \
        ZLayerOpts z__opts = {__VA_ARGS__};                               \
        return z_layer_app_main(&z__state, z__body_trampoline, #BODYFN,   \
                                &z__opts);                                \
    }

// Resize a layer-shell surface at runtime. A layer app's size is normally fixed
// at startup (the Z_LAYER_APP opts), but a surface whose footprint changes — the
// notification shade, which should occupy the top of the screen only while a
// banner is up and reserve/cover nothing when idle — re-requests its size here.
// Passing 0 on an axis lets the compositor size it from the anchors. No-op for
// an xdg (non-layer) app. The new size arrives via the next configure + repaint.
void z_layer_resize(ZApp *app, int width, int height);

// Current surface size in pixels (after the latest configure). A full-screen
// layer that sizes or translates itself — a slide-up app drawer offset by its
// own height — needs the height; both are read fresh inside body().
int z_app_width(ZApp *app);
int z_app_height(ZApp *app);

// Request a rebuild (like setState). Schedules a new body()/layout/paint.
void z_invalidate(ZApp *app);

// Force the NEXT paint to repaint the whole surface instead of just the diffed
// damage rects. Call it from body() on a frame where a large subtree is moving
// under an Offset (a slide-up sheet, a drag): the per-rect partial path
// under-damages a big translated subtree and leaves stale pixels, so motion
// frames want a full repaint (the same path screen transitions already take).
void z_full_repaint(ZApp *app);

// Request the app to exit its loop.
void z_app_quit(ZApp *app);

// ---------------------------------------------------------------------------
// Lifecycle.
//
// An app moves between foreground (Active) and background (Inactive) as the user
// switches apps; it is Stopped when the compositor asks it to close. The state
// rides the standard xdg "activated" toplevel state the compositor broadcasts to
// exactly the front window — no custom protocol. Register a handler to save/
// restore work across transitions; the framework also repaints on every change
// so a body() that reads z_app_active() reflects the live state.
// See docs/platform/app-lifecycle.md.
// ---------------------------------------------------------------------------
typedef enum ZLifecycle {
    Z_LC_ACTIVE = 0,   // entered the foreground (xdg activated)
    Z_LC_INACTIVE,     // left the foreground (paused / another app in front)
    Z_LC_STOPPED,      // the compositor asked the app to close
} ZLifecycle;

typedef void (*ZLifecycleHandler)(ZApp *app, void *state, ZLifecycle ev);

// Register the lifecycle handler (one per app). `state` passed to it is the same
// pointer given to body(). Pass NULL to clear.
void z_on_lifecycle(ZApp *app, ZLifecycleHandler handler);

// True while the app is the foreground (activated) window.
bool z_app_active(ZApp *app);

// ---------------------------------------------------------------------------
// Permissions.
//
// Sensitive capabilities (camera, location, ...) are declared in the app's
// manifest and granted at runtime by the user. The grant is brokered by the
// zsysd system daemon over a unix socket; libzelto wraps it so the app never
// touches the IPC. z_perm_status is a quick synchronous query; z_perm_request
// may show a system consent dialog, so it is asynchronous — the reply (after the
// user answers) is delivered to the callback from the app loop, not inline.
// Re-requesting an already-granted permission returns immediately (no dialog).
// See docs/platform/permissions.md + docs/api-reference/c/platform.md.
// ---------------------------------------------------------------------------
typedef enum ZPermStatus {
    Z_PERM_GRANTED = 0,   // the user granted it (cached)
    Z_PERM_DENIED,        // denied (cached) or not declared in the manifest
    Z_PERM_PROMPT,        // declared, no decision yet — request to prompt
} ZPermStatus;

// Delivered the outcome of z_perm_request. `app`/`ud` are the live app and the
// pointer passed to z_perm_request (typically the app's state struct).
typedef void (*ZPermCallback)(ZApp *app, ZPermStatus status, void *ud);

// Current status of a permission for this app (synchronous round-trip).
ZPermStatus z_perm_status(const char *name);   // "camera", "location", ...

// Request a permission. If a decision is cached the callback fires almost
// immediately with it; otherwise the system shows a consent dialog and the
// callback fires once the user answers. Does not block the render loop.
void z_perm_request(const char *name, ZPermCallback cb, void *ud);

// ---------------------------------------------------------------------------
// App-to-app intents: deep links + share targets.
//
// An app hands content to another app without knowing which: z_open_url hands a
// URL to whichever installed app registered that scheme (a deep link), and
// z_share presents a share sheet of apps that accept the payload's MIME type.
// Both are brokered by zsysd, which resolves candidate handlers from manifests
// (the `[links] schemes=` and `[capabilities] share-targets=` fields), shows the
// System-UI chooser when there is a choice, then launches/activates the target
// and delivers the payload to it. The receiving app registers a handler that
// fires when an intent arrives (even if it had to be launched to handle it).
// See docs/platform/ipc-and-intents.md + docs/api-reference/c/platform.md.
// ---------------------------------------------------------------------------

// One item being shared. `mime` is its MIME type (e.g. "text/plain"); `text`
// carries the payload for text MIME types. (Binary payloads are Planned.)
typedef struct ZShareItem {
    const char *mime;
    const char *text;
} ZShareItem;

// Incoming-deep-link handler: the URL another app (or the system) opened on us.
typedef void (*ZUrlCb)(ZApp *app, const char *url, void *ud);

// Incoming-share handler: the items another app shared to us. `items` is valid
// only for the duration of the call (copy what you keep).
typedef void (*ZShareCb)(ZApp *app, const ZShareItem *items, int count,
                         void *ud);

// Hand a URL to the system: zsysd resolves the scheme to its registered handler
// app, launching or activating it, and fires that app's z_on_open_url. No-op if
// no app handles the scheme or the broker is unreachable.
void z_open_url(const char *url);

// Present the share sheet for `items`: zsysd resolves the apps that accept the
// MIME type, the System UI shows a chooser, and the picked app receives the
// payload via z_on_share_target (launched first if it was not running).
void z_share(ZShareItem *items, int count);

// Register this app's incoming-deep-link / incoming-share handlers (one each).
// A queued intent that arrived before the handler was set fires as soon as it
// is registered, so an app launched to handle an intent never misses it.
void z_on_open_url(ZApp *app, ZUrlCb cb, void *ud);
void z_on_share_target(ZApp *app, ZShareCb cb, void *ud);

// ---------------------------------------------------------------------------
// Notifications.
//
// An app posts a notification to the Zelto shade (heads-up banner) via the
// zsysd notification broker; it requires the `notifications` permission (the
// first post with no stored grant shows the system consent dialog, exactly like
// any other permission). A notification carries a title + body, an optional
// channel (importance grouping), an optional tap_route deep link delivered
// through z_open_url when the banner body is tapped, and optional action
// buttons. Tapping an action routes back to the poster's
// z_on_notification_action handler. Build with z_notify_new + the setters, then
// z_notify_post (a synchronous round-trip that returns the assigned id and frees
// the builder). See docs/guides/notifications.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// Channel importance (how prominently the shade presents the notification). The
// shade's per-channel tuning is Planned; the MVP records the channel and treats
// every banner as a heads-up card.
typedef enum ZImportance {
    Z_IMPORTANCE_MIN = 0,
    Z_IMPORTANCE_LOW,
    Z_IMPORTANCE_DEFAULT,
    Z_IMPORTANCE_HIGH,
} ZImportance;

// Opaque notification builder. Allocated by z_notify_new, mutated by the
// setters, consumed (and freed) by z_notify_post.
typedef struct ZNotification ZNotification;

// Register a channel (importance grouping). MVP: recorded by the broker; the
// per-channel importance routing in the shade is Planned.
void z_notify_define_channel(const char *id, const char *name, ZImportance imp);

// Build a notification. Copies title + body; returns a builder to configure and
// post. The MVP supports a single action button (the last z_notify_add_action
// wins).
ZNotification *z_notify_new(const char *title, const char *body);
void z_notify_set_channel(ZNotification *n, const char *channel_id);
void z_notify_set_tap_route(ZNotification *n, const char *url);   // deep link on body tap
void z_notify_add_action(ZNotification *n, const char *id, const char *title);

// Post the notification (synchronous: blocks until the broker — possibly after a
// consent prompt — assigns an id). Returns the global notification id, or -1 if
// the permission was denied / the broker is unreachable. Frees the builder.
int64_t z_notify_post(ZNotification *n);

// Cancel a posted notification by id (removes its banner from the shade).
void z_notify_cancel(int64_t id);

// Set the app-icon badge count (0 clears). Forwarding to the bar/shade is
// Planned; the broker currently records it only.
void z_notify_set_badge(int count);

// An action / body tap delivered back to the poster. action_id is NULL or empty
// when the notification body itself was tapped (the tap_route already routed via
// z_open_url); a non-empty action_id is the id of the action button tapped.
typedef struct ZNotifyActionEvent {
    int64_t notification_id;
    const char *action_id;
} ZNotifyActionEvent;

typedef void (*ZNotifyActionCb)(ZApp *app, const ZNotifyActionEvent *e,
                                void *ud);

// Register this app's notification-action handler (one per app). A queued action
// that arrived before the handler was set fires as soon as it is registered (an
// app launched to handle its own action never misses it).
void z_on_notification_action(ZApp *app, ZNotifyActionCb cb, void *ud);

// ---------------------------------------------------------------------------
// Notification shade sink (System UI).
//
// The shade is an ordinary libzelto OVERLAY layer-shell app that subscribes as
// the single notification sink. The broker pushes a notify_show for every posted
// notification (post-grant) and a notify_hide when one is cancelled/dismissed.
// On a banner body tap the shade opens the tap_route itself (z_open_url) and
// reports the tap so the broker drops the banner; on an action tap it reports
// the action so the broker routes it to the poster, then drops the banner.
// ---------------------------------------------------------------------------
typedef struct ZShownNotification {
    int64_t id;
    const char *app_id;
    const char *title;
    const char *body;
    const char *tap_route;
    const char *action_id;
    const char *action_title;
} ZShownNotification;

typedef void (*ZNotifyShowCb)(ZApp *app, const ZShownNotification *n, void *ud);
typedef void (*ZNotifyHideCb)(ZApp *app, int64_t id, void *ud);

// Subscribe this app as the notification sink (last subscriber wins). The
// callbacks fire from the app loop when the broker pushes a show/hide.
void z_notify_subscribe(ZApp *app, ZNotifyShowCb on_show, ZNotifyHideCb on_hide,
                        void *ud);

// Report a banner body tap (the shade drops the banner; the deep link is routed
// separately via z_open_url) or an action-button tap (routed to the poster).
void z_notify_report_tap(int64_t id);
void z_notify_report_action(int64_t id, const char *action_id);

// ---------------------------------------------------------------------------
// Persistent storage: preferences, files, and SQLite — all scoped to the app's
// private data directory under $ZELTO_DATA_DIR (/var/zelto), keyed by app_id.
//
// Each app gets /var/zelto/apps/<app_id>/{documents,cache}. Preferences live in
// a small key=value store inside documents; SQLite databases are ordinary files
// in documents; cache is the same shape but the system may evict it. The paths
// are created on first use. Storage is direct filesystem access (no zsysd
// round-trip) on a real writable disk, so it survives a reboot.
// See docs/guides/storage.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// A read result. ok == false on failure (missing file, denied); data is a
// heap buffer (NUL-terminated for convenience) the caller frees with free().
typedef struct ZBytes {
    void *data;
    size_t len;
    bool ok;
} ZBytes;

// A directory listing. Free with z_list_free. Each name is a basename within
// the listed directory (not a full path).
typedef struct ZList {
    char **items;
    int count;
} ZList;

void z_list_free(ZList *list);

// --- Preferences (small key/value settings) -------------------------------
bool        z_prefs_set_str(const char *key, const char *value);
const char *z_prefs_get_str(const char *key, const char *fallback);
bool        z_prefs_set_int(const char *key, int64_t value);
int64_t     z_prefs_get_int(const char *key, int64_t fallback);
bool        z_prefs_remove(const char *key);

// --- Files (paths relative to the app's private directory) ----------------
bool   z_file_write(const char *path, const void *data, size_t len);
ZBytes z_file_read(const char *path);          // .ok == false on failure
bool   z_file_delete(const char *path);
ZList *z_file_list(const char *dir);
char  *z_path_documents(const char *rel);      // persistent;  caller frees
char  *z_path_cache(const char *rel);          // evictable;   caller frees

// --- Database (SQLite) -----------------------------------------------------
typedef struct ZDatabase ZDatabase;            // opaque connection handle
typedef struct ZRows ZRows;                    // opaque cursor over a query

// A single bound parameter (int or text). Built via the z_args macro; users
// never construct these directly.
typedef struct ZArg {
    int is_text;          // 0 = integer (i), 1 = text (s)
    int64_t i;
    const char *s;
} ZArg;

// The parameter list passed to z_db_run / z_db_query. Built by z_args(...),
// which tags each argument by type (int vs. string) at the call site.
typedef struct ZArgs {
    int n;
    ZArg v[8];            // up to 8 bound parameters (MVP)
} ZArgs;

// Internal helpers behind the z_args macro (do not call directly).
ZArg  z_arg_int_(int64_t x);
ZArg  z_arg_str_(const char *x);
ZArgs z_args_make_(int n, const ZArg *v);

// Tag one argument by its C type. `+0` forces array-to-pointer decay so a string
// literal (char[N]) selects the text branch; integers fall through to default.
#define Z_ARG_(x) _Generic((x) + 0,           \
        char *:       z_arg_str_,             \
        const char *: z_arg_str_,             \
        default:      z_arg_int_)((x) + 0)

#define Z_CAT_(a, b) a##b
#define Z_CAT(a, b) Z_CAT_(a, b)

// Count 1..6 macro arguments. (We can't count zero: invoking a `...` macro with
// no arguments is itself ill-formed under -std=c17 -Wpedantic, so the no-binding
// case is spelled Z_NO_ARGS instead of z_args().)
#define Z_NTH6_(_1, _2, _3, _4, _5, _6, N, ...) N
#define Z_NARG1(...) Z_NTH6_(__VA_ARGS__, 6, 5, 4, 3, 2, 1)

// Map each argument through Z_ARG_ (trailing comma so the sentinel follows).
#define Z_MAP1(a) Z_ARG_(a),
#define Z_MAP2(a, b) Z_ARG_(a), Z_ARG_(b),
#define Z_MAP3(a, b, c) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c),
#define Z_MAP4(a, b, c, d) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d),
#define Z_MAP5(a, b, c, d, e) Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d), Z_ARG_(e),
#define Z_MAP6(a, b, c, d, e, f) \
    Z_ARG_(a), Z_ARG_(b), Z_ARG_(c), Z_ARG_(d), Z_ARG_(e), Z_ARG_(f),

// z_args("Buy milk", 0) -> a ZArgs binding text then integer parameters (1..6).
// The trailing {0,0,0} keeps the compound-literal array non-empty (valid ISO C);
// z_args_make_ copies only the first Z_NARG1 entries. For a query/statement with
// no bound parameters, pass Z_NO_ARGS.
#define z_args(...)                                                     \
    z_args_make_(Z_NARG1(__VA_ARGS__),                                  \
                 (ZArg[]){Z_CAT(Z_MAP, Z_NARG1(__VA_ARGS__))(__VA_ARGS__){0, 0, 0}})

// No bound parameters (object-like, so it sidesteps the zero-arg variadic-macro
// restriction). Use where the docs show z_args(): z_db_query(db, sql, Z_NO_ARGS).
#define Z_NO_ARGS (z_args_make_(0, (const ZArg *)0))

ZDatabase *z_db_open(const char *name);              // <documents>/<name>(.db)
bool       z_db_exec(ZDatabase *db, const char *sql);          // schema / DDL
bool       z_db_run(ZDatabase *db, const char *sql, ZArgs args);   // writes
ZRows     *z_db_query(ZDatabase *db, const char *sql, ZArgs args); // reads
void       z_db_close(ZDatabase *db);

bool        z_rows_next(ZRows *r);                   // advance; false at end
int64_t     z_rows_int(ZRows *r, int col);
const char *z_rows_str(ZRows *r, int col);           // valid until next/free
void        z_rows_free(ZRows *r);

// ---------------------------------------------------------------------------
// Networking.
//
// Apps make asynchronous HTTP requests and open WebSockets, gated by the
// `network` permission (declared in the manifest; the first send with no stored
// grant shows the system consent dialog, like any permission). Nothing blocks
// the render loop: the socket is driven from the app loop and the callback fires
// once the response is complete. The MVP is plain HTTP/1.x over TCP to a numeric
// IPv4 host; DNS and TLS/https are Planned.
// See docs/guides/networking.md + docs/api-reference/c/system.md.
// ---------------------------------------------------------------------------

// A heap request builder + in-flight handle. Created by z_net_get/z_net_request,
// configured with the setters, consumed by z_net_send (which owns and frees it
// once the callback fires) or released by z_net_cancel.
typedef struct ZNetRequest ZNetRequest;

// A completed response, passed to the callback. Valid only for the duration of
// the call — copy what you keep. Read `status` (HTTP status code; 0 on a
// transport error) and `ok` (true for 2xx) directly; reach the body/headers
// through z_net_body / z_net_header. The trailing fields are framework-internal.
typedef struct ZNetResponse {
    int status;
    bool ok;
    char *raw;          // owns the whole response (headers + body); internal
    size_t raw_len;
    char *body;         // into raw (NUL-terminated); internal
    size_t body_len;
    char *hdr_end;      // points at the CRLFCRLF; internal
    char scratch[256];  // z_net_header return storage; internal
} ZNetResponse;

// Result callback: `res` is the response; `ud` is the pointer passed to send.
typedef void (*ZNetCallback)(ZNetResponse *res, void *ud);

// Build a GET / arbitrary-method request to `url` ("http://10.0.2.2:8080/x").
// Returns NULL on a malformed URL.
ZNetRequest *z_net_get(const char *url);
ZNetRequest *z_net_request(const char *method, const char *url);

// Add a request header / set the request body (both copied).
void z_net_set_header(ZNetRequest *r, const char *k, const char *v);
void z_net_set_body(ZNetRequest *r, const void *data, size_t len);

// Send asynchronously. Checks the `network` permission first (showing the
// consent dialog if undecided); on grant it connects, sends, and fires `cb` from
// the app loop when the response is complete (or on error, with res->status==0).
// Frees the request after the callback returns.
void z_net_send(ZNetRequest *r, ZNetCallback cb, void *ud);

// Cancel an in-flight (or unsent) request; no callback fires. Frees it.
void z_net_cancel(ZNetRequest *r);

// Response accessors, valid only until the callback returns. z_net_body returns
// the body bytes (NUL-terminated for convenience); z_net_header looks up a
// response header by name (case-insensitive), NULL if absent.
ZBytes      z_net_body(ZNetResponse *res);
const char *z_net_header(ZNetResponse *res, const char *name);

// WebSockets (unfragmented text frames; binary/fragmented are Planned). Gated by
// the `network` permission (typically already granted via a prior z_net_send).
typedef struct ZWebSocket ZWebSocket;
typedef void (*ZWsMessageCb)(ZWebSocket *ws, const char *text, size_t len,
                             void *ud);

// Open a WebSocket to `url` ("ws://10.0.2.2:8081/echo"): performs the HTTP/1.1
// Upgrade handshake and parks the socket in the app loop. NULL on failure.
ZWebSocket *z_ws_open(const char *url);
void z_ws_on_message(ZWebSocket *ws, ZWsMessageCb cb, void *ud);
void z_ws_send(ZWebSocket *ws, const void *data, size_t len);  // masked text frame
void z_ws_close(ZWebSocket *ws);

// ---------------------------------------------------------------------------
// Task switcher (running apps).
//
// A client (the launcher) can list every other running app window and switch to
// or close it, over the standard wlr-foreign-toplevel-management protocol. The
// app's own window is filtered out by app_id. The list mutates as windows open,
// change title, gain/lose focus, and close; each change repaints the caller.
// ---------------------------------------------------------------------------
typedef struct ZTask {
    const char *title;    // window title (may be NULL until reported)
    const char *app_id;   // window app_id (may be NULL until reported)
    bool active;          // true if this is the foreground window
    void *handle;         // opaque foreign-toplevel handle (for activate/close)
} ZTask;

// A stable snapshot of the running apps (excluding this app's own window). Valid
// until the next z_running_apps call; read it fresh inside body(). *count gets
// the entry count.
const ZTask *z_running_apps(ZApp *app, int *count);

// Switch to / close a running app from the snapshot.
void z_task_activate(ZApp *app, const ZTask *task);
void z_task_close(ZApp *app, const ZTask *task);

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_UI_H
