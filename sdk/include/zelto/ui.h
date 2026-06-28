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
#include <stdint.h>

#include "zelto/gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles. A ZView is an arena-allocated node, cheap to create and
// thrown away each rebuild; never freed by the app.
typedef struct ZNode *ZView;
typedef struct ZApp ZApp;

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
