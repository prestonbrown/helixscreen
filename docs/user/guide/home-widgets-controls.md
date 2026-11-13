# Home Panel: Control and System Widgets

The widgets that move or command the printer, and the ones that report on the computer behind it: Toolhead, Bed Mesh, Tool Switcher, GCode Console, Shutdown/Reboot, Host Health and the LED controls. Sizes and hardware needs are in the [Widget Catalog](home-widgets.md).

## Toolhead Widget

The Toolhead widget shows where the toolhead is without opening Motion. Each axis reads `--` until it is homed, because an unhomed axis has no real position.

The Toolhead widget is not available on the BigTreeTech K-Touch.

![Toolhead at 2x2: the plate with a crosshair for the nozzle, the Z bar and the three axis values](../../images/user/home-widget-toolhead.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5, 1x0.5 | The letters X, Y and Z, lit as each axis is homed. During a print, the Z height instead |
| 0.5x1, 1x1 | One row per axis with its position |
| 1.5x1 to 2x1 | The plate with a crosshair for the nozzle and a bar for Z, beside the three values |
| 2x2 and larger | The same, with **Home All** underneath while an axis is not homed. During a print, the active tool and live speed take its place |
| 2x3 and larger | The plate grows, and a row of three preset buttons sits under it with a line saying how high the next move will lift |

**Home All** homes the printer straight away. It is the only thing on the widget that homes.

### Moving the toolhead from the plate

From 1.5x1 up, the plate moves the toolhead once the printer is homed and idle:

1. **Tap** a spot on the plate. A target marker appears there, with a **Move here** button and an **x** at the top of the plate. The line under the plate shows where the toolhead will go and how high it will lift first, for example "-> X60 Y61" and "Z 0.20, will lift to 5mm". Nothing moves yet.
2. Tap **Move here** to send the toolhead there, or **x** to cancel. If you do neither, the target disappears by itself after about five seconds.

To move the toolhead live, **drag** on the plate instead: the toolhead follows your finger, just like the [Bed tab](motion.md#bed-tab) of the Motion screen. Start the drag on the plate itself; a drag that starts there does not swipe the home page.

Before any move across the plate, the nozzle lifts to the **Bed Map Clearance** height if it is lower ([Motion Settings](motion.md#motion-settings), 5 mm by default). While your last print may still be on the bed, it lifts above that print instead, the plate outlines the print's objects, and a **Cleared** button appears in the plate's top-right corner. Tap it once you have removed the part, and confirm when HelixScreen asks **Is the bed clear?**. See [A print on the bed](motion.md#a-print-on-the-bed).

On a small plate the **Move here** and **Cleared** buttons show only their icons.

The plate does not move anything while:

- any axis is not homed (use **Home All**, or home from the Motion screen)
- a print is running or paused (the plate shows the live toolhead over the print's objects)
- the printer is not ready, or something else is using the toolhead
- you are in Edit Mode

Then a tap on the plate opens the Motion panel, like a tap anywhere else on the widget. A tap on the Z bar or the X, Y and Z values always opens the Motion panel.

### Preset buttons

At 2x3 and larger, three preset buttons sit under the plate. By default they are **Front**, **Center** and **Park**. To change them:

1. Enter Edit Mode and tap the Toolhead widget.
2. Tap the **gear icon** in the upper-left corner. The **Presets** picker opens with a tab for each of the three buttons.
3. Pick a tab, then pick what that button does. Your choice is saved straight away.

| Choice | What the button does |
|--------|----------------------|
| One of the nine positions (Rear Left to Front Right) | Moves the toolhead there, the same as the [Move tab](motion.md#move-tab) |
| **Park** | Parks the toolhead, the same as [Park](motion.md#park) on the Motion screen |
| **Z +10** / **Z -10** | Moves Z by 10 mm. While your last print may still be on the bed, **Z -10** stops at the height that clears it |
| **Macro** | Runs a macro you pick from the list that follows, asking for its parameters or a confirmation first, the way a [Macro Button](home-widgets.md#macro-button-confirmation) with **Require Confirmation?** turned on does |
| **Empty** | Hides the button |

The position, **Park** and Z buttons are grayed out until the printer is homed, and while a print is running or paused. A macro button works whenever the printer is connected.

---

## Bed Mesh Widget

The Bed Mesh widget draws the active mesh as a color heatmap, blue at the low points and red at the high ones, and tells you how flat the bed is. The flatness number is the difference between the highest and lowest point: green below 0.15 mm, amber below 0.30 mm, red from there. See [Bed Mesh](calibration.md#bed-mesh) for creating and managing meshes.

![Bed Mesh at 2x2: heatmap, color scale, profile name and flatness](../../images/user/home-widget-bed-mesh.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5 | A grid icon over the flatness number |
| 1x0.5 to 1x1 | A small heatmap and the flatness number |
| 2x1 | The heatmap beside the profile name, flatness, lowest and highest point, and grid size |
| 2x2 and larger | The heatmap with a color scale, the profile name, flatness and the number of saved profiles |
| 3x3 and larger | The same with a 3D view of the bed, on screens that can draw it. A slow screen keeps the flat heatmap |

With no mesh loaded the heatmap gives way to "No mesh loaded". At 2x2 and larger, while nothing is printing, two buttons appear:

- **Calibrate** homes the printer if needed and probes a new mesh into the default profile.
- **Load profile** appears when you have saved profiles and opens the Bed Mesh screen to pick one.

Both are unavailable while a print is running. **Tap** anywhere else on the widget to open the Bed Mesh screen.

---

## Tool Switcher Widget

The Tool Switcher widget lets you change the active tool on multi-tool printers, IDEX machines, toolchangers, and other multi-head setups. It is made for printers with more than one tool.

### How It Looks

Each tool gets its own button, sized so a fingertip can always hit it. How much the widget shows depends on how big you make it:

- **A row of tool buttons** when every tool fits: one button per tool, with the mounted tool highlighted. Each button shows the tool's friendly name over its G-code number ("Tool 1" over "T0"), or just "T0" when the button is narrow. A strip along the bottom of the button shows the color of the filament loaded in that tool.
- **One large button** when the tools do not all fit at a usable size. It shows the mounted tool, for example "Tool 2 (T1)", and its temperature when there is room. Tap it to open a **tool picker** listing every tool.
- **A detail list** under the row on tall widgets: the mounted tool's material and temperature first, then the other tools in smaller text. Temperatures of 150°C and up show in the hot color.

On a Snapmaker U1 the tools are called toolheads ("Toolhead 1"). If your Klipper config gives a tool its own name, such as `[tool Left]` with klipper-toolchanger, that name appears instead of "Tool N": "Left (T0)", or "T0" where space is tight.

The widget updates automatically when the active tool changes, whether you switch it here, run a tool-change macro, or it changes during a print.

### Switching Tools

Tap a tool that is not mounted to switch to it. Tapping the tool that is already mounted opens the tool picker instead.

- **When idle**, HelixScreen asks "Switch to Tool 2 (T1)?" first. Turn off [Confirm tool changes](settings/printing.md#confirm-tool-changes) in Settings > Printing to switch right away.
- **When paused**, a confirmation dialog always appears first, because changing tools mid-print can cause problems. Printers whose filament change homes the toolhead itself (such as the AD5X) refuse tool changes while paused as well.
- **While printing or preparing to print**, and while spools are [drying on the bed](temperature.md#drying-filament-on-the-bed), switching is unavailable: the tools are grayed out, and a tap shows a warning instead of changing tools.
- **While your filament system needs attention** (it has stopped and is waiting for you), a tap shows a warning instead. While it is busy loading or unloading, a tap does nothing.

Where a hub feeds several lanes into one toolhead, tapping that head picks up the lane already loaded in it, so its filament stays where it is (#1811). With no lane loaded, or more than one claiming the head, the head's lowest lane is used.

A toolhead on your [filament system's screen](filament.md#switching-toolheads-from-the-filament-path) and the tool list on the Filament screen switch tools the same way.

### Parking the Tool

On printers that can leave the carriage empty (Snapmaker U1 and klipper-toolchanger setups) the widget also has a **Park** button, which puts the mounted tool back in its dock. It sits at the end of the row when there is room, beside the large button on smaller widgets, and otherwise in the tool picker. Park is grayed out when no tool is mounted, and unavailable while a print is running or paused.

> **Tool Switcher vs. Active Tool Badge:** The Tool Switcher is an optional, interactive widget you add to your dashboard. The [tool badge](home-panel.md#emergency-stop-and-the-tool-badge) is a small read-only number beside the nozzle icon on temperature tiles and the print status card, shown on multi-tool printers. The badge shows the current tool; the widget lets you change it.

---

## GCode Console Widget

The GCode Console widget gives you quick access to a full-featured command console for sending G-code commands directly to Klipper.

### Opening the Console

Tap the GCode Console widget on the Home Panel. A full-screen console overlay opens with your recent command history.

### Using the Console

**Sending commands:**
- Type a G-code command in the text field at the bottom (e.g., `G28`, `M114`, `FIRMWARE_RESTART`)
- Press **Enter** to send

**Navigating history:**
- Press the **Up arrow** to recall previous commands
- Press the **Down arrow** to move forward through history
- The arrows recall your last 20 sent commands (the scrolling log above shows a larger history loaded from Moonraker's G-code store)

**Reading output:**
- Commands you sent appear in the scrolling log
- Klipper responses appear below each command
- **Error messages** are highlighted in red
- Color-coded output from plugins like AFC and Happy Hare is preserved
- Periodic temperature status messages (T:/B: reports) are filtered out to reduce noise

**Scrolling:**
- The console auto-scrolls to the newest entry as responses arrive
- Scroll up manually to pause auto-scroll and read older output
- Scroll back to the bottom to resume auto-scrolling

---

## Shutdown/Reboot Widget

The Shutdown/Reboot widget puts one-tap host shutdown/reboot on your home panel, a faster alternative to the **Shutdown** and **Reboot** entries on the **Advanced** panel. A confirmation dialog always appears first, so there's no risk of an accidental shutdown. For switching a PSU or smart plug instead, see the **Power** widget above.

---

## Host Health Widget

The Host Health widget watches the computer running Klipper and your printer's controller boards (MCUs). The same readings are on **Settings > System > Performance**, which a tap on the widget opens. It is grayed out until Moonraker reports performance data.

![Host Health at 2x2: host CPU and memory, free disk space and each controller board](../../images/user/home-widget-host-health.png)

| Size | What you see |
|------|--------------|
| 0.5x0.5 | One chip icon and one number. Normally the CPU load; when something is off, the reading that is off |
| 1x0.5, 0.5x1 | CPU load and temperature, with memory on the tall shape |
| 1x1 | CPU load with a small history trace, temperature and memory |
| 2x1 | CPU and memory traces. During a print the busiest MCU's load replaces memory |
| 2x2 and larger | The host's CPU, temperature, memory and free disk space, a line for CPU throttling, and a row for each MCU |

A reading turns amber, then red, as it gets worse:

| Reading | Amber | Red |
|---------|-------|-----|
| Host CPU load, or an MCU's load | 70% | 90% |
| Host CPU temperature | 70 C | 80 C |
| CPU throttling | It happened since the last boot | Happening now |
| MCU communication | Retransmits are climbing | None |
| Free disk space for G-code files | Under 500 MB | None |

---

## LED Controls

Tap the **LED Controls** widget, or the arrow on a wider **LED Light** button, to open the **LEDs** overlay, one tab per light, each showing only the controls that light actually supports.

### Tabs

A tab per light runs along the top in a row that scrolls sideways once you have more than fit. Each tab carries a small dot: filled in that light's current color while it's on, hollow while it's off, or a dimmed ring for a light whose on/off state HelixScreen can't read (a macro-driven light). Tapping a tab only changes which light you're looking at, it never changes what a Home Panel Light button or Automatic LED Control targets.

![LEDs overlay, tabs across the top, power and brightness, white tones, color swatches](../../images/user/home-led-control.png)

### Power & Brightness

Most lights show a round power button above a tall brightness slider:

- **Power button**: fills with the light's current color while on, an outline while off. Tap to toggle. Turning a light off this way also stops any effect running on it.
- **Brightness slider**: drag to set 0-100%; 0% turns the light off. The fill matches the light's current color, with the percentage shown inside it.

A light with no brightness control (a plain on/off `[output_pin]`, or a macro Toggle device) shows just a centered button in that spot.

### White (Color-Capable Klipper LEDs)

RGBW and RGB lights show three fixed white swatches, **Cool**, **Neutral**, **Warm**. Tap one to set that white level: on an RGBW light it drives the dedicated white channel, on RGB it's mixed from the color channels. A neopixel counts as RGBW when its `color_order` includes a `W`. A ring shows which one (if any) matches the light's current look.

### Color

Color-capable lights also show a row of preset color swatches, plus a rainbow **Custom** swatch at the end that opens the full color picker. A ring shows which preset matches the light's current color.

### Effects & Presets

- **Klipper LED effects** ([klipper-led_effect](https://github.com/julianschill/klipper-led_effect)): the effects defined for that light appear as chips, plus a **None** chip that stops whichever one is running. Tap a chip to activate its effect.
- **WLED presets**: the presets you've configured on that WLED device appear as chips, tap to activate. WLED lights get power and brightness here, but no color controls.
- **Non-color Klipper lights and PWM output pins**: quick level chips (10/25/50/75/100%) appear instead, since there's nothing else to show.

### Macro-Driven Lights

A light backed by a Klipper macro can't show a power dot or button, because HelixScreen has no way to read whether it's actually on:

- **On/Off devices** (configured in [LED Settings](settings/led-settings.md#macro-devices)): separate **On** and **Off** buttons take the place of the slider
- **Toggle devices**: a single **Toggle** button
- **Preset devices**: named preset buttons fill the whole page

---

**Back to:** [Home Panel](home-panel.md) | [Widget Catalog](home-widgets.md)
