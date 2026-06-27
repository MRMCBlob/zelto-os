# Run on a Device

Once an app works in the simulator, run it on real Zelto OS hardware (a
postmarketOS-class phone running Zelto). This page covers connecting, sideloading, and
testing — including APK interop, which only exists on device.

> To install Zelto OS itself on a phone, see
> [../contributing/porting-to-a-device.md](../contributing/porting-to-a-device.md).

## 1. Connect the device

Enable developer mode on the phone (Settings → System → Developer), then connect over
USB or the same network. Verify the link:

```sh
zelto devices
```

You should see your device listed. `zelto doctor` also reports device status.

## 2. Run directly

From an app directory:

```sh
zelto run --device            # build, install, launch on the connected device
zelto run --device --watch    # reinstall on source changes
```

Logs stream to your terminal; see [../tooling/debugging.md](../tooling/debugging.md) for
filtering and the remote inspector.

## 3. Install a package manually

```sh
zelto build --release         # produce the .zap
zelto install hello-1.0.0.zap --device
```

The package is verified (signature + manifest) and sandboxed on install. See
[../packaging/signing.md](../packaging/signing.md).

## 4. Testing APK interop

APK features (launching Android apps, share targets) only work on device, where the
Android container runs. Install a test APK:

```sh
zelto apk install some-app.apk        # installs into the Android container
zelto apk list
```

Then exercise interop from your app using the platform APIs in
[../platform/apk-interop.md](../platform/apk-interop.md).

## 5. On-device profiling

```sh
zelto profile --device --frames
```

Captures real frame timings, GPU time, and memory. See
[../tooling/profiling.md](../tooling/profiling.md).

## Troubleshooting

- **Device not found:** check USB/network, re-run `zelto doctor`, confirm developer mode.
- **Install rejected:** the package signature or manifest failed verification — see
  [../packaging/signing.md](../packaging/signing.md).
- **APK won't install:** the device's Android container may be disabled or unsupported on
  that port — see [../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md).
