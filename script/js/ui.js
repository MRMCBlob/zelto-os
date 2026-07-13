// The `zelto/ui` module: the declarative toolkit, as seen from Zelto Script.
//
// Every constructor returns a chainable View — the JS answer to the C toolkit's
// wrapper modifiers (Padding(8, Frame(...))), which read inside-out. Here they
// read in order:
//
//   Text("Hi").font(Font.title).color(Color.accent).padding(8)
//
// The chain mutates the underlying node in place (each native modifier returns
// the same node), so it costs nothing per link. A View is only valid during the
// render that made it; stash state, not views.
import * as N from "zelto:native";
import { __navRoot } from "zelto";

class View {
  constructor(handle) {
    this.h = handle;
  }
  padding(px) { N.padding(this.h, px); return this; }
  frame(width, height) { N.frame(this.h, width, height); return this; }
  bg(color) { N.background(this.h, color); return this; }
  color(color) { N.foreground(this.h, color); return this; }
  radius(px) { N.cornerRadius(this.h, px); return this; }
  font(size) { N.font(this.h, size); return this; }
  weight(w) { N.weight(this.h, w); return this; }
  grow(weight = 1) { N.grow(this.h, weight); return this; }
  opacity(amount) { N.opacity(this.h, amount); return this; }
  shadow(elevation = N.ELEV_2) { N.shadow(this.h, elevation); return this; }
  fill() { N.fill(this.h); return this; }
  cover() { N.cover(this.h); return this; }
  textShadow() { N.textShadow(this.h); return this; }
  onTap(fn) { N.onTap(this.h, fn); return this; }

  // Drag. `fn` gets { x, y, dx, dy, vx, vy, phase } — dx/dy are the translation
  // since the gesture began, vx/vy the finger velocity in px/s (what you hand to
  // .fling() on release). Inside a Scroll a vertical drag already scrolls; onPan
  // is for custom drags (swipe-to-dismiss, sliders, sheets).
  onPan(fn) { N.onPan(this.h, fn); return this; }

  // Press-and-hold. Composes with onTap on the same subtree: a quick release is
  // still a tap, a move past the slop becomes a pan instead, and a hold past the
  // threshold fires this and suppresses the tap. `fn` gets { x, y }.
  onLongPress(fn) { N.onLongPress(this.h, fn); return this; }

  // Bind an animated value to this subtree's horizontal translation (y is static).
  offset(animatedX, y = 0) {
    N.offset(this.h, animatedX ? animatedX.h : null, y);
    return this;
  }

  // Static 2-D placement, for an absolutely-positioned cell inside a depth stack.
  offsetXY(x, y) { N.offsetXY(this.h, x, y); return this; }

  // Animate BOTH axes (either may be null to leave that axis alone).
  offsetXYAnimated(animatedX, animatedY) {
    N.offsetXYAnimated(this.h, animatedX ? animatedX.h : null,
                       animatedY ? animatedY.h : null);
    return this;
  }
}

function unwrap(v) {
  return v instanceof View ? v.h : v;   // null/false pass through and are skipped
}

// Children may be listed positionally or as one array; an options object may
// lead. VStack({ spacing: 8 }, [a, b]) === VStack(a, b) with defaults.
function stack(axis) {
  return (...args) => {
    let opts = {};
    if (args.length && isOptions(args[0])) opts = args.shift();
    const kids = args.length === 1 && Array.isArray(args[0]) ? args[0] : args;
    return new View(N.stack(axis, kids.map(unwrap), opts));
  };
}

function isOptions(v) {
  return v && typeof v === "object" && !(v instanceof View) && !Array.isArray(v);
}

export const VStack = stack(N.AXIS_VERTICAL);
export const HStack = stack(N.AXIS_HORIZONTAL);
export const ZStack = stack(N.AXIS_DEPTH);

export function Text(value) {
  return new View(N.text(String(value)));
}

export function Rect(opts = {}) {
  return new View(N.rect(opts));
}

export function Spacer() {
  return new View(N.spacer());
}

export function Image(path) {
  return new View(N.image(path));
}

// True if `path` decodes (cached) — probe before Image() when you want to draw
// a fallback instead of nothing.
export function imageLoads(path) {
  return N.imageLoads(path);
}

// A scrollable viewport around one child (momentum + rubber-band come free).
export function Scroll(content, axis = N.AXIS_VERTICAL) {
  return new View(N.scroll(unwrap(content), axis));
}

// An editable field bound to the system on-screen keyboard. Pass the handle from
// useTextField (zelto): tapping it focuses the field and raises the keyboard, and
// every key lands in the buffer with no key handling in the app.
export function TextField(field, placeholder = "") {
  return new View(N.textField(field.id, placeholder));
}

// The navigation stack, rendered. `rootScreen` is the component shown at the
// bottom of the stack — it must NOT be the app's own root component (the one that
// renders this Navigator), or rendering the stack would re-enter it forever. Push
// more screens with `navigation.push` (zelto); Back — the edge-swipe or Escape —
// is the system's, and pops without asking.
export function Navigator(rootScreen) {
  if (typeof rootScreen !== "function") {
    throw new TypeError("Navigator(rootScreen): rootScreen must be a component");
  }
  __navRoot(rootScreen);
  return new View(N.navigator());
}

// The filled, rounded, tappable control — the same recipe as the C toolkit's
// Button (a padded stack + an inverted label), so the two look identical.
export function Button(label, onTap) {
  return HStack({ padding: 14, align: N.ALIGN_CENTER }, [
    Text(label).color(N.COLOR_TEXT_INV).weight(N.WEIGHT_MEDIUM),
  ])
    .bg(N.COLOR_PRIMARY)
    .radius(12)
    .onTap(onTap);
}

// --- Design tokens ---------------------------------------------------------
// Re-exported straight from gfx.h (via the native module), so the palette has
// exactly one source of truth.

export const Color = {
  bg: N.COLOR_BG,
  surface: N.COLOR_SURFACE,
  surface2: N.COLOR_SURFACE_2,
  surface3: N.COLOR_SURFACE_3,
  border: N.COLOR_BORDER,
  primary: N.COLOR_PRIMARY,
  accent: N.COLOR_ACCENT,
  text: N.COLOR_TEXT,
  textMuted: N.COLOR_TEXT_MUTED,
  textFaint: N.COLOR_TEXT_FAINT,
  textInv: N.COLOR_TEXT_INV,
  success: N.COLOR_SUCCESS,
  warn: N.COLOR_WARN,
  danger: N.COLOR_DANGER,
  successDim: N.COLOR_SUCCESS_DIM,
  warnDim: N.COLOR_WARN_DIM,
  dangerDim: N.COLOR_DANGER_DIM,
  accentDim: N.COLOR_ACCENT_DIM,
};

// An ad-hoc colour: rgba(0x4a, 0xa3, 0xff) — prefer a token, but a chart or an
// app's own brand hue needs the escape hatch.
export function rgba(r, g, b, a = 255) {
  return (((r & 255) << 24) | ((g & 255) << 16) | ((b & 255) << 8) | (a & 255)) >>> 0;
}

export const Font = {
  caption2: N.FONT_CAPTION2,
  caption: N.FONT_CAPTION,
  footnote: N.FONT_FOOTNOTE,
  subhead: N.FONT_SUBHEAD,
  body: N.FONT_BODY,
  headline: N.FONT_HEADLINE,
  callout: N.FONT_CALLOUT,
  title2: N.FONT_TITLE2,
  title: N.FONT_TITLE,
  largeTitle: N.FONT_LARGE_TITLE,
};

export const Weight = {
  regular: N.WEIGHT_REGULAR,
  medium: N.WEIGHT_MEDIUM,
  semibold: N.WEIGHT_SEMIBOLD,
  bold: N.WEIGHT_BOLD,
};

export const Align = {
  leading: N.ALIGN_LEADING,
  center: N.ALIGN_CENTER,
  trailing: N.ALIGN_TRAILING,
};

export const Axis = {
  vertical: N.AXIS_VERTICAL,
  horizontal: N.AXIS_HORIZONTAL,
  depth: N.AXIS_DEPTH,
};

export const Elevation = {
  low: N.ELEV_1,
  medium: N.ELEV_2,
  high: N.ELEV_3,
};

// Motion tokens (P31): pick a spring by ROLE, never by tuning numbers, so the
// whole system moves in one language. Reduce Motion collapses every one of them
// to an instant jump, inside the toolkit.
export const Spring = {
  standard: N.SPRING_STANDARD,   // the default settle: sheets, drawers, slides
  snappy: N.SPRING_SNAPPY,       // stiffer, quicker: decisive moves
  press: N.SPRING_PRESS,         // tight + fast: touch feedback
};

// The phase of an onPan event.
export const Pan = {
  begin: N.PAN_BEGIN,
  changed: N.PAN_CHANGED,
  end: N.PAN_END,
};
