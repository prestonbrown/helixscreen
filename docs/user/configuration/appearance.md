# Appearance and Previews

How HelixScreen looks and how it draws your printer, your print and your bed mesh. Unless a row says otherwise these are **per screen**. Toolhead style, Z movement and the printer picture are **per printer**.

See [Settings: Appearance](../guide/settings/appearance.md) for the walkthrough.

[Back to the Configuration Reference](../CONFIGURATION.md)

---

## Theme and colours

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `dark_mode` (top level) | `true` | `true` or `false` | Dark or light version of the theme. **Settings > Appearance > Dark Mode**. Grayed out when the theme only comes in dark. The `--dark` and `--light` command-line flags override it for one run. |
| `display.theme` | `"helixscreen"` | a theme name | The colour theme. **Settings > Appearance > Theme Colors** writes it and applies it straight away. A hand edit takes effect at the next start. |

Built-in theme names: `ayu`, `catppuccin`, `chatgpt`, `cupertino`, `dracula`, `everforest`, `gruvbox`, `hazard`, `helixscreen`, `kanagawa`, `material-design`, `midnight`, `nord`, `onedark`, `rose-pine`, `solarized`, `tokyonight` and `yami`. Themes you make or edit in the theme editor are stored in the `config/themes/` folder of your install and can be named here too.

The `HELIX_THEME` variable overrides the theme for one run or from `helixscreen.env`. The old `theme.preset` number that earlier versions wrote is no longer read.

---

## Effects

| Key | Default | What it does |
|-----|---------|--------------|
| `display.animations_enabled` | on where the hardware handles it well, off on slower hardware and on screens HelixScreen has to rotate in software | Screen transitions, confetti and other motion. **Settings > Appearance > Animations**. Turn it off if the screen feels slow. |
| `display.glass_effects` | `false` | Draws buttons and panels with glass depth and lighting. **Settings > Appearance > Glass Effects**. The row only appears on screens that passed HelixScreen's speed check, and the effect shows only there. Elsewhere the value is kept but the screen stays flat. |
| `appearance.show_widget_labels` | `false` | Text labels under the Home screen widget icons. **Settings > Appearance > Widget Labels**. |
| `display.page_scroll_buttons` | `false` (`true` on ESP32 screens such as the BTT K-Touch) | Up and down scroll buttons on long lists. See [Input](input.md). |

---

## Printer Visuals

These rows are under **PRINTER VISUALS** in **Settings > Appearance**. The printer behaves the same whatever you choose.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `printers.<id>.appearance.toolhead_style` | `0` | `0` Auto, `1` Default, `2` A4T, `3` AntHead, `4` JabberWocky, `5` Stealthburner, `6` Creality K1, `7` Creality K2 | The toolhead picture on the Home screen and Print Status. **Toolhead Style**. Auto works the picture out from your printer type or filament system, and uses Default when it cannot tell. Override it only when the wrong picture shows. |
| `printers.<id>.z_movement_style` | `0` | `0` Auto, `1` Bed moves, `2` Nozzle moves | How the Z buttons on the Motion screen are labelled. **Z Movement**. Only the labels change, never the commands sent. |
| `display.gcode_render_mode` | `0` | `0` Auto, `1` 3D View, `2` 2D Layers, `3` Thumbnail Only | How Print Status draws the file that is printing. **G-code Preview**. |
| `display.bed_mesh_render_mode` | `0` | `0` Auto, `1` 3D View, `2` 2D Heatmap | How bed mesh results are drawn. **Bed Mesh Render**. |
| `display.bed_mesh_show_zero_plane` | `true` | `true` or `false` | A translucent reference plane at Z=0 in the 3D bed mesh view, which shows where the nozzle touches the bed. |
| `display.gcode_3d_enabled` | `true` | `true` or `false` | When `false`, no toolpath is drawn at all and Print Status shows only the slicer's thumbnail, whatever `gcode_render_mode` says. |
| `display.speed_flow_physical_units` | `false` | `true` or `false` | Leads with the live toolhead speed in mm/s and the live volumetric flow in mm³/s instead of percentages, in the Print Tune overlay and the Speed / Flow home widget. Tapping either readout in the Tune overlay flips it. The Print Status speed chip always shows the live speed and flow. See [Print Tune Overlay](../guide/printing.md#print-tune-overlay). |
| `display.print_status_show_camera` | `false` | `true` or `false` | Shows the live camera in the Print Status preview card instead of the thumbnail or G-code render. The camera button in the card's corner flips it and the choice carries to the next print. The button appears only when the printer has a webcam and HelixScreen runs on a different machine from Klipper. See [Camera](../guide/camera.md). |
| `printers.<id>.printer_image` | `""` | `""`, `"shipped:<name>"`, `"custom:<name>"` | The picture on the Home screen and in the Printer Manager. Empty picks one from the printer type Klipper reports. `shipped:` names an image that ships with HelixScreen, such as `shipped:voron-v2`. `custom:` names one you imported. Change it from the Printer Manager (tap the printer picture on the Home screen). |

**Auto G-code preview.** In Auto, HelixScreen uses interactive 3D on hardware with a working GLES renderer and 2D layers everywhere else. It also falls back to 2D if 3D has to give up in the middle of a print. A file whose preview would cost the board too much processor time shows its thumbnail instead; pick 3D View or 2D Layers yourself to draw it anyway.

**Forcing a render mode for one run.** The command line wins over the environment, which wins over the setting. `helix-screen --render-2d` forces 2D Layers and `--render-3d` forces 3D View. `HELIX_GCODE_MODE=3D` or `HELIX_GCODE_MODE=2D` (exactly those capital letters) does the same from `helixscreen.env`; any other value falls back to 2D. Launching with `--render-2d` is the quick way to rule out 3D rendering on a slow board.

**Custom printer images.** Put PNG or JPEG files in the install's `config/custom_images/` folder. They are converted to the format HelixScreen draws (300 px and 150 px versions) when you open the Printer Image picker. The maximum file size is 5 MB and the maximum size is 2048 x 2048 pixels.

---

## G-code viewer tuning

These settings are in the `gcode_viewer` section and tune the 3D and 2D viewers. There is no menu for them.

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `tube_sides` | `4` | `4`, `8`, `16` | Cross-section detail of the toolpath: `4` diamond (fastest), `8` octagonal (balanced), `16` circular (matches OrcaSlicer). |
| `streaming_mode` | `"auto"` | `"auto"`, `"on"`, `"off"` | How large files are handled. `auto` streams files that would use too much memory, `on` always streams (least memory), `off` always loads the whole file (fastest viewing). The `HELIX_GCODE_STREAMING` variable overrides it. |
| `streaming_threshold_percent` | `40` | `1` to `90` | The percentage of free memory that triggers streaming in `auto` mode. Lower values stream smaller files. On boards with 2 GB of memory or less it is capped at 15. |
| `layers_per_frame` | `0` | `0` to `100` | How many layers the 2D view draws per frame while it builds its picture. `0` adapts to your hardware. Higher values finish faster but can stutter the UI on slow devices. |
| `adaptive_layer_target_ms` | `16` | milliseconds | The render time target when `layers_per_frame` is `0`. Lower is smoother, higher builds faster. 16 targets about 60 frames per second. |

Two more settings sit in the `streaming` section:

| Key | Default | What it does |
|-----|---------|--------------|
| `threshold_mb` | `0` | Files larger than this many MB always stream, even if memory would allow loading them whole. `0` leaves the decision to `streaming_threshold_percent`. A value here can only add streaming. |
| `force_streaming` | `false` | Always stream. Same as `streaming_mode` set to `"on"`. An explicit `streaming_mode` of `"on"` or `"off"` takes precedence. |

---

## Filament spool picture

| Key | Default | Values | What it does |
|-----|---------|--------|--------------|
| `ams.spool_style` | `"3d"` | `"3d"`, `"flat"` | How spools are drawn: `3d` from the side in 3D, `flat` face-on with the filament showing through the vents. |
