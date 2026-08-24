# Settings: Printing

The Printing category contains settings that affect how prints are configured and displayed.

---

## Toolhead Style

Choose the toolhead icon shown on the Home Panel and Print Status screen. Options:

| Option | Description |
|--------|-------------|
| **Auto** (default) | HelixScreen detects your toolhead from the printer database or Klipper config |
| **Stealthburner** | Voron StealthBurner toolhead |
| **A4T** | Armored Turtle toolhead |
| **AntHead** | AntHead toolhead |
| **JabberWocky** | JabberWocky toolhead |

Most users can leave this on **Auto**. Change it if HelixScreen picks the wrong icon or if you've swapped to an aftermarket toolhead.

> **Note:** The native styles (**Default**, **Creality K1**, **Creality K2**) are auto-detected from your printer and don't appear as choices in the dropdown.

---

## G-code Preview

Choose how the G-code of the active print is visualized:

| Option | Description |
|--------|-------------|
| **Auto** (default) | HelixScreen picks the best mode for your hardware — interactive 3D on capable devices, falling back to lighter modes on slower ones |
| **3D View** | Interactive 3D rendering of the toolpath |
| **2D Layers** | Flat per-layer view — lighter on the GPU than 3D |
| **Thumbnail Only** | Shows just the slicer-embedded thumbnail, no live toolpath rendering — the lightest option |

Use a lighter mode if your hardware struggles with 3D rendering.

---

## Z Movement

Controls how Z-axis movement is displayed in the motion controls.

| Mode | Behavior |
|------|----------|
| **Auto** (default) | HelixScreen auto-detects based on your printer type (bed-slinger vs CoreXY vs delta) |
| **Bed Moves** | Z controls labeled as bed movement (bed goes down = nozzle moves up relative to bed) |
| **Nozzle Moves** | Z controls labeled as nozzle movement (nozzle goes up = away from bed) |

This only changes the direction labels in the UI — the actual G-code sent is the same. Use this if auto-detection picks the wrong style for your printer.

---

## Machine Limits

Tap to open the Machine Limits overlay. A banner at the top reminds you: **"Changes are temporary and reset on printer reboot."** These sliders override your Klipper config for the current session — useful for testing or troubleshooting motion issues. To make permanent changes, edit `printer.cfg` directly.

Each setting has a slider with the live value shown on the right:

| Setting | Range | Description |
|---------|-------|-------------|
| **Max Velocity** | 50–1000 mm/s | Maximum toolhead speed |
| **Max Acceleration** | 500–50000 mm/s² | Maximum acceleration |
| **Accel to Decel** | 500–50000 mm/s² | Acceleration-to-deceleration limit (caps how aggressively moves slow down) |
| **Square Corner Velocity** | 1–20 mm/s | Maximum speed carried through square corners |
| **Extrude Speed** | 1–50 mm/s | Feedrate used for manual extrude/retract actions |

> **Extrude Speed is saved.** Unlike the motion limits above, the Extrude Speed value is a persisted HelixScreen setting — it is remembered across restarts and applies to the manual extrude/retract controls.

Below the adjustable sliders is a read-only **Config-defined** section showing your **Max Z Velocity** and **Max Z Accel**, which come from your Klipper config and cannot be changed here.

**Reset:** the **Reset** button at the bottom restores the motion limits to your printer's original configured values.

---

## Retraction Settings

> Only shown when firmware retraction (`[firmware_retraction]`) is configured in Klipper.

Tap to open the retraction overlay. Configure G10/G11 firmware retraction parameters. **Changes apply immediately** and can be adjusted mid-print for tuning.

| Parameter | Range | Description |
|-----------|-------|-------------|
| **Enable Retraction** | On/Off | Master toggle for firmware retraction |
| **Retract Length** | 0–6 mm | Amount of filament to retract. 0.4–2 mm for direct drive, 4–6 mm for bowden. |
| **Retract Speed** | 10–80 mm/s | Speed of the retraction movement |
| **Unretract Extra** | 0–1 mm | Extra filament to prime after retraction to compensate for ooze |
| **Unretract Speed** | 10–60 mm/s | Speed of the prime (unretract) movement |

---

## Material Temperatures

Configure preheat presets for different filament materials (PLA, PETG, ABS, TPU, etc.). Each material can have:

- **Nozzle Temperature** — Target extruder temperature
- **Bed Temperature** — Target bed temperature
- **Preheat Macro** — A Klipper macro to run when preheating this material
- **Macro Handles Heating** — If enabled, the macro is responsible for setting temperatures. If disabled, HelixScreen sets temperatures first, then runs the macro as an additional step.

---

## Timelapse

> Only shown when the [Moonraker-Timelapse](https://github.com/mainsail-crew/moonraker-timelapse) plugin is installed.

Tap to open the Timelapse Settings overlay. Configure how HelixScreen records timelapse videos of your prints.

| Setting | Options | Description |
|---------|---------|-------------|
| **Enable Timelapse** | On/Off | Master toggle for timelapse recording |
| **Recording Mode** | Layer / Hyperlapse | **Layer** captures one frame at each layer change — best for most prints. **Hyperlapse** captures frames at fixed time intervals — better for very long prints. |
| **Framerate** | 15 / 24 / 30 / 60 fps | Playback speed of the rendered video. 30 fps is the default. |
| **Auto-render video** | On/Off | When enabled, automatically renders a video file when the print completes |

A quick **timelapse toggle** also appears on the print status panel, so you can enable or disable recording without leaving the print view.

Changes are saved immediately and sent to Moonraker. If the timelapse plugin is not yet installed, HelixScreen shows an **Install Wizard** that walks you through the SSH commands to set it up — see [Advanced > Timelapse](../advanced.md#timelapse) for details.

For browsing and playing recorded videos, see **Advanced > Timelapse Videos** (covered in [Advanced > Timelapse](../advanced.md#timelapse)).

---

## Macro Buttons

Tap to open the Macro Buttons overlay. Configure quick-action buttons and standard macro assignments.

### Quick Buttons

These macros power the buttons you see on the Controls and Filament panels:

| Button | Where it appears | What it does by default |
|--------|-----------------|------------------------|
| **Cooldown** | Preheat widget (when heaters are on), Filament panel | Turns off extruder and bed heaters |
| **Load Filament** | Filament panel | Runs `LOAD_FILAMENT` |
| **Unload Filament** | Filament panel | Runs `UNLOAD_FILAMENT` |
| **Custom Macro 1** | Controls panel | Runs `HELIX_CLEAN_NOZZLE` (label: "Clean Nozzle") |
| **Custom Macro 2** | Controls panel | Runs `HELIX_BED_LEVEL_IF_NEEDED` (label: "Bed Level") |

**Cooldown behavior:** When you preheat a material using the Preheat widget on the home or controls panel, the button automatically switches to **Cool Down** while any heater target is above zero. Tapping it runs your configured cooldown macro. This is especially useful if your cooldown needs to do more than just turn off heaters — for example, turning off chamber heaters, bed fans, or recirculation fans.

You can customize any of these. Each button has a **label** (what the button says) and **G-code** (what it runs when tapped). The cooldown macro can be a simple G-code string or a multi-line sequence:

```
SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0
SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0
SET_FAN_SPEED FAN=bed_fan SPEED=0
```

For advanced configuration via `settings.json`, see the [default_macros reference](../../CONFIGURATION.md#default_macros).

### Standard Macros

HelixScreen auto-detects common macros from your Klipper configuration (e.g., it recognizes `CLEAN_NOZZLE`, `NOZZLE_CLEAN`, and similar naming patterns). You can override any auto-detected assignment:

| Slot | What it controls | Common auto-detected macros |
|------|------------------|-----------------------------|
| **Load Filament** | Filament load operations | LOAD_FILAMENT, M701 |
| **Unload Filament** | Filament unload operations | UNLOAD_FILAMENT, M702 |
| **Purge** | Nozzle purge/prime | PURGE, PURGE_LINE, LINE_PURGE, PRIME_LINE |
| **Pause** | Print pause | PAUSE, M600 |
| **Resume** | Print resume | RESUME |
| **Cancel** | Print cancel | CANCEL_PRINT |
| **Bed Mesh** | Bed mesh calibration | BED_MESH_CALIBRATE |
| **Bed Level** | Manual bed leveling | BED_SCREWS_ADJUST, SCREWS_TILT_CALCULATE |
| **Clean Nozzle** | Nozzle cleaning | CLEAN_NOZZLE, NOZZLE_CLEAN |
| **Heat Soak** | Chamber heat soak | HEAT_SOAK |

If your printer doesn't have a matching macro, some slots fall back to HelixScreen helper macros (installed via **Settings > Advanced > Install HelixScreen Macros**). Leave a slot empty to disable that function.

**Load Filament and Unload Filament on a multi-filament printer:** left on **(Auto)**, these two drive your filament system directly rather than running a macro. Choose a macro yourself and it takes over — your macro runs and the filament system's own handling is skipped for that operation, so anything it would have done becomes your macro's job. Set the slot back to **(Auto)** to hand the operation back. The other slots are unaffected. See [Customizing which macro runs](../filament.md#customizing-which-macro-runs).

> **Looking for Load/Unload/Purge button customization?** See the [Filament guide](../filament.md#customizing-which-macro-runs) for a step-by-step walkthrough, including how these buttons interact with AMS systems.

### Per-Material Preheat Macros

You can also assign a custom Klipper macro to each material preset (PLA, PETG, ABS, TPU). This is useful when preheating requires more than just setting temperatures — for example, turning on bed fans for ABS or starting a chamber heater.

Configure per-material macros in **Material Temperatures** (above). Each material can have:

- **Preheat Macro** — A Klipper macro to run when preheating this material
- **Macro Handles Heating** — If enabled, the macro is responsible for setting temperatures. If disabled, HelixScreen sets temperatures first, then runs the macro as an additional step.

---

[Back to Settings](../settings.md) | [Prev: Display & Sound](display-sound.md) | [Next: Hardware & Devices](hardware.md)
