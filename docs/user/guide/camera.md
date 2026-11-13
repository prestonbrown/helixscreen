# Camera

If your printer has a webcam set up in Moonraker, HelixScreen can show the live feed as a home-screen widget and in a fullscreen viewer. You can choose which camera a widget shows, and rotate or flip the image to match how the camera is mounted.

![Fullscreen Camera Viewer](../../images/user/camera.png)

---

## Viewing the Camera

- **Home widget:** long-press the home screen to enter edit mode, tap the **+** button, and add the **Camera** widget from the **Print** category. It shows the live feed in place. Tap it to open the fullscreen viewer.
- **Fullscreen viewer:** open **Settings > Devices > Camera** ("View live camera feed"). The row only appears when Moonraker has an enabled webcam. Tap the **X** at the top right to close. While a print is running or paused, a red E-stop button sits at the top left, because the viewer covers the navigation bar.
- **During a print:** when HelixScreen runs on a separate screen or device from the printer (a Raspberry Pi, a desktop, a phone) and a webcam is detected, the print screen offers the camera in two ways:
  - The **camera button** in the corner of the preview card swaps the thumbnail or G-code render for the live feed in the same spot. Tap it again to go back. HelixScreen remembers the choice for the next print (the [`print_status_show_camera`](../CONFIGURATION.md#print_status_show_camera) setting, off by default). If the webcam goes away, the card shows the preview again. The feed also steps aside while the exclude-object map is open.
  - **More > Camera** in the print screen opens the fullscreen viewer.

The feed is shown as an MJPEG stream when the camera offers one. If only a snapshot address is available, HelixScreen fetches a new picture every two seconds instead. Cameras on an `https` address work too, though the Android app polls their snapshots rather than streaming them.

The **HelixScreen Screen** webcam that [Screen Sharing](screen-sharing.md) adds to Mainsail and Fluidd is never offered as a camera, since it shows the printer's screen rather than the printer.

---

## Printers Without a Camera View

HelixScreen running on the printer's own screen is built without the camera view on these printers:

- Creality K1, K1C, K1 Max, K1 SE and Ender-3 V3 KE
- Creality K2 series
- FlashForge Adventurer 5M, Adventurer 5X and Creator 5
- Elegoo Centauri Carbon
- Snapmaker U1

On these builds there is no Camera widget, no camera entry under **Settings > Devices**, and no camera on the print screen, even when Moonraker has a webcam. Your camera still works in Mainsail, Fluidd and other clients, and a separate screen pointed at the same printer shows it as usual. These boards share a small amount of memory and two slow processor cores with Klipper, so the live view is left out.

The Spoolman QR scanner is the exception: it still uses the printer's webcam for a single picture. The picture in the [Print issue detected](print-monitoring.md#what-happens-on-a-detection) dialog and the camera photos in [Print History](print-history.md) need the camera view, so these builds do not show them.

---

## Choosing Between Several Cameras

When Moonraker lists more than one enabled webcam, HelixScreen picks one automatically: the first camera whose service is an MJPEG streamer (such as mjpegstreamer or ustreamer) and that is running, otherwise the first camera with a usable snapshot address. That automatic choice is what the fullscreen viewer and the QR scanner always show.

A home widget can show a specific camera instead. In edit mode, tap the gear icon on the Camera widget and pick one under **Source**. The list only appears when there is more than one choice:

- **Automatic:** the camera HelixScreen would pick on its own.
- A named camera, one of Moonraker's enabled webcams. A note beside the name says what you get: **Snapshot only** for a camera whose service is not MJPEG (WebRTC, HLS, an IP camera), which HelixScreen polls rather than streams, and **Unavailable** for one that is not running.

The choice is saved by camera name, so it survives a change of the printer's address. If that camera disappears, is disabled or stops running, the widget shows the automatic choice instead of **No Camera**, and goes back to the named camera when it returns. Each widget has its own Source, so two Camera widgets can show two feeds. Each is a separate stream, so expect extra CPU use on small boards. Many camera services serve only one viewer, so two places showing the same camera take turns.

---

## Stream Status

While the camera connects or cannot be reached, the widget shows a message over a spinner:

| Message | Meaning |
|---------|---------|
| **Connecting Camera...** | HelixScreen is opening the stream. It clears when the first picture arrives |
| **No Camera** | No webcam is configured, or the stream could not be reached |
| **Snapshot unavailable** | Three snapshot requests in a row failed or could not be read. Check the camera's snapshot address in Mainsail or Fluidd |
| **Camera unavailable** | The webcam has neither a stream nor a snapshot address HelixScreen can use |

When the widget is only one column wide, it shows an icon and does not stream. Make it wider to see the live feed.

---

## Rotation & Flip

To correct a camera that is mounted upside down or mirrored, enter edit mode on the home screen and tap the gear icon on the Camera widget. The **Camera** dialog has:

- **Source:** which camera to show (see above)
- **Rotation:** 0°, 90°, 180° or 270°
- **Flip:** **Horizontal** and **Vertical**, either or both

Tap **Save** to apply or **Cancel** to discard. The settings are saved with the widget and survive restarts.

---

## Performance Notes

HelixScreen limits the camera to keep the screen responsive:

- The stream runs at the frame rate set for the webcam in Moonraker, or 15 frames per second when none is set.
- While another screen covers the widget, the stream pauses and resumes when you return.
- In home-screen edit mode the frame rate drops to 2 so editing stays smooth.
- When the display goes to sleep, the stream stops and restarts on wake.

---

**Next:** [Print History](print-history.md) | **Prev:** [Security & Screen Lock](security.md) | [Back to User Guide](../USER_GUIDE.md)
