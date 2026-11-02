# Camera

If your printer has a webcam configured in Moonraker, HelixScreen can show the live feed both as a home-dashboard widget and as a standalone fullscreen viewer. You can rotate and flip the image to match how your camera is mounted.

![Fullscreen Camera Viewer](../../images/user/camera.png)

---

## Viewing the Camera

There are two ways to see the feed:

- **Home widget** — add the Camera widget from the Home dashboard's edit mode (long-press the dashboard, then tap Add Widget). It shows the live feed inline. Tap it to expand into the fullscreen viewer.
- **Standalone fullscreen viewer** — open **Settings > Hardware & Devices > Camera** to view the live feed fullscreen without adding a widget. This entry only appears when a webcam is detected (an enabled webcam configured in Moonraker).

The feed is decoded as an MJPEG stream when one is available. If only a snapshot URL is configured, HelixScreen falls back to periodically polling that snapshot image instead.

---

## Printers Without a Camera View

HelixScreen running on the printer's own screen is built without the camera view on these printers:

- Creality K1, K1C, K1 Max, K1 SE and Ender-3 V3 KE
- Creality K2 series
- FlashForge Adventurer 5M, Adventurer 5X and Creator 5
- Elegoo Centauri Carbon
- Snapmaker U1

On these builds there is no Camera widget and no camera entry under **Settings**, even when Moonraker has a webcam configured. Your camera still works in Mainsail, Fluidd and other clients. A separate screen or device pointed at the same printer (a Raspberry Pi, a desktop, the Android app) shows the camera as usual.

These boards share a small amount of memory and two slow processor cores with Klipper, and the camera view has not been tuned and measured on them yet. A lighter camera view for these printers is planned.

---

## Stream Status

While the camera is connecting or unavailable, the widget shows a status message over a spinner:

| Status | Meaning |
|--------|---------|
| **Connecting Camera…** | HelixScreen is establishing the stream; the status clears once the first frame arrives |
| **No Camera** | No webcam is configured, or the stream couldn't be reached |

At the smallest widget size (1x1) the camera shows only an icon and does not stream — make the widget larger to see the live feed.

---

## Rotation & Flip

To correct a camera that's mounted upside-down or mirrored, open the camera configuration:

1. Enter Home dashboard **edit mode**.
2. Tap the gear icon on the Camera widget.

The configuration dialog offers:

- **Rotation** — 0°, 90°, 180°, or 270°
- **Flip** — Horizontal and/or Vertical

Tap **Save** to apply, or **Cancel** to discard. The transform is saved with the widget so it persists across restarts.

---

## Performance Notes

HelixScreen throttles the camera stream to keep the UI responsive and save resources:

- The stream runs at the frame rate configured in Moonraker (defaulting to 15 fps if not specified).
- While another overlay is covering the widget, the stream is **paused** and resumes when the overlay closes.
- During Home dashboard edit mode, the frame rate is **reduced** so editing stays smooth.
- When the display goes to sleep, the camera stream **stops** entirely and restarts on wake.

---

**Next:** [Print History](print-history.md) | **Prev:** [Security & Screen Lock](security.md) | [Back to User Guide](../USER_GUIDE.md)
