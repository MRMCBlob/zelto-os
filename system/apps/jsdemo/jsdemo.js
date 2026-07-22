// JS Demo — the Zelto Script showcase app.
//
// The whole app is this file: no C, no build step. It exercises what a script app
// can reach, which is now what a C app can reach — hooks and timers, persistent
// storage, the gesture and motion primitives, the on-screen keyboard, navigation,
// and the four brokered system APIs (network, notifications, settings, intents),
// each of them going through the same zsysd consent flow a C app gets.
//
// The permission-gated calls all `await` their grant, so the consent modal is
// answered while the frame loop keeps running — a script never blocks the UI to
// ask the user a question.
import {
  useState, useEffect, useRef, useAnimatedValue, useTextField, navigation,
  appSize,
} from "zelto";
import {
  VStack, HStack, Text, WrapText, Button, Rect, Spacer, Scroll, TextField,
  Navigator, Color, Font, Weight, Align, Elevation, Spring, Pan,
} from "zelto/ui";
import * as storage from "zelto/storage";
import * as net from "zelto/net";
import * as notifications from "zelto/notifications";
import * as settings from "zelto/settings";
import * as intents from "zelto/intents";

const KEY = "jsdemo.count";
const NOTE = "jsdemo.note";

// The demo server the harness runs. On the device (QEMU, NET=1) the host is
// reachable through slirp's 10.0.2.2 alias; in the simulator the app runs
// natively and the server is on loopback, so the harness overrides this through
// the app's own prefs rather than the app special-casing where it is running. The
// MVP transport is plain HTTP to a numeric IPv4 host (no DNS, no TLS yet).
const ENDPOINT = () =>
  storage.get("jsdemo.endpoint", "http://10.0.2.2:8080/hello");

// The prose columns, DERIVED rather than guessed: the screen less the page's own
// padding, and less a card's padding again for prose inside one. P46's catalogue
// audit caught both paragraphs below painting through their right edge — a Text
// measures to one line however long it is — and a script app had no way to wrap
// until wrapText() was bound. Reading the width from appSize() is what keeps this
// from becoming another literal that is right at 720 and wrong everywhere else.
const PAGE_PAD = 20;
const CARD_PAD = 14;
const pageTextW = () => appSize().width - 2 * PAGE_PAD;
const cardTextW = () => appSize().width - 2 * PAGE_PAD - 2 * CARD_PAD;

function Card(title, body, tint) {
  return VStack({ spacing: 8, padding: CARD_PAD }, [
    Text(title).font(Font.footnote).color(Color.textMuted),
    body,
  ])
    .bg(tint)
    .radius(16)
    .shadow(Elevation.low);
}

// The app's root: it owns nothing but the navigation stack and the handlers that
// must be live from the moment the process starts. An intent or a notification
// action can be the REASON this app was launched, so those handlers are
// registered on the first render — the runtime replays anything that arrived
// first, so the app never misses the payload it was started for.
export default function App() {
  useEffect(() => {
    intents.onOpenUrl((url) => storage.set("jsdemo.lastUrl", url));
    intents.onShareTarget((items) => {
      if (items.length) storage.set(NOTE, items[0].text);
    });
    notifications.onAction((e) => {
      storage.set("jsdemo.lastAction", e.action || "(body)");
    });
    notifications.defineChannel("demo", "Demo", notifications.Importance.default);
  }, []);

  return Navigator(HomeScreen).bg(Color.bg);
}

function HomeScreen() {
  // Seeded from the store, so a relaunch picks up where the last one left off.
  const [count, setCount] = useState(() => storage.getNumber(KEY, 0));
  const [seconds, setSeconds] = useState(0);
  const [netState, setNetState] = useState("idle");
  const [notified, setNotified] = useState(null);
  const [muted, setMuted] = useState(() => settings.getBool(settings.Keys.mute));

  // A note typed on the on-screen keyboard, persisted. The buffer IS the state:
  // the keyboard writes into it and the app handles no keys at all.
  const note = useTextField(storage.get(NOTE, ""), (text) => storage.set(NOTE, text));

  // Direct manipulation: a spring the finger drives 1:1, then hands back with its
  // own velocity. Not a setState loop — the value lives in C and the frame loop
  // animates it.
  const dragX = useAnimatedValue(0);
  const grabbed = useRef(0);
  const [pinned, setPinned] = useState(false);

  useEffect(() => {
    storage.setNumber(KEY, count);
  }, [count]);

  useEffect(() => {
    const id = setInterval(() => setSeconds((s) => s + 1), 1000);
    return () => clearInterval(id);
  }, []);

  // Live system settings: the broker pushes every change, so flipping Mute in the
  // Settings app recolours this row without a poll. The observer also sees this
  // app's OWN writes echoed back — applying them idempotently is why that never
  // loops.
  useEffect(() => {
    settings.observe((key, value) => {
      if (key === settings.Keys.mute) setMuted(value === "1");
    });
    return () => settings.stop();
  }, []);

  const bump = (delta) => () => setCount((c) => Math.max(0, c + delta));

  // Permission-gated, and asynchronous end to end: the grant is awaited (the
  // consent modal runs on the live loop), then the request is driven by the app
  // loop's state machine. A denial rejects, so it lands in the same catch as a
  // dead socket.
  const doFetch = async () => {
    setNetState("fetching…");
    try {
      const res = await net.get(ENDPOINT());
      setNetState(res.ok ? `${res.status} · ${res.text().slice(0, 24)}`
                         : `HTTP ${res.status}`);
    } catch (e) {
      setNetState(`failed: ${e.message}`);
    }
  };

  const doNotify = async () => {
    try {
      const id = await notifications.post("JS Demo", `Counter is at ${count}.`, {
        channel: "demo",
        actionId: "ack",
        actionTitle: "Got it",
      });
      setNotified(id);
    } catch (e) {
      setNotified(-1);
    }
  };

  return Scroll(
    VStack({ spacing: 14, padding: PAGE_PAD }, [
      Text("JS Demo").font(Font.title).weight(Weight.bold).color(Color.text),
      WrapText("Zelto Script: gestures, motion, text input, and the system APIs.",
               pageTextW(), Font.subhead)
        .color(Color.textMuted),

      Card(
        "COUNTER (PERSISTED)",
        VStack({ spacing: 12 }, [
          Text(String(count))
            .font(Font.largeTitle)
            .weight(Weight.bold)
            .color(Color.accent),
          HStack({ spacing: 10 }, [
            Button("−1", bump(-1)).grow(1),
            Button("+1", bump(1)).grow(1),
          ]),
          Text(`${seconds}s since launch · reboot and the count is still here.`)
            .font(Font.caption)
            .color(Color.textFaint),
        ]),
        Color.surface2,
      ),

      // Gestures + motion. The card follows the finger exactly (grab -> set), then
      // springs home carrying the release velocity, so the settle continues the
      // gesture instead of easing from rest. Under Reduce Motion the spring
      // collapses to a jump — handled inside the toolkit, not here.
      Card(
        "DRAG ME (onPan + spring)",
        VStack({ spacing: 8 }, [
          HStack({ spacing: 10, align: Align.center }, [
            Rect({ color: pinned ? Color.success : Color.primary,
                   width: 10, height: 10, radius: 5 }),
            Text(pinned ? "Pinned — long-press again" : "Drag sideways; hold to pin")
              .font(Font.body)
              .color(Color.text),
          ])
            .padding(12)
            .bg(Color.surface3)
            .radius(12)
            .offset(dragX)
            .onPan((e) => {
              if (e.phase === Pan.begin) {
                grabbed.current = dragX.grab();
              } else if (e.phase === Pan.changed) {
                dragX.set(grabbed.current + e.dx);
              } else {
                dragX.fling(0, e.vx, Spring.standard);
              }
            })
            .onLongPress(() => setPinned((p) => !p)),
          WrapText("A tap, a drag and a hold on one view — the recognizer sorts "
                   + "them out.", cardTextW(), Font.caption)
            .color(Color.textFaint),
        ]),
        Color.surface,
      ),

      Card(
        "TEXT INPUT (on-screen keyboard)",
        VStack({ spacing: 8 }, [
          TextField(note, "Tap to type a note…"),
          Text("Saved as you type; shared text lands here too.")
            .font(Font.caption)
            .color(Color.textFaint),
        ]),
        Color.surface,
      ),

      Card(
        "SYSTEM APIS",
        VStack({ spacing: 10 }, [
          HStack({ spacing: 10 }, [
            Button("Fetch", doFetch).grow(1),
            Button("Notify", doNotify).grow(1),
          ]),
          HStack({ spacing: 10 }, [
            Button("Share", () => intents.shareText(note.text || "Hello from Zelto Script"))
              .grow(1),
            Button(muted ? "Unmute" : "Mute",
                   () => settings.set(settings.Keys.mute, !muted)).grow(1),
          ]),
          Text(`network · ${netState}`).font(Font.caption).color(Color.textMuted),
          Text(notified === null
                 ? "notifications · none posted"
                 : notified < 0
                   ? "notifications · denied"
                   : `notifications · posted #${notified}`)
            .font(Font.caption)
            .color(Color.textMuted),
          Text(`settings · sys.mute is ${muted ? "on" : "off"} (live)`)
            .font(Font.caption)
            .color(muted ? Color.warn : Color.textMuted),
        ]),
        Color.surface2,
      ),

      Button("Open details →", () => navigation.push(DetailScreen, { count })),
      Spacer(),
    ]),
  ).bg(Color.bg);
}

// A pushed screen: its own component, its own hook state, slid in by the C
// navigator. Back is the system's — the edge-swipe and Escape both pop it, so
// this screen must not assume its Back button is the only way out.
function DetailScreen(props) {
  const [note, setNote] = useState("");
  const reduceMotion = settings.getBool(settings.Keys.reduceMotion);

  return Scroll(
    VStack({ spacing: 14, padding: 20 }, [
      Text("Details").font(Font.title).weight(Weight.bold).color(Color.text),
      Text("A second screen, pushed onto the navigation stack — with hook state of its own.")
        .font(Font.subhead)
        .color(Color.textMuted),

      Card(
        "PROPS FROM THE PUSH",
        Text(`The counter was ${props.count} when you opened this.`)
          .font(Font.callout)
          .color(Color.text),
        Color.surface2,
      ),

      Card(
        "LOCAL STATE",
        VStack({ spacing: 10 }, [
          Text(note ? `You picked: ${note}` : "Nothing picked yet.")
            .font(Font.body)
            .color(Color.text),
          HStack({ spacing: 10 }, [
            Button("Alpha", () => setNote("Alpha")).grow(1),
            Button("Beta", () => setNote("Beta")).grow(1),
          ]),
          Text("Pop and push again: this resets, the Home screen behind it does not.")
            .font(Font.caption)
            .color(Color.textFaint),
        ]),
        Color.surface,
      ),

      Card(
        "ACCESSIBILITY",
        Text(`Reduce Motion is ${reduceMotion ? "ON — springs are jumps" : "off"}.`)
          .font(Font.body)
          .color(reduceMotion ? Color.warn : Color.textMuted),
        Color.surface,
      ),

      Button("← Back", () => navigation.pop()),
      Text("Or swipe from the left edge — the system gesture pops it too.")
        .font(Font.caption)
        .color(Color.textFaint),
      Spacer(),
    ]),
  ).bg(Color.bg);
}
