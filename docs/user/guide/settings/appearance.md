# Settings: Appearance

**Settings > Appearance** changes how HelixScreen looks. Use it to switch between light and dark, pick or edit a color theme, turn motion effects on or off, and choose how your printer is drawn on screen. Nothing here changes what the printer does.

On the Settings screen, the **Appearance** row shows the mode and the theme you're using, for example *Dark Mode · Nord*.

![Appearance settings, top of the page](../../../images/user/settings-appearance.png)

![Appearance settings, scrolled to Printer Visuals](../../../images/user/settings-appearance-2.png)

---

## Dark Mode

Switches between the light and dark version of your theme. **On** (dark) by default. The switch is grayed out when your theme only comes in one version (see the Modes column in [Built-in Themes](#built-in-themes)).

---

## Theme Colors

Opens the theme explorer, where you browse, preview and apply color themes.

![The theme explorer](../../../images/user/settings-theme.png)

To change your theme:

1. Tap **Theme Colors**.
2. Open the **Theme Preset** menu and pick a theme. The preview cards, buttons and status colors update as you go.
3. If the theme has both a light and a dark version, the **Dark Mode** switch picks which one you see.
4. Tap **Apply**. The new theme takes effect everywhere right away. No restart needed.

To back out, leave the explorer without tapping **Apply**. Your previous theme comes back, in the light or dark version it was in when you opened the explorer, even if you flipped the **Dark Mode** switch while looking around.

### Built-in Themes

Eighteen themes come with HelixScreen, listed alphabetically in the Theme Preset menu. Every theme has a dark version. All but the four marked **Dark only** also have a light version.

| Theme | Look | Modes |
|-------|------|-------|
| **Ayu** | Warm gold accent with coral errors on slate | Light + dark |
| **Catppuccin** | Soft pastels on gentle lavender | Light + dark |
| **ChatGPT** | Understated grays with green and red status colors | Light + dark |
| **Cupertino** | Apple-style system colors on near-black | Light + dark |
| **Dracula** | Violet on deep slate with bright green highlights | Dark only |
| **Everforest** | Muted forest green and clay | Light + dark |
| **Gruvbox** | Warm retro earth tones: amber, olive, brick | Light + dark |
| **Hazard** | Safety-vest yellow, warning orange and danger red on powder-black, with sharp corners and brass seams | Dark only |
| **HelixScreen** | The default: balanced blue on charcoal | Light + dark |
| **Kanagawa** | Soft wave-blue and gold on deep indigo | Light + dark |
| **Material Design** | Material blues and greens on gray | Light + dark |
| **Midnight** | Near-black navy, dim and low-glare | Dark only |
| **Nord** | Arctic blues and frost tones | Light + dark |
| **One Dark** | The Atom editor look: blue and green on charcoal | Light + dark |
| **Rose Pine** | Muted rose, iris and pine on soft charcoal | Light + dark |
| **Solarized** | The classic muted scheme on deep teal | Light + dark |
| **Tokyo Night** | City-at-night blues with pastel highlights | Light + dark |
| **Yami** | Vivid blue on deep gray, high contrast | Dark only |

### Editing a Custom Theme

Tap **Edit** in the theme explorer to open the theme editor. You can recolor a theme and change its corners, borders and shadows. Everything around the editor updates as you go, so you see the result straight away.

The editor has two sections:

- **Theme Colors**: 16 color swatches, each labeled with what it colors (background, text, primary, success, warning and so on). Tap a swatch, then pick a ready-made color or mix your own.
- **Style Properties**: four sliders.

| Slider | What it does |
|--------|--------------|
| **Border Radius** | How rounded corners are, from square to fully round |
| **Border Width** | How thick borders are |
| **Border Opacity** | How visible borders are (0 is invisible, 255 is solid) |
| **Shadow Intensity** | How strong drop shadows are (0 turns them off) |

### Light and Dark Palettes

A theme can have a separate light and dark palette, and the editor changes one at a time. Pick which one before you tap **Edit**: turn the explorer's **Dark Mode** switch on to edit the dark palette, or off to edit the light one. The switch only appears for themes that have both. Saving keeps both palettes, including the one you didn't touch.

> **Tip:** To tune both palettes, edit one and save. Then flip the Dark Mode switch, tap **Edit** again and adjust the other.

### Saving Your Changes

Three buttons sit at the bottom of the editor:

| Button | What it does |
|--------|--------------|
| **Reset** | For a built-in theme, puts the original colors back. For a theme you made, goes back to its last saved state |
| **Save As New** | Saves your changes as a new theme and leaves the original alone |
| **Save** | Saves over the current theme. Grayed out until you've changed something |

**Save As New** asks for a name. It suggests "*theme name* Copy". Tap **Save** and the new theme is applied and added to the Theme Preset menu.

Both **Save** and **Save As New** apply the theme right away. If you try to leave the editor with unsaved changes, HelixScreen asks **Discard Changes?** first.

> **Note:** Themes you make or edit are stored on the printer in `~/helixscreen/config/themes/`. They survive updates, and you can copy them to another printer.

---

## Glass Effects

Gives buttons and panels depth and lighting: buttons look like glass you press, and cards catch light from their top-left corner. It works with every theme and keeps your theme's colors. **Off** by default.

The row only appears on screens fast enough to draw it. The first time HelixScreen starts on a new screen, or after an update, it checks that quietly in the background, so the row can take a few seconds to appear. On slower hardware, 16-bit displays and screens whose picture HelixScreen has to rotate itself, it never appears and the screen stays flat.

---

## Animations

Turns motion effects on or off, such as screens sliding in and confetti when a print finishes. It starts on wherever the hardware handles it well, and off on slower hardware and on screens whose picture HelixScreen has to rotate itself. Turn it off if the screen feels slow, for example on a Raspberry Pi 3.

---

## Widget Labels

Shows a text label under icon-only widgets on the home screen. **Off** by default. Turn it on while you're learning the icons, and off for a cleaner look on small screens once you know what each icon means.

---

## Printer Visuals

The rows under **PRINTER VISUALS** change how your printer and its data are drawn. The printer itself behaves exactly the same whatever you pick.

### Toolhead Style

The toolhead picture used on the Home screen and the print status screen.

| Option | Picture |
|--------|---------|
| **Auto** (default) | HelixScreen works out your toolhead from its printer list or your filament system |
| **Default** | A generic toolhead |
| **A4T** | Armored Turtle A4T |
| **AntHead** | AntHead |
| **JabberWocky** | JabberWocky |
| **Stealthburner**, **Mini Stealthburner**, **Dragonburner** | The Voron toolheads of those names |
| **Creality K1**, **Creality K2**, **Bambu Lab**, **Ender 3**, **XOL**, **Snapmaker U1**, **FlashForge AD5**, **FlashForge Creator 5**, **Elegoo Centauri**, **Artillery M1**, **Qidi**, **Sovol SV08** | That printer's or toolhead's own head |

Leave it on **Auto** unless HelixScreen shows the wrong toolhead, or you've fitted an aftermarket one. Printers with their own toolhead picture get it automatically on Auto. The `toolhead_style` numbers for `settings.json` are in [Configuration](../../CONFIGURATION.md#toolhead_style).

### Tool Numbers

How a tool's number is written where it appears on its own: the small badge beside the nozzle, the other-tools chip on the print status screen, and names such as "Nozzle 2". This row only appears on printers with more than one nozzle.

| Option | Shows |
|--------|-------|
| **Auto** (default) | "T0, T1, T2" on a tool changer whose Klipper tools are all named `T0`, `T1`, ...; "1, 2, 3" on every other printer |
| **1, 2, 3** | Each tool's position, counted from 1 ("Nozzle 2") |
| **T0, T1, T2** | Each tool's G-code name, counted from 0 ("Nozzle T1") |

A tool you named yourself in Klipper (such as `[tool Left]`) keeps its name on the badge whatever you pick. Choose the option that matches the labels on your machine's tools or docks.

### G-code Preview

How the print status screen draws the file that's printing.

| Option | What you see |
|--------|--------------|
| **Auto** (default) | HelixScreen picks for your hardware: 3D where the screen can handle it, lighter views where it can't |
| **3D View** | A 3D model of the toolpath you can turn and zoom |
| **2D Layers** | A fixed corner view of the model, drawn layer by layer, with ledges and rims shaded so the shape reads. Lighter than 3D |
| **Thumbnail Only** | Just the picture your slicer saved in the file. The lightest option |

Pick a lighter option if the print status screen is slow on your printer.

In **Auto**, a file whose preview would cost your board too much CPU shows its thumbnail instead. HelixScreen judges that from how fast it read earlier files on this board, and allows less while a print is running, so a large file never slows the printer. Pick **3D View** or **2D Layers** yourself to draw such a file anyway.

### Bed Mesh Render

How bed mesh results are drawn: **Auto**, **3D View** or **2D Heatmap**. The heatmap is lighter on slow hardware and easier to read at a glance.

---

[Back to Settings](../settings.md) | [Prev: Display](display.md) | [Next: Touch & Input](touch-input.md)
