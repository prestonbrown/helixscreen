# Settings: LED Settings

Go to **Settings > Devices** and tap **LED Settings** to open the LED configuration screen. It has three sections: **STARTUP** (turn lights on when the printer starts), **AUTO STATE** (change lights with what the printer is doing) and **MACRO DEVICES** (lights driven by your own macros). The row only appears when HelixScreen finds a light; if your printer declares a light that isn't fitted, see [Chamber light fitted](devices.md#chamber-light-fitted).

> **Tip:** Each **Light** button on the Home Panel controls one light of your choosing, or every light at once. Tap the gear icon on a Light button in Edit Mode to pick which one; see [Home Panel > LED Controls](../home-widgets-controls.md#led-controls). A Light button sized 2x1 or larger also has an arrow that opens full color, brightness, and effects for its light in the LEDs overlay; the separate **LED Controls** widget always opens straight to that overlay.

---

## Supported LED Types

HelixScreen auto-detects your LED hardware from Klipper and Moonraker. Five types of lighting are supported:

| Type | Examples | How It's Detected |
|------|----------|-------------------|
| **Klipper native strips** | Neopixel (WS2812, SK6812), Dotstar (APA102), PCA9632, GPIO LEDs | Automatically from your Klipper config |
| **Output pin lights** | Single-channel PWM or on/off lights via `[output_pin]` | Auto-detected from pins with "light", "led", or "lamp" in the name |
| **WLED strips** | Network-attached WLED controllers | From Moonraker's WLED configuration |
| **LED effects** | Animated effects (breathing, rainbow, etc.) | Requires the [klipper-led_effect](https://github.com/julianschill/klipper-led_effect) plugin |
| **Macro devices** | Any Klipper macro that controls lights | User-configured (see [Macro Devices](#macro-devices) below) |

A newly detected light shows up as its own tab in the LEDs overlay as soon as it's found, so there is nothing to enable here first. If you don't see a light:
- Make sure it's defined in your Klipper config (`printer.cfg`)
- For WLED, make sure it's configured in Moonraker (`moonraker.conf`)
- Restart HelixScreen after adding new LED hardware

---

## Startup

### LED on at Start

Turns on your lights when Klipper becomes ready: every light your Home Panel Light buttons control, or the chamber light if you haven't placed a button. Useful for chamber lights that should always be on when the printer is powered up. If a button controls a WLED strip, HelixScreen waits up to 5 seconds for WLED to answer before it falls back to the chamber light.

### Startup Brightness

> Only shown while LED on at Start is on.

A slider from 1 to 100% (80% to begin with) that sets how bright the lights are when they come on at startup. HelixScreen also uses this brightness for later light commands until you change it in the LEDs overlay.

---

## Auto-State Lighting

Turn on **Automatic LED Control** and your lights change by themselves based on what the printer is doing, with no macros or manual control. It is good for visual status feedback: dim lights when idle, bright white while printing, green when a print finishes.

If your print start or end macro also sets LEDs, a warning appears under the switch: "Your print start or end macro also sets LEDs. It and Automatic LED Control may override each other." Use one or the other.

### Applies To

Automatic LED Control has its own list of lights, separate from whatever your Home Panel Light buttons control. A chamber light button and a toolhead-strip auto-state effect are two different jobs, and each has its own target. Tap a chip to add or remove that light from the list; at least one light always stays selected. The **Applies to** row and the state rows below appear only while Automatic LED Control is on.

### Printer States

| State | When It Triggers |
|-------|-----------------|
| **Idle** | Klipper is ready, not printing |
| **Heating** | Extruder is heating up (target > 0, not yet printing) |
| **Printing** | Print is in progress |
| **Paused** | Print is paused |
| **Error** | Klipper error or shutdown |
| **Complete** | Print just finished |

### Action Types

Each state has an action type dropdown that sets what HelixScreen does with the "Applies to" lights when that state is entered:

| Action | What It Does |
|--------|--------------|
| **Off** | Turn lights off |
| **Brightness** | Set a brightness level (0–100%) without changing color; 0% turns the lights off |
| **Color** | Set a specific color from the preset swatches, with a brightness slider |
| **Effect** | Activate a Klipper LED effect (e.g., breathing, rainbow) |
| **WLED Preset** | Activate a WLED preset by ID |
| **Macro** | Run one of your macro devices |

> **Note:** Only actions your hardware supports are shown. **Effect** requires the `led_effect` Klipper plugin. **WLED Preset** requires a WLED strip among the lights in **Applies to**. **Color** requires a color-capable strip among them (not single-channel PWM). **Macro** requires at least one macro device.

### Example Setup

| State | Action | Setting |
|-------|--------|---------|
| Idle | Brightness | 30% (dim standby light) |
| Heating | Color | Red at 100% |
| Printing | Color | White at 100% |
| Paused | Effect | "breathing" |
| Error | Color | Red at 100% |
| Complete | Color | Green at 100% |

---

## Macro Devices

Macro devices let you control lights that Klipper's LED system doesn't handle directly, such as relay-switched cabinet lights, custom G-code lighting macros, or multi-mode LED setups.

**Auto-discovery:** HelixScreen finds Klipper macros with "led", "light" or "lamp" in their name and offers them in the macro dropdowns. If yours has a different name, choose **Custom...** and type the macro's name.

### Device Types

| Type | Best for | Controls |
|------|----------|----------|
| **On/Off (state-aware)** | Lights with separate on and off macros | Two macros, **On** and **Off**. HelixScreen tracks whether the light is on |
| **Toggle (fire-and-forget)** | Lights with a single toggle macro | One **Toggle** macro. HelixScreen does not track the state |
| **Preset** | Multi-mode lights (for example party mode, reading mode) | Several named presets, each mapped to a macro |

An On/Off or Toggle device is treated as a light: it gets its own tab in the LEDs overlay and can be chosen in **Applies to**. A Preset device is not a light. It shows its presets as named actions in the LEDs overlay (from the LED Controls widget) and is not offered under **Applies to**.

### Adding a Macro Device

1. Tap **Add** next to **MACRO DEVICES**.
2. Enter a **Name** (for example "Cabinet Light").
3. Choose the **Type**.
4. Choose the macro or macros from the dropdowns. For a Preset device, tap **+ Add Preset** for each preset.
5. Save.

To change a device, tap the pencil icon on its card. To remove it, tap the trash icon. Until you add one, the section reads "No macro devices configured" and "Tap + to add". If HelixScreen found no LED macros on the printer, it also says so and asks you to add Klipper macros for LED control first.

**Example:** with macros `LIGHTS_CABINET_ON` and `LIGHTS_CABINET_OFF` in your Klipper config, create an **On/Off** device named "Cabinet Light" and pick each macro. It gets its own tab in the LEDs overlay, with On and Off buttons where a slider would normally sit.

---

## Setting Up Different LED Types

### Klipper Native Strips (Neopixel, Dotstar, etc.)

These work out of the box. Define them in your `printer.cfg`:

```ini
[neopixel chamber_light]
pin: PA8
chain_count: 24
color_order: GRB
```

Restart Klipper, and the strip appears as its own tab in the LEDs overlay with full color and brightness control. There is nothing to turn on here first.

### Output Pin Lights (Brightness-Only)

If your chamber light or enclosure LED is connected to a `[output_pin]` in Klipper, HelixScreen can control it directly with no macro device.

```ini
[output_pin chamber_light]
pin: PA9
pwm: true
value: 0
```

Pins with "light", "led", or "lamp" in their name are auto-detected. PWM pins get a brightness slider (0–100%); non-PWM pins get a simple on/off toggle. Color controls don't appear, because output pins don't support color.

> **Note:** If your output pin doesn't have one of those keywords in its name, add it as a [Macro Device](#macro-devices) instead.

### WLED Network Strips

WLED strips are network-attached LED controllers managed outside of Klipper. Configure them in `moonraker.conf`:

```ini
[wled strip_name]
address: 192.168.1.100
```

After restarting Moonraker, the WLED strip appears as its own tab in the LEDs overlay. You get on/off toggle and brightness. To switch to a WLED preset, use a **WLED Preset** action in Automatic LED Control with the preset's ID from the WLED web interface.

### LED Effects (Animated Patterns)

Install the [klipper-led_effect](https://github.com/julianschill/klipper-led_effect) plugin and define effects in your `printer.cfg`:

```ini
[led_effect breathing]
leds:
    neopixel:chamber_light
autostart: false
frame_rate: 24
layers:
    breathing 10 1 top (1.0, 1.0, 1.0)
```

Effects show up on that strip's tab in the LEDs overlay and as Auto-State Lighting action options. Only effects targeting that particular strip are shown on its tab.

### Macro-Controlled Lights

For lights controlled via G-code macros (relay-switched enclosure lights, Klipper macros wrapping custom commands, etc.):

1. Define your macros in Klipper (include "led", "light" or "lamp" in the name for auto-discovery)
2. Go to **Settings > Devices > LED Settings** and add a device under **MACRO DEVICES**
3. Pick the macros
4. An On/Off or Toggle device gets its own tab in the LEDs overlay, alongside your other lights

---

[Back to Settings](../settings.md) | [Back to Devices](devices.md)
