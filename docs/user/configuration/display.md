# Display, Language and Time

Settings for the screen itself: brightness, sleep, rotation, size, and the clock. All of these are **per screen**, not per printer. Rows you can change from the touchscreen name their menu path. See [Settings: Display](../guide/settings/display.md) and [Settings: Language & Time](../guide/settings/language-time.md) for the walkthroughs.

[Back to the Configuration Reference](../CONFIGURATION.md)

Display keys live in the `display` section unless the table says otherwise.

---

## Brightness and sleep

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `brightness` (top level) | `80` | `10` to `100` | Screen brightness in percent. Set it with the slider at **Settings > Display > Brightness**, which is shown only on screens whose backlight HelixScreen can control. |
| `dim_sec` | `600` | `0`, `30`, `60`, `120`, `300`, `600` | Seconds of inactivity before the screen dims. `0` never dims. Set it at **Settings > Display > Screen Dim**. A value that is not on this list snaps to the nearest one. |
| `dim_brightness` | `30` | `1` to `100` | Brightness percentage while dimmed. |
| `sleep_sec` | `1200` | `0`, `60`, `300`, `600`, `1200`, `1800`, `3600` | Seconds of inactivity before the screen turns off. `0` never sleeps. Set it at **Settings > Display > Display Sleep**. A value that is not on this list snaps to the nearest one. |
| `sleep_while_printing` | `true` | `true` or `false` | Whether the screen may dim and sleep during a print. Set `false` to keep it lit for the whole print. **Settings > Display > Sleep While Printing**. |

Dimming starts the screensaver at the same moment, so while a screensaver is selected the **Screen Dim** row is called **Screensaver after**. On a screen with no backlight control (an Android phone, for example) the screensaver is the only idle sign, and the row appears only while a screensaver is selected.

### Backlight behaviour on unusual panels

You only need these if your screen misbehaves at sleep or at low brightness. Each takes effect after a restart.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `sleep_backlight_off` | `true` | `true` or `false` | Whether sleep switches the backlight off, rather than only dimming it. Set `false` if your screen goes dark at sleep and a touch does not bring it back. |
| `backlight_enable_ioctl` | `true` | `true` or `false` | Whether HelixScreen also uses the kernel's backlight enable and disable call. A few panels wire this backwards, so the screen is dark when it should be lit. Set `false` so brightness alone controls it. |
| `backlight_floor_percent` | `0` (`20` on K2 builds with a sysfs backlight) | `0` to `100` | The lowest brightness the panel can still show, as a percentage of its raw range. The brightness slider's minimum lands on the floor. Raise it if your panel goes black before the slider reaches its minimum. `0` turns the floor off. Stock K2 firmware drives its panel differently and takes no default floor. A value you set applies to every kind of backlight. |
| `panel_power_off` | `-1` | `-1` automatic, `0` never, `1` always | Whether sleep powers the panel down or only turns the backlight off. Automatic powers it down only when the screen has no brightness control. Use `1` if the screen goes black but the backlight stays lit. Use `0` if the screen flashes colours, glows at the edges or fails to wake. See [Troubleshooting](../TROUBLESHOOTING.md#screen-goes-dark-at-sleep-but-the-backlight-stays-on). |

---

## Screensaver

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `screensaver_type` | `"flipclock"` | `"off"`, `"toasters"`, `"starfield"`, `"pipes"`, `"bounce"`, `"fireworks"`, `"logo"`, `"maze"`, `"hyperspace"`, `"flipclock"`, `"fireflies"`, `"aqua"`, `"highfly"`, `"nixie"`, `"random"` | Which screensaver plays while the screen is idle. Set it at **Settings > Display > Screensaver**. |
| `screensaver_print_progress` | `true` | `true` or `false` | Whether screensavers without a print display of their own (all but the Flip Clock, Aqua Surface and High Fly) show a small progress card during a print. **Settings > Display > Show Print Progress**. A paused or finished print shows the card either way. |
| `screensaver_levels` | absent | written by HelixScreen | Do not edit. |

The names correspond to the menu entries Flying Toasters, Starfield, 3D Pipes, Bouncing Printer, Fireworks, Bouncing Logo, 3D Maze, Hyperspace, Flip Clock, Fireflies, Aqua Surface and High Fly. `"random"` plays a different one each time. The names are not case-sensitive. A value that names no screensaver reads as the default, and an old numeric value is still read as the screensaver it stood for and is rewritten as a name on the next update. Not every screensaver is available on every build, and the default is the Flip Clock wherever it is available.

The screensaver starts at the `dim_sec` mark and stops when `sleep_sec` turns the display off.

Each screensaver measures how much processor time it uses on your hardware. If it would slow the printer it lowers its frame rate or detail, and steps back up once it has run comfortably for a while. A screensaver that is still too heavy at its lowest setting is skipped and another plays in its place, or, during a print, the Flip Clock, or a plain black screen if none fits. Your choice stays saved either way. The measurements are kept in `screensaver_levels` and are redone after an update or on different hardware. Delete that entry to have every screensaver measured again.

---

## Rotation, size and layout

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `rotate` | `0` | `0`, `90`, `180`, `270` | Rotates the whole display, and touch follows. **Settings > Display > Screen Rotation**, applied after a restart. The row is hidden on the Linux desktop build. |
| `rotation_probed` | `false` | `true` or `false` | Set to `true` once automatic rotation detection has run. Remove this key and `rotate` to run detection again. |
| `ui_scale_percent` | `0` (Automatic) | `0`, or `100` to `200` | The size of the whole interface. `0` works the size out from the panel's pixel density, which leaves every supported printer at 100 and only enlarges very sharp screens such as a phone. The menu at **Settings > Display > UI Scale** offers Automatic, 100%, 125%, 150%, 175% and 200%. A value outside the range reads as Automatic. Applies after a restart. |
| `layout` | `"auto"` | `auto`, `standard`, `ultrawide`, `portrait`, `micro`, `micro-portrait`, `tiny`, `tiny-portrait` | Overrides the detected screen layout. Leave it at `auto` unless you are testing. |

**Automatic rotation.** On the first start HelixScreen asks the kernel for the panel's orientation (for example `panel_orientation=upside_down`) and, for boards whose mounting is known, uses that. If it finds one it applies and saves the rotation with no prompt. On framebuffer displays that give no hint, such as the AD5M, a tap-to-confirm rotation screen runs instead. Raspberry Pi displays do not run that screen.

**Rotation on a Raspberry Pi and other DRM displays.** HelixScreen rotates by any angle. On a panel whose hardware cannot rotate, it may switch itself to a software-rotating display path, which redraws the whole screen each frame (typically under 1 ms on a Pi 5). To avoid that cost, rotate the panel in the kernel instead with a `video=...,rotate=90` parameter on the kernel command line. The app then draws unrotated. Framebuffer displays (AD5M, K1, K2, CC1, AD5X) rotate by any angle with no meaningful cost.

**Layout detection.** HelixScreen picks the layout from the display's aspect ratio: wider than about 2.5:1 is `ultrawide`, narrower than about 0.8:1 is `portrait`, anything between is `standard`. Displays whose longest side is 480 pixels or less get `micro` (the 480x272 class) or `tiny`, with `-portrait` variants when the screen is taller than wide.

> **Ultrawide and portrait are alpha.** Detection, navigation bar and grid sizing work, and both get their own home dashboard. Portrait also rearranges Print Status, Print Tune, Motion, Bed Mesh, the temperature graph and the Advanced panel's E-stop bar. Ultrawide has the home dashboard and nothing else, so other panels fall back to the standard landscape layout and may look stretched or clipped. Neither has had much testing on real hardware. Forcing one is for people contributing layouts, not for daily use.

`drm_device` (default `""`, auto-detect) overrides which display device HelixScreen draws to, for example `/dev/dri/card1`. On a Raspberry Pi 5, `card0` is the 3D chip with no display output, `card1` is the DSI touchscreen and `card2` is HDMI. Auto-detection picks the first device with a connected display. The same override exists as the `HELIX_DRM_DEVICE` variable (see [Environment variables](environment.md)).

---

## Language, clock and time zone

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `language` (top level) | `"en"` | `"en"`, `"de"`, `"fr"`, `"es"`, `"ru"`, `"pt"`, `"it"`, `"zh"`, `"ja"` | The language: English, German, French, Spanish, Russian, Portuguese, Italian, Chinese and Japanese. ESP32 screens such as the BTT K-Touch offer the first seven only. **Settings > Language & Time > Language**. |
| `time_format` | `0` | `0` 12-hour, `1` 24-hour | Whether the time reads "2:30 PM" or "14:30". **Settings > Language & Time > Time Format**. |
| `timezone` | absent | an IANA name such as `"America/New_York"` | The time zone for every clock and print-time estimate. When absent, HelixScreen follows the system's time zone (the **System** entry in the list). The BTT K-Touch has no system time zone, so it uses UTC until you pick one; its Wi-Fi setup page offers your phone's. **Settings > Language & Time > Timezone**. |
