# Porting to a Device

Bringing Zelto OS up on a phone. Zelto rides on **postmarketOS**, so porting is mostly:
get postmarketOS booting with a suitable kernel, then layer Zelto's compositor, SDK,
services, and Waydroid on top.

## Realistic targets

Best first targets are devices with **mainline (or mainline-leaning) Linux** support and
known Waydroid compatibility:

- **PinePhone / PinePhone Pro**
- **Librem 5**
- postmarketOS devices with a mainline kernel and working GPU (Mesa)

Devices with only a downstream/vendor kernel are harder and may lack GPU or Waydroid
support. Set expectations accordingly ([../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md)).

## Steps

### 1. Get postmarketOS booting

Use `pmbootstrap` to build and flash a base image for the device. Confirm it boots to a
console and has working storage, USB, and ideally Wi-Fi.

### 2. Kernel configuration

Zelto needs:

- **GPU / DRM-KMS** working with Mesa (for `zcomp`).
- **libinput** input devices (touch).
- **Android container** support for APKs:
  - `CONFIG_ANDROID_BINDER_IPC=y`, `CONFIG_ANDROID_BINDERFS=y`
  - `ashmem` or a `memfd` shim

Patch the device kernel config in `device/<device>/` ([repo-layout.md](repo-layout.md)).

### 3. Bring up the compositor

Verify `zcomp` drives the display and input on the device:

```sh
zcomp --drm        # run directly on DRM/KMS (not nested)
```

Debug display/input issues here before adding the shell
([compositor-internals.md](compositor-internals.md)).

### 4. Add the shell + services

Enable the System UI, `zsysd`, and the session so the device boots into the Zelto shell.
Verify launching native apps end-to-end.

### 5. Waydroid + APKs

Install/initialize Waydroid against the device kernel and confirm an APK renders into
`zcomp`. Wire the **APK bridge** ([services-and-ipc.md](services-and-ipc.md)).

### 6. Hardware passthrough

Camera, sensors, GPS, modem, and audio depend on device HALs and the Waydroid bridges.
Enable and test each; document what works in the device's port notes.

## Port status matrix

Track per-device support so users know what to expect:

| Capability | PinePhone | … |
|---|---|---|
| Display / GPU | ✅ | |
| Touch | ✅ | |
| Wi-Fi / cellular | ⚠️ | |
| APKs (Waydroid) | ⚠️ | |
| Camera / sensors / GPS | ⚠️ | |

(✅ works · ⚠️ partial · ❌ unsupported — fill in per port.)

## Contributing a port

Add `device/<vendor>-<model>/` with the kernel config, device-specific tweaks, and a
`PORT.md` status page; submit with the matrix above filled in.

## See also

- [build-zelto-os.md](build-zelto-os.md) · [../overview/how-it-runs-apks.md](../overview/how-it-runs-apks.md)
