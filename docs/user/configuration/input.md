# Touch and Input

Settings for how the screen reacts to your finger, to a keyboard and to a barcode scanner. All of these are **per screen**. They live in the `input` section unless the table says otherwise. See [Settings: Touch & Input](../guide/settings/touch-input.md) and the [Touch Calibration guide](../guide/touch-calibration.md) for the walkthroughs.

[Back to the Configuration Reference](../CONFIGURATION.md)

> **Tuning touch feel.** `scroll_throw` and `scroll_limit` interact. The [troubleshooting guide](../TROUBLESHOOTING.md#touch-feel--which-setting-do-i-tune) maps each symptom to the setting that fixes it.

---

## Scrolling and pressing

| Key | Default | Range | What it does |
|-----|---------|-------|--------------|
| `scroll_throw` | `25` (`35` on ESP32 screens such as the BTT K-Touch) | `5` to `50` | How quickly a flicked list coasts to a stop. Higher values stop sooner, with less coasting. Lower it if lists feel sticky at the end of a flick. There is no on-screen control. |
| `scroll_limit` | `10` | `1` to `20` | Pixels your finger must move before a gesture counts as a scroll rather than a tap. Lower values start scrolling sooner and stop phantom clicks when you scroll a list with a short, slow swipe. Higher values need a more deliberate gesture, which reduces accidental scrolls but makes short scrolls feel unresponsive. **Settings > Touch & Input > Scroll Engage Distance**, applied at once. |
| `long_press_time` | `500` | `300` to `1500` | Milliseconds a finger must hold before a press counts as a long press. It governs every long press in the app, such as entering Home screen edit mode, deleting a file card and editing a macro. Raise it if long-press actions fire when a finger merely rests on the glass, which is common on a tablet lying flat. **Long Press Time**, applied at once. |
| `home_edit_mode_enabled` | `true` | `true` or `false` | Whether a long press on the Home grid starts Edit Mode, the layout editor. With `false` the long press does nothing. **Allow Home Screen Editing**, applied at once. |
| `keypad_layout` | `0` | `0` phone, `1` calculator | The digit order of the number pad: phone puts 1 2 3 on top, calculator puts 7 8 9 on top. **Number Pad Layout**, applied at once. |
| `scroll_guard` | `false` (`true` in the AD5M and AD5X presets) | `true` or `false` | Ignores the stray click some touch controllers send when you lift your finger at the end of a scroll. **Scroll Guard**. Takes effect after a restart. It applies only to built-in touchscreens, not to the desktop or Android builds. |
| `scroll_guard_cooldown_ms` | `80` | `20` to `500` | With Scroll Guard on, a press that starts within this many milliseconds of a scroll does not count as a click. No menu. |
| `debug_touches` | `false` | `true` or `false` | Draws a ripple at each touch to diagnose offsets. **Show Touch Points**. The `--debug-touches` flag does the same for one run. |

The `HELIX_SCROLL_GUARD` and `HELIX_SCROLL_GUARD_COOLDOWN_MS` variables override the saved Scroll Guard values. For clicks that fire while you are still scrolling, the setting that helps is `scroll_limit`.

---

## Touch devices

| Key | Default | What it does |
|-----|---------|--------------|
| `touch_device` | `""` (auto-detect) | The input device to use for touch or pointer input, for example `"/dev/input/event1"`. Empty lets HelixScreen find a touch or pointer device. The `HELIX_TOUCH_DEVICE` variable does the same. |
| `device_blacklist` | `[]` | USB input devices HelixScreen ignores for keyboard and barcode-scanner input, each as a lowercase `"vid:pid"` pair such as `["002c:261a"]`. Use it when a USB barcode scanner shows up as a plain keyboard and HelixScreen keeps claiming it, for example when another tool needs the scanner to itself. A blacklisted device is skipped but still listed in the Barcode Scanner settings so you can identify it. |

To find a device's ID, open **Settings > Devices > Spoolman > Barcode Scanner**, where each device is listed with its ID, or run `lsusb` over SSH and read the pair after `ID`, for example `ID 002c:261a`. The full walkthrough is in [Sharing a scanner with another tool](../guide/barcode-scanner.md#sharing-a-scanner-with-another-tool-device-blacklist).

---

## Touch calibration

HelixScreen stores what the calibration wizard solved. Do not edit these by hand; run **Settings > Touch & Input > Touch Calibration** instead.

| Key | Default | What it does |
|-----|---------|--------------|
| `force_calibration` | `false` | Runs the calibration wizard at the next start, then clears itself after a successful calibration. Useful when touch is too far off to reach Settings. The Settings entry is offered for any touchscreen, so you rarely need this. The `--calibrate-touch` flag and `HELIX_TOUCH_CALIBRATE=1` do the same. |
| `calibration` | `{"valid": false}` | The six coefficients `a` to `f` of the correction, written when valid. If the wizard finds the touch X and Y axes swapped against the display it folds that into these numbers, so there is no separate swap setting. |
| `touch_range` | `{"valid": false}` | The raw touch range and axis swap the wizard solved for panels that declare the wrong range. `valid: false` uses the range the kernel declared. Written instead of a full `calibration`, never together. |

The older `display.calibration` location is moved to `input.calibration` on load.

---

## Keyboards and navigation

These sit in the `display` section.

| Key | Default | What it does |
|-----|---------|--------------|
| `page_scroll_buttons` | `false` (`true` on ESP32 screens) | Up and down scroll buttons on long lists, useful on small screens or where dragging feels unresponsive. **Settings > Touch & Input > Scroll Buttons**. |
| `use_system_keyboard` | `false` | Android only: use the Android keyboard for text entry. **System Keyboard**. |
| `hide_keyboard_with_hardware` | `false` | Hide the on-screen keyboard while a USB keyboard is attached. **Hardware Keyboard**. |
| `keep_navbar_visible` | `false` | Android only: always show the Android navigation bar instead of using gestures. **Keep Navigation Bar**. |

---

## Barcode scanner

These sit in the top-level `scanner` section and apply to the machine HelixScreen runs on, not to one printer. Set them from **Settings > Devices > Spoolman > Barcode Scanner**.

| Key | Default | What it does |
|-----|---------|--------------|
| `scanner.usb_vendor_product` | `""` | The chosen USB scanner as `vid:pid`. |
| `scanner.usb_device_name` | `""` | Its name, for display. |
| `scanner.bt_address` | `""` | The chosen Bluetooth scanner. |
| `scanner.keymap` | `"qwerty"` | The scanner's keyboard layout: `qwerty`, `qwertz` or `azerty`. Any other value reads as `qwerty`. |

See the [Barcode Scanner guide](../guide/barcode-scanner.md).
