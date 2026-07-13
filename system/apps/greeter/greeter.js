// Greeter — a Zelto Script app that ships as a signed .zap.
//
// Its whole point is the packaging path: this file is not installed into the
// image. It is packaged (meta/mkzap.sh), signed, verified and unpacked by
// zelto-install onto the persistent partition, and only then does a tile for it
// appear on Home. So everything below runs from /var/zelto, from code whose hash
// the installer checked against a signature before it ever ran.
//
// It carries no binary: a script package rides the shared runtime that is already
// on the device, which is why the same .zap installs on the aarch64 phone and in
// the x86_64 simulator.
import { useState, useEffect } from "zelto";
import {
  VStack, HStack, Text, Button, Rect, Spacer, Scroll,
  Color, Font, Weight, Align, Elevation,
} from "zelto/ui";
import * as storage from "zelto/storage";

const KEY = "greeter.launches";

export default function App() {
  // Its own private store under /var/zelto/apps/os.zelto.greeter — counted up on
  // each launch, so a reboot proves the install (and the data) really persisted.
  const [launches] = useState(() => {
    const n = storage.getNumber(KEY, 0) + 1;
    storage.setNumber(KEY, n);
    return n;
  });
  const [taps, setTaps] = useState(0);

  useEffect(() => {
    console.log(`greeter: launch #${launches}`);
  }, []);

  return Scroll(
    VStack({ spacing: 16, padding: 24 }, [
      Text("Greeter").font(Font.largeTitle).weight(Weight.bold).color(Color.text),
      Text("Installed from a signed .zap — no binary, just JavaScript.")
        .font(Font.subhead)
        .color(Color.textMuted),

      VStack({ spacing: 10, padding: 16 }, [
        Text("VERIFIED ON INSTALL").font(Font.footnote).color(Color.textMuted),
        HStack({ spacing: 10, align: Align.center }, [
          Rect({ color: Color.success, width: 10, height: 10, radius: 5 }),
          Text("Ed25519 signature + per-file SHA-256")
            .font(Font.body)
            .color(Color.text),
        ]),
        Text(`Launch #${launches} from /var/zelto`)
          .font(Font.caption)
          .color(Color.textFaint),
      ])
        .bg(Color.surface2)
        .radius(16)
        .shadow(Elevation.low),

      VStack({ spacing: 12, padding: 16 }, [
        Text("AND IT IS A REAL APP").font(Font.footnote).color(Color.textMuted),
        Text(String(taps))
          .font(Font.largeTitle)
          .weight(Weight.bold)
          .color(Color.accent),
        Button("Tap me", () => setTaps((t) => t + 1)),
      ])
        .bg(Color.surface)
        .radius(16),

      Spacer(),
    ]),
  ).bg(Color.bg);
}
