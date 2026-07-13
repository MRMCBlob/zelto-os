// The `zelto` module: hooks + the render bridge.
//
// State lives in call-order cells (the same identity trick libzelto uses for its
// retained animation/scroll cells, and the same one React's hooks use): the Nth
// useState of a render is the Nth cell, every render. So hooks must be called
// unconditionally, at the top level of a component — no hooks inside if/loops.
//
// The host (host.c) calls __render() once per body(); everything else here is
// ordinary JS.
import * as N from "zelto:native";

let root = null;
let effects = [];          // queued for after the tree is built

// Hook state lives in a SCOPE: an array of call-order cells plus the counter that
// walks it. The app body is one scope; each Navigator screen is another, because
// the C navigator calls a screen's body directly (and only renders the top one in
// steady state), so a single shared cursor would misalign the moment a screen was
// pushed. A scope also gives a popped screen somewhere to be dropped from.
function newScope() {
  return { cells: [], idx: 0 };
}

const rootScope = newScope();
let scope = rootScope;

// The Navigator's screen instances, mirroring the C stack: index 0 is the root
// screen (z_navigator passes the root props == NULL == 0), each push appends. The
// host identifies a screen by its index here, since a C function pointer has
// nowhere to carry a closure.
//
// The Navigator's root screen is NOT the app's root component — it is the
// component handed to Navigator(). They must differ: the app's root is what
// RENDERS the Navigator, so if the two were the same, rendering screen 0 would
// re-enter the app root and recurse forever.
let screens = [];

// Set by the host with the app's default export.
export function __setRoot(component) {
  root = component;
}

// Called by Navigator() (zelto/ui) with the component to use as screen 0. Runs on
// every render, so it refreshes the closure but keeps the scope: the root screen's
// hook state must survive its own rebuilds.
export function __navRoot(component) {
  if (!screens[0]) {
    screens[0] = { fn: component, props: undefined, scope: newScope() };
  } else {
    screens[0].fn = component;
  }
}

// One build: reset the cell cursor, run the component, then flush effects.
// Returns the native view handle (the host unwraps a chainable View for us).
export function __render() {
  syncNav();
  scope = rootScope;
  scope.idx = 0;
  const view = root();
  flushEffects();
  return unwrapView(view);
}

// Render one Navigator screen, under its OWN hook scope. Called by the host from
// inside the C navigator, so it runs during the same build as __render.
export function __screen(i) {
  const s = screens[i];
  if (!s) return null;

  const prev = scope;
  scope = s.scope;
  scope.idx = 0;
  try {
    const view = s.fn(s.props);
    flushEffects();
    return unwrapView(view);
  } finally {
    scope = prev;
  }
}

// A screen popped by the back gesture or the Escape key never goes through
// nav.pop(), so the C stack is the truth: if it shrank, those screens are gone
// and their hook state (and any cleanup they registered) goes with them.
function syncNav() {
  const depth = N.navDepth();
  while (screens.length > depth) {
    const dead = screens.pop();
    for (const c of dead.scope.cells) {
      if (c.v && typeof c.v.cleanup === "function") {
        try {
          c.v.cleanup();
        } catch (e) {
          console.error("effect cleanup threw: " + (e && e.message ? e.message : e));
        }
      }
    }
  }
}

function flushEffects() {
  const queued = effects;
  effects = [];
  for (const effect of queued) {
    try {
      effect.cell.cleanup = effect.fn() || null;
    } catch (e) {
      console.error("effect threw: " + (e && e.message ? e.message : e));
    }
  }
}

function unwrapView(view) {
  return view && view.h !== undefined ? view.h : view;
}

function cell(initial) {
  const s = scope;
  const i = s.idx++;
  if (s.cells.length <= i) {
    s.cells[i] = { v: typeof initial === "function" ? initial() : initial };
  }
  return s.cells[i];
}

// A process-wide identity for a hook cell, used to key retained C-side state (an
// animated value's spring cell) so it survives rebuilds without depending on call
// order. Monotonic, so two scopes can never collide.
let nextKey = 1;

// State: [value, setValue]. setValue takes a value or an updater, and schedules
// a rebuild only when the value actually changed (Object.is), so a handler that
// re-sets the same value costs nothing.
export function useState(initial) {
  const c = cell(initial);
  const set = (next) => {
    const value = typeof next === "function" ? next(c.v) : next;
    if (Object.is(value, c.v)) return;
    c.v = value;
    N.invalidate();
  };
  return [c.v, set];
}

// A mutable box that survives renders and does NOT trigger one when written.
export function useRef(initial) {
  const c = cell(() => ({ current: initial }));
  return c.v;
}

// Memoize a derivation across renders while `deps` are unchanged.
export function useMemo(fn, deps) {
  const c = cell(() => ({ deps: null, value: undefined, first: true }));
  const box = c.v;
  if (box.first || !sameDeps(box.deps, deps)) {
    box.value = fn();
    box.deps = deps ? deps.slice() : null;
    box.first = false;
  }
  return box.value;
}

export function useCallback(fn, deps) {
  return useMemo(() => fn, deps);
}

// Run a side effect after the tree is built, when `deps` change (omit deps to
// run every render, pass [] to run once). Return a function to clean up before
// the next run.
export function useEffect(fn, deps) {
  const c = cell(() => ({ deps: null, cleanup: null, first: true }));
  const box = c.v;
  if (box.first || !deps || !sameDeps(box.deps, deps)) {
    if (box.cleanup) {
      try {
        box.cleanup();
      } catch (e) {
        console.error("effect cleanup threw: " + (e && e.message ? e.message : e));
      }
      box.cleanup = null;
    }
    box.deps = deps ? deps.slice() : null;
    box.first = false;
    effects.push({ cell: box, fn });
  }
}

function sameDeps(a, b) {
  if (!a || !b || a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (!Object.is(a[i], b[i])) return false;
  }
  return true;
}

// --- motion ----------------------------------------------------------------

// A spring-backed scalar. It is RETAINED by the toolkit — unlike a view, it is
// meant to outlive the render that made it, which is how a spring stays in flight
// across the rebuilds it drives. Drive it from a handler; bind it to a subtree
// with .offset() (zelto/ui) and the frame loop animates it in C, with no
// per-frame setState from the script.
//
// Every motion verb takes a named token (Spring.standard / snappy / press), not
// raw stiffness numbers, so a script moves in the same language as the rest of
// the OS — and Reduce Motion collapses all of them to an instant jump inside the
// toolkit, so a script honours the accessibility setting without asking for it.
class Animated {
  constructor(handle) {
    this.h = handle;
  }
  get() { return N.animGet(this.h); }
  target() { return N.animTarget(this.h); }
  active() { return N.animActive(this.h); }

  set(to) { N.animSet(this.h, to); }        // jump
  pin(to) { N.animPin(this.h, to); }        // jump without waking the loop
  spring(to, token = N.SPRING_STANDARD) { N.animSpring(this.h, to, token); }

  // Take control of a (possibly mid-flight) spring: returns its current value and
  // stops it evolving, so a finger landing on a moving surface picks it up from
  // where it is instead of fighting it. Drive with .set(), release with .fling().
  grab() { return N.animGrab(this.h); }

  // Release a drag with the finger's velocity carried into the spring (px/s), so
  // the settle continues the gesture rather than easing from rest.
  fling(to, velocity, token = N.SPRING_STANDARD) {
    N.animSpringVelocity(this.h, to, token, velocity);
  }
}

// A spring cell that survives rebuilds, keyed by this hook's identity (not its
// call order). Use it for anything continuous — a drag, a sheet, a reveal —
// instead of a setState loop, which would rebuild the tree every frame.
export function useAnimatedValue(initial = 0) {
  const c = cell(() => ({ key: nextKey++ }));
  // Re-resolved every render: the C cell is looked up by key, and the handle must
  // be taken while the render is on the (per-screen) keyed table it belongs to.
  return new Animated(N.animatedValue(c.v.key, initial));
}

// --- text input ------------------------------------------------------------

// An editable buffer bound to the system on-screen keyboard. The buffer is the
// single source of truth: read `field.text` in the render, and the keyboard fills
// it — the app handles no keys at all. `onChange` is optional; the field always
// repaints itself on an edit.
export function useTextField(initial = "", onChange) {
  const c = cell(() => ({ id: N.fieldNew(initial), registered: false, handler: null }));
  const box = c.v;
  // Keep the LATEST closure, but register the native callback only once: the
  // handler is long-lived (it fires between builds), so re-registering it every
  // render would churn a registry slot per frame.
  box.handler = onChange || null;
  if (!box.registered) {
    box.registered = true;
    N.fieldOnChange(box.id, (text) => {
      if (box.handler) box.handler(text);
    });
  }

  return {
    id: box.id,
    get text() { return N.fieldText(box.id); },
    setText(value) { N.fieldSetText(box.id, String(value)); },
  };
}

// --- navigation ------------------------------------------------------------

// The navigation stack. `push` mounts a component as a new screen — with its own
// hook state — and slides it in; `pop` slides the top one back out. Back (the
// edge-swipe, or Escape) pops too, without going through here, so never assume
// pop() is the only way a screen leaves.
export const navigation = {
  push(component, props) {
    if (typeof component !== "function") {
      throw new TypeError("navigation.push(component, props): component must be a function");
    }
    screens.push({ fn: component, props, scope: newScope() });
    N.navPush(screens.length - 1);
  },

  // The C stack retires the screen only once its exit transition has finished (it
  // is still rendered while sliding out), so the JS mirror is NOT truncated here
  // — the next build's syncNav does it once the screen is really gone.
  pop() {
    N.navPop();
  },

  get depth() {
    return N.navDepth();
  },
};

// The hook form, for symmetry with the rest of the API (and the documented shape).
// The stack is process-wide, so this is just `navigation` — it takes no cell.
export function useNavigation() {
  return navigation;
}

// Force a rebuild (for state the framework can't see — a module-level cache).
export function invalidate() {
  N.invalidate();
}

// Close the app (the window disappears; the process exits).
export function quit() {
  N.quit();
}

// The app's current content size in logical px.
export function appSize() {
  return { width: N.appWidth(), height: N.appHeight() };
}
