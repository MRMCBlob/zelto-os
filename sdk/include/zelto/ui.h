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

// Request a rebuild (like setState). Schedules a new body()/layout/paint.
void z_invalidate(ZApp *app);

// Request the app to exit its loop.
void z_app_quit(ZApp *app);

#ifdef __cplusplus
}
#endif

#endif  // ZELTO_UI_H
