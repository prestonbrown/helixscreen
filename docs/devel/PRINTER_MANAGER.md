# Printer Manager (Developer Guide)

How the Printer Manager overlay works, its sub-components (inline name editing, custom printer images, capability chips), and how to extend it.

---

## Overview

The Printer Manager is a full-screen overlay accessed by tapping the printer image on the home panel. It serves as the central hub for printer identity and hardware capabilities, displaying:

- **Printer identity** -- name (editable inline), model, and image (swappable)
- **Software versions** -- Klipper, Moonraker, and HelixScreen
- **Hardware capabilities** -- auto-detected features shown as filterable chips

It follows the standard OverlayBase lifecycle and uses the `overlay_panel` XML component for navigation (back button, title bar).

### Key Files

| File | Purpose |
|------|---------|
| `include/ui_printer_manager_overlay.h` | Class declaration, subjects, callbacks |
| `src/ui/ui_printer_manager_overlay.cpp` | Implementation (refresh, name edit, chip nav) |
| `ui_xml/printer_manager_overlay.xml` | Declarative layout (3 sections) |
| `tests/unit/test_printer_manager_overlay.cpp` | Unit tests |
| `include/printer_image_manager.h` | Image import/conversion/selection |
| `src/system/printer_image_manager.cpp` | PNG/JPEG to LVGL .bin pipeline |
| `include/ui_overlay_printer_image.h` | Image picker overlay |
| `src/ui/ui_overlay_printer_image.cpp` | Image picker implementation |
| `ui_xml/printer_image_overlay.xml` | Image picker layout (list + preview) |
| `include/printer_images.h` | Printer type to image path mapping |
| `include/prerendered_images.h` | Screen-responsive image size selection |
| `config/printer_database.json` | Printer definitions, heuristics, images |
| `include/capability_matrix.h` | Pre-print operation capability merging |
| `include/capability_overrides.h` | User overrides for auto-detected capabilities |

---

## Architecture

```
PrinterManagerOverlay (overlay_panel)
  |
  +-- Section 1: Printer Identity
  |     +-- Printer image (lv_image, clickable -> PrinterImageOverlay)
  |     +-- Printer name (text_heading, clickable -> inline edit)
  |     +-- Printer model (text_muted, bound to subject)
  |
  +-- Section 2: Software Versions
  |     +-- Klipper version (bound to klipper_version subject)
  |     +-- Moonraker version (bound to moonraker_version subject)
  |     +-- HelixScreen version (bound to helix_version subject)
  |
  +-- Section 3: Hardware Capabilities
        +-- Capability chips (flow-wrapped, subject-bound visibility)
        +-- Clickable chips navigate to feature overlays
```

### Subject Bindings

The overlay owns three subjects for its own data:

| Subject Name | Type | Description |
|---|---|---|
| `printer_manager_name` | string | User-editable printer name |
| `printer_manager_model` | string | Printer model from config |
| `helix_version` | string | HelixScreen build version |

Capability chip visibility is driven by subjects defined elsewhere (in `PrinterState`):

| Subject | Controls |
|---|---|
| `printer_has_probe` | Probe chip |
| `printer_has_bed_mesh` | Bed Mesh chip |
| `printer_has_heater_bed` | Heated Bed chip |
| `printer_has_led` | LEDs chip |
| `printer_has_accelerometer` | ADXL chip |
| `printer_has_qgl` | QGL chip |
| `printer_has_z_tilt` | Z-Tilt chip |
| `printer_has_firmware_retraction` | Retraction chip |
| `printer_has_spoolman` | Spoolman chip |
| `printer_has_timelapse` | Timelapse chip |
| `printer_has_screws_tilt` | Screws Tilt chip |
| `printer_has_webcam` | Webcam chip |
| `printer_has_extra_fans` | Fans chip |
| `printer_has_chamber_sensor` | Chamber chip |
| `printer_has_speaker` | Speaker chip |
| `printer_has_nozzle_clean` | Nozzle Clean chip |
| `ams_type` | AMS chip (hidden when 0) |

All chips use `<bind_flag_if_eq flag="hidden" ref_value="0"/>` -- they are hidden when the subject value is 0, visible when non-zero.

---

## Overlay Lifecycle

```
get_printer_manager_overlay()     // Singleton, lazy-created
  .init_subjects()                // Register 3 string subjects
  .register_callbacks()           // Register chip + name edit + image callbacks
  .create(parent)                 // Build XML tree, find named widgets

on_activate()                     // Called each time overlay becomes visible
  -> cancel_name_edit()           // Reset any in-progress edit
  -> refresh_printer_info()       // Re-read config, update subjects + image
```

`refresh_printer_info()` is called on every activation, ensuring the overlay always shows current data. It:

1. Reads printer name from `Config` (key: `/wizard/printer_name`)
2. Reads printer model from `Config` (key: `/wizard/printer_type`)
3. Reads HelixScreen version from `helix_version()`
4. Resolves the printer image: checks `PrinterImageManager` for a user-selected image first, falls back to auto-detection via `PrinterImages::get_best_printer_image()`

---

## Inline Name Editing

The printer name supports click-to-edit inline renaming. This is implemented as a heading/textarea swap pattern.

### How It Works

Two widgets occupy the same layout slot in the XML:

```xml
<!-- Always visible (unless editing) -->
<text_heading name="pm_printer_name" bind_text="printer_manager_name"/>

<!-- Hidden until edit starts -->
<text_input name="pm_printer_name_input" hidden="true" one_line="true"/>
```

The edit lifecycle is managed by three methods:

```
start_name_edit()     // User taps heading
  1. Set name_editing_ = true
  2. Pre-fill textarea with current name_buf_
  3. Hide heading, show textarea
  4. Focus textarea and show keyboard (ui_keyboard_show)

finish_name_edit()    // User presses Enter/checkmark (LV_EVENT_READY)
  1. Read new name from textarea
  2. Save to Config and persist to disk
  3. Update printer_manager_name subject
  4. Swap back: show heading, hide textarea

cancel_name_edit()    // User presses Escape/X (LV_EVENT_CANCEL)
  1. Swap back without saving
```

### Implementation Notes

- The heading click is registered via XML `<event_cb callback="pm_printer_name_clicked"/>` (declarative)
- The textarea `READY` and `CANCEL` events are registered via `lv_obj_add_event_cb()` in `create()` -- this is an acceptable exception to the declarative rule because LVGL textarea lifecycle events are not supported in XML
- The keyboard is managed by `ui_keyboard_show()` from `ui_keyboard_manager.h`
- `on_activate()` cancels any in-progress edit, preventing stale state when navigating back

### Reusability

The heading/textarea swap pattern is currently inline in `PrinterManagerOverlay`. To reuse it elsewhere:

1. Add a `text_heading` + `text_input` pair in XML (same parent, input hidden)
2. Register a click callback on the heading container
3. Use `lv_obj_add_event_cb()` for READY/CANCEL on the textarea
4. Implement the three-method pattern: `start_edit()`, `finish_edit()`, `cancel_edit()`

The pattern could be extracted into a reusable component if more inline edit fields are needed.

---

## Custom Printer Images

### Image Selection Flow

```
User taps printer image in Printer Manager
  -> change_printer_image_clicked_cb()
  -> PrinterImageOverlay.show()
     -> Left panel: scrollable list of images
        - Auto-Detect option (uses printer type for lookup)
        - Shipped images (bundled PNGs, pre-rendered to .bin)
        - Custom images (user-imported, in config/custom_images/)
        - USB import (if USB drive detected)
     -> Right panel: live preview of selected image
```

### Image ID Namespace

All image references use a namespaced ID string:

| Format | Meaning | Example |
|---|---|---|
| `""` (empty) | Auto-detect from printer type | Default behavior |
| `"shipped:NAME"` | Bundled printer image | `"shipped:voron-24r2"` |
| `"custom:NAME"` | User-imported image | `"custom:my-printer"` |

The active image ID is persisted in `Config` at the per-printer path (e.g., `/printers/{id}/printer_image`). Legacy configs with `/display/printer_image` are migrated automatically during v3-to-v4 config migration.

### Supported Source Formats

| Format | Constraints |
|---|---|
| PNG | Up to 2048x2048px, up to 5MB |
| JPEG/JPG | Up to 2048x2048px, up to 5MB |

Validation is performed by `PrinterImageManager::validate_image()` using `stbi_info()` for dimension checks without full decode.

### Conversion Pipeline

When a PNG/JPEG is imported (from USB or dropped into `config/custom_images/`):

```
Source PNG/JPEG
  |
  stbi_load() -> RGBA pixel buffer (4 channels forced)
  |
  +-- stbir_resize_uint8() -> 300px variant (aspect ratio preserved)
  |     RGBA -> BGRA swap (LVGL ARGB8888 in little-endian)
  |     write_lvgl_bin() -> config/custom_images/NAME-300.bin
  |
  +-- stbir_resize_uint8() -> 150px variant (aspect ratio preserved)
        RGBA -> BGRA swap
        write_lvgl_bin() -> config/custom_images/NAME-150.bin
```

The `write_lvgl_bin()` function (in `lvgl_image_writer.h`) writes an LVGL 9 binary image header followed by raw pixel data. It uses atomic write (temp file + rename) to prevent corruption on embedded devices.

Two size variants are always created:
- **300px** -- for medium and large displays (800x480 and above)
- **150px** -- for small displays (480x320) and preview thumbnails

### Shipped Image Pipeline

Shipped images (bundled with HelixScreen) go through a build-time conversion:

```bash
make gen-printer-images          # Runs scripts/regen_printer_images.sh
```

This converts all PNGs in `assets/images/printers/` to pre-rendered `.bin` files at 300px and 150px, stored in `build/assets/images/printers/prerendered/`. Generic fallback images (`generic-*.png`) are skipped (kept as PNG for flexibility).

### Screen-Responsive Image Sizing

`get_printer_image_size(screen_width)` selects the variant:

| Screen Width | Image Size |
|---|---|
| >= 600px | 300px |
| < 600px | 150px |

The XML also uses responsive consts for the image container:

```xml
<consts>
  <px name="pm_image_size_small" value="120"/>   <!-- 391-460px height -->
  <px name="pm_image_size_medium" value="160"/>  <!-- 461-550px height -->
  <px name="pm_image_size_large" value="200"/>   <!-- 551-700px height (_xlarge falls back here) -->
</consts>
```

### Image Resolution Priority

When displaying the printer image in the Printer Manager:

1. **User-selected image** -- `PrinterImageManager::get_active_image_path()` checks config for `shipped:` or `custom:` ID
2. **Auto-detected image** -- `PrinterImages::get_best_printer_image()` looks up the printer type in `printer_database.json`
3. **Fallback** -- `generic-corexy.png` if nothing else matches

### Auto-Import

On overlay activation, `auto_import_raw_images()` scans `config/custom_images/` for any raw PNG/JPEG files that lack corresponding `.bin` variants and imports them automatically. This lets users simply drop image files into the directory.

---

## Printer Database

The printer database (`config/printer_database.json`) is the source of truth for printer identity, detection heuristics, and image mappings.

### Entry Structure

```json
{
  "id": "flashforge_adventurer_5m",
  "name": "FlashForge Adventurer 5M",
  "manufacturer": "FlashForge",
  "image": "flashforge-adventurer-5m.png",
  "print_start_profile": "forge_x",
  "z_offset_calibration_strategy": "gcode_offset",
  "heuristics": [
    {
      "type": "macro_match",
      "field": "macros",
      "pattern": "SUPPORT_FORGE_X",
      "confidence": 99,
      "reason": "Definitive ForgeX signature"
    }
  ],
  "print_start_capabilities": {
    "macro_name": "START_PRINT",
    "params": { ... }
  }
}
```

### Key Fields

| Field | Purpose |
|---|---|
| `id` | Unique identifier (snake_case) |
| `name` | Human-readable name shown in UI |
| `manufacturer` | Manufacturer name |
| `image` | PNG filename in `assets/images/printers/` |
| `print_start_profile` | Profile for print start phase detection |
| `z_offset_calibration_strategy` | Z-offset approach (`gcode_offset`, etc.) |
| `heuristics` | Array of detection rules with confidence scores |
| `print_start_capabilities` | PRINT_START macro parameters |

### Heuristic Types

| Type | Description |
|---|---|
| `hostname_match` | Match Moonraker hostname |
| `kinematics_match` | Match printer kinematics (corexy, cartesian, delta) |
| `object_exists` | Check for Klipper object in objects/list |
| `stepper_count` | Count of Z steppers |
| `sensor_match` | Match temperature_sensor names |
| `fan_match` | Match fan object names |
| `fan_combo` | Match multiple fan patterns |
| `led_match` | Match LED/neopixel names |
| `mcu_match` | Match MCU chip type |
| `build_volume_range` | Match bed dimensions |
| `macro_match` | Match G-code macro names |

Detection scores are combined across multiple heuristics. A confidence of >= 70 is considered high-confidence.

### Image Lookup

`PrinterDetector::get_image_for_printer(name)` returns the `image` field for a given printer name. Note: there is no `PrinterDatabase` class -- the printer database is a JSON file (`config/printer_database.json`) accessed through `PrinterDetector` static methods. `PrinterImages::get_best_printer_image()` then resolves this to an LVGL path, preferring pre-rendered `.bin` files.

---

## Capability Chips

Capability chips are pill-shaped UI elements displayed in a flow-wrapped container. Each chip has:

- An icon (Material Design icon via the `icon` component)
- A label (via `text_small`)
- Subject-driven visibility (`bind_flag_if_eq` hides when capability is 0)
- Optional click handler for navigation

### Chip Categories

| Chip | Subject | Clickable | Navigates To |
|---|---|---|---|
| Probe | `printer_has_probe` | No | -- |
| Bed Mesh | `printer_has_bed_mesh` | Yes | BedMeshPanel |
| Heated Bed | `printer_has_heater_bed` | No | -- |
| LEDs | `printer_has_led` | Yes | (toast: coming soon) |
| ADXL | `printer_has_accelerometer` | Yes | InputShaperPanel |
| QGL | `printer_has_qgl` | No | -- |
| Z-Tilt | `printer_has_z_tilt` | No | -- |
| Retraction | `printer_has_firmware_retraction` | Yes | RetractionSettingsOverlay |
| Spoolman | `printer_has_spoolman` | Yes | SpoolmanPanel |
| Timelapse | `printer_has_timelapse` | Yes | TimelapseSettingsOverlay |
| Screws Tilt | `printer_has_screws_tilt` | Yes | ScrewsTiltPanel |
| Webcam | `printer_has_webcam` | No | -- |
| AMS | `ams_type` | Yes | AMS Panel |
| Fans | `printer_has_extra_fans` | Yes | FanControlOverlay |
| Chamber | `printer_has_chamber_sensor` | No | -- |
| Speaker | `printer_has_speaker` | Yes | SoundSettingsOverlay |
| Nozzle Clean | `printer_has_nozzle_clean` | No | -- |

### Chip Navigation Pattern

Clickable chips use the `lazy_create_and_push_overlay` helper for lazy panel creation:

```cpp
void PrinterManagerOverlay::on_chip_bed_mesh_clicked(lv_event_t* e) {
    auto& pm = get_printer_manager_overlay();
    helix::ui::lazy_create_and_push_overlay<BedMeshPanel>(
        get_global_bed_mesh_panel,          // global accessor
        pm.bed_mesh_panel_,                 // cached panel pointer
        lv_display_get_screen_active(nullptr),
        "Bed Mesh", "Printer Manager");     // overlay name, source name
}
```

The panel pointer is cached as a member of `PrinterManagerOverlay` so the panel is only created once.

---

## Developer Guide: Extending

### Adding a New Capability Chip

1. **Add the subject** in `PrinterState` (if not already present):
   ```cpp
   // In printer_state.h / printer_state.cpp
   lv_subject_t printer_has_my_feature_;
   ```

2. **Add the chip XML** in `printer_manager_overlay.xml` inside the capabilities section:
   ```xml
   <lv_obj name="pm_chip_my_feature"
           width="content" height="content" style_bg_color="#card_bg" style_bg_opa="255"
           style_radius="16" style_pad_top="#space_xs" style_pad_bottom="#space_xs"
           style_pad_left="#space_sm" style_pad_right="#space_sm"
           flex_flow="row" style_pad_gap="#space_xs" style_flex_cross_place="center"
           scrollable="false" style_border_width="0" clickable="true">
     <bind_flag_if_eq subject="printer_has_my_feature" flag="hidden" ref_value="0"/>
     <event_cb trigger="clicked" callback="pm_chip_my_feature_clicked"/>
     <icon src="my_icon" size="xs" variant="success"/>
     <text_small text="My Feature"/>
   </lv_obj>
   ```

3. **Add the callback** in `ui_printer_manager_overlay.h`:
   ```cpp
   static void on_chip_my_feature_clicked(lv_event_t* e);
   lv_obj_t* my_feature_panel_ = nullptr;
   ```

4. **Register the callback** in `register_callbacks()`:
   ```cpp
   lv_xml_register_event_cb(nullptr, "pm_chip_my_feature_clicked",
                            on_chip_my_feature_clicked);
   ```

5. **Implement navigation** in the `.cpp`:
   ```cpp
   void PrinterManagerOverlay::on_chip_my_feature_clicked(lv_event_t* e) {
       (void)e;
       auto& pm = get_printer_manager_overlay();
       helix::ui::lazy_create_and_push_overlay<MyFeaturePanel>(
           get_global_my_feature_panel, pm.my_feature_panel_,
           lv_display_get_screen_active(nullptr),
           "My Feature", "Printer Manager");
   }
   ```

### Adding a Shipped Printer Image

1. Place a PNG file in `assets/images/printers/` (e.g., `my-printer.png`)
2. Run `make gen-printer-images` to create pre-rendered `.bin` variants
3. Add a printer entry in `config/printer_database.json` with `"image": "my-printer.png"`
4. The image will appear in both the auto-detect lookup and the shipped images list

### Adding a Custom Image Import Source

The `PrinterImageOverlay` supports USB import out of the box. To add a new import source (e.g., network download):

1. Add a new section in `printer_image_overlay.xml` (similar to the USB section)
2. Add a populate method in `PrinterImageOverlay` (similar to `populate_usb_images()`)
3. Use `PrinterImageManager::import_image()` to convert and store the image
4. Call `refresh_custom_images()` after import to update the list

### Display Name Formatting

`PrinterImageManager::format_display_name()` converts filename stems to readable names:
- Dashes and underscores become spaces
- Dashes between two digits become dots (e.g., `voron-0-2` becomes `voron 0.2`)

---

## Testing

```bash
# Run Printer Manager tests
./build/bin/helix-tests "[printer_manager]"

# Run printer image manager tests
./build/bin/helix-tests "[printer_image_manager]"

# Run capability tests
./build/bin/helix-tests "[capability_matrix]"
./build/bin/helix-tests "[capability_overrides]"
```

Tests cover:
- Subject initialization and double-init safety
- Global singleton accessor pattern
- Destructor cleanup (with and without init)
- Lifecycle state tracking

---

## Multi-Printer Management

When beta features are enabled and multiple printers are configured, a **Manage Printers** button appears at the bottom of the Printer Manager overlay. This navigates to the Printer List overlay for switching, adding, and deleting printers.

See [Multi-Printer Management](MULTI_PRINTER.md) for the full developer guide on the multi-printer system.
