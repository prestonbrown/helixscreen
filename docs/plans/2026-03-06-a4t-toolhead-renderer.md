# A4T Toolhead Renderer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an Armored Turtle (A4T) toolhead renderer and convert the toolhead style system from boolean to user-selectable enum.

**Architecture:** New `ToolheadStyle` enum follows the existing `ZMovementStyle` pattern in SettingsManager. The A4T renderer uses polygon-based drawing (like the faceted/Stealthburner renderer) with procedural hexagons for the honeycomb accent. All canvas widgets switch from `bool use_faceted_toolhead` to `ToolheadStyle`.

**Tech Stack:** LVGL 9.5 drawing primitives (triangles, fills, arcs), C++17, SettingsManager subjects, XML settings panel.

**Design doc:** `docs/plans/2026-03-06-a4t-toolhead-renderer-design.md`

---

### Task 1: Create A4T Renderer

The core drawing function. Header already exists at `include/nozzle_renderer_a4t.h`.

**Files:**
- Modify: `include/nozzle_renderer_a4t.h` (already created, verify)
- Create: `src/rendering/nozzle_renderer_a4t.cpp`

**Docs:** Read `include/nozzle_renderer_common.h` for helper functions (`nr_darken`, `nr_lighten`, `nr_blend`, `nr_draw_nozzle_tip`). Study `src/rendering/nozzle_renderer_faceted.cpp` for the polygon + ear-clipping pattern. Study `src/rendering/nozzle_renderer_bambu.cpp` for the procedural approach.

**Step 1: Create the renderer implementation**

Create `src/rendering/nozzle_renderer_a4t.cpp` with:

- **Design space:** 1000x1000, center at (500, 500)
- **Body:** Dark rectangular housing (#1A1A1A) with beveled bottom corners, polygon-based
- **Extruder section:** Narrower block on top (#2A2A2A) with a gear circle
- **Fan:** Central dark circle at ~(500, 580), radius ~100
- **Hexagonal honeycomb:** 8-10 hexagons in A4T green (#B5CC18), two columns flanking the fan. Use a procedural `draw_hexagon()` helper (6 triangles from center, like `draw_circle()` in the faceted renderer but with 6 segments and 30-degree offset).
- **Nozzle tip:** Use `nr_draw_nozzle_tip()` from common helpers
- **Opacity:** Same dim lambda pattern as faceted/bambu renderers
- **Filament detection:** Same unloaded-color check pattern as other renderers

Rendering approach: polygons for body outline (reuse `draw_polygon()` and `scale_polygon()` from faceted — extract or duplicate), procedural `draw_hexagon()` for hex accents, `draw_circle()` for fan.

Note: The faceted renderer's `draw_polygon()`, `draw_circle()`, `scale_polygon()`, `cross_product_sign()`, `point_in_triangle()`, `is_convex_vertex()`, `is_ear()` are all `static` in `nozzle_renderer_faceted.cpp`. For the A4T renderer, either duplicate the needed helpers or (better) use simpler shapes that only need triangles. The body can be drawn as a few rectangles + triangles for the bevels rather than a full polygon with ear-clipping.

**Step 2: Build and verify compilation**

Run: `make -j`
Expected: Clean build, no errors.

**Step 3: Commit**

```
feat(rendering): add Armored Turtle (A4T) toolhead renderer
```

---

### Task 2: Add ToolheadStyle Enum and SettingsManager Integration

Follow the `ZMovementStyle` pattern exactly.

**Files:**
- Modify: `include/settings_manager.h`
  - Add enum near line 17 (after `ZMovementStyle`)
  - Add getter/setter/subject declarations (after Z movement section, ~line 114)
  - Add subject member (after line 170)
- Modify: `src/system/settings_manager.cpp`
  - Add options string (after line 27)
  - Add subject init in `init_subjects()` (after Z movement init, ~line 69)
  - Add getter/setter/options implementations (after Z movement impls, ~line 156)

**Step 1: Add the enum to settings_manager.h**

After `ZMovementStyle` (line 17), add:
```cpp
/** @brief Toolhead rendering style (Auto=detect from printer type, or force) */
enum class ToolheadStyle { AUTO = 0, DEFAULT = 1, STEALTHBURNER = 2, A4T = 3 };
```

**Step 2: Add declarations to settings_manager.h**

After the Z movement section (after line 114), add a new section:
```cpp
// =========================================================================
// TOOLHEAD STYLE (owned by SettingsManager — appearance setting)
// =========================================================================

/** @brief Get toolhead rendering style */
ToolheadStyle get_toolhead_style() const;

/** @brief Get effective toolhead style (resolves AUTO using printer detection) */
ToolheadStyle get_effective_toolhead_style() const;

/** @brief Set toolhead rendering style and persist */
void set_toolhead_style(ToolheadStyle style);

/** @brief Get dropdown options string */
static const char* get_toolhead_style_options();

/** @brief Toolhead style subject (integer: 0=Auto, 1=Default, 2=Stealthburner, 3=A4T) */
lv_subject_t* subject_toolhead_style() {
    return &toolhead_style_subject_;
}
```

**Step 3: Add subject member to settings_manager.h**

After `lv_subject_t extrude_speed_subject_;` (line 171), add:
```cpp
lv_subject_t toolhead_style_subject_;
```

**Step 4: Add implementation to settings_manager.cpp**

After `Z_MOVEMENT_STYLE_OPTIONS_TEXT` (line 27), add:
```cpp
static const char* TOOLHEAD_STYLE_OPTIONS_TEXT = "Auto\nDefault\nStealthburner\nArmored Turtle";
```

In `init_subjects()`, after Z movement init (~line 69), add:
```cpp
// Toolhead style (default: 0 = Auto)
int toolhead_style = config->get<int>("/appearance/toolhead_style", 0);
toolhead_style = std::clamp(toolhead_style, 0, 3);
UI_MANAGED_SUBJECT_INT(toolhead_style_subject_, toolhead_style, "settings_toolhead_style",
                       subjects_);
```

After `get_z_movement_style_options()` (~line 156), add getter/setter/options:
```cpp
ToolheadStyle SettingsManager::get_toolhead_style() const {
    int val = lv_subject_get_int(const_cast<lv_subject_t*>(&toolhead_style_subject_));
    return static_cast<ToolheadStyle>(std::clamp(val, 0, 3));
}

ToolheadStyle SettingsManager::get_effective_toolhead_style() const {
    auto style = get_toolhead_style();
    if (style != ToolheadStyle::AUTO) {
        return style;
    }
    // Auto-detect: Voron → Stealthburner, else Default
    if (PrinterDetector::is_voron_printer()) {
        return ToolheadStyle::STEALTHBURNER;
    }
    return ToolheadStyle::DEFAULT;
}

void SettingsManager::set_toolhead_style(ToolheadStyle style) {
    int val = static_cast<int>(style);
    val = std::clamp(val, 0, 3);
    spdlog::info("[SettingsManager] set_toolhead_style({})", val);

    lv_subject_set_int(&toolhead_style_subject_, val);

    Config* config = Config::get_instance();
    config->set<int>("/appearance/toolhead_style", val);
    config->save();
}

const char* SettingsManager::get_toolhead_style_options() {
    return TOOLHEAD_STYLE_OPTIONS_TEXT;
}
```

Note: Add `#include "printer_detector.h"` to settings_manager.cpp if not already present.

**Step 5: Build and verify**

Run: `make -j`
Expected: Clean build.

**Step 6: Commit**

```
feat(settings): add ToolheadStyle enum with Auto/Default/Stealthburner/A4T
```

---

### Task 3: Update Canvas Widgets to Use ToolheadStyle

Replace `bool use_faceted_toolhead` with `ToolheadStyle` in all three canvas widgets.

**Files:**
- Modify: `include/ui_filament_path_canvas.h:312-319` — change setter signature
- Modify: `src/ui/ui_filament_path_canvas.cpp` — change data field, setter, render dispatch (~lines 169, 1513-1516, 2243-2246, 2496, 2876-2878)
- Modify: `include/ui_system_path_canvas.h:222-228` — change setter signature
- Modify: `src/ui/ui_system_path_canvas.cpp` — change data field, setter, render dispatch (~lines 109, 892-895, 1067-1070, 1464-1465)
- Modify: `src/ui/ui_z_offset_indicator.cpp` — change data field, render dispatch (~lines 34, 236-239, 471)

**Step 1: Update filament_path_canvas header**

In `include/ui_filament_path_canvas.h`, change the setter declaration from:
```cpp
void ui_filament_path_canvas_set_faceted_toolhead(lv_obj_t* obj, bool faceted);
```
to:
```cpp
void ui_filament_path_canvas_set_toolhead_style(lv_obj_t* obj, ToolheadStyle style);
```

Add `#include "settings_manager.h"` (for the enum) or forward-declare/include just the enum. Since `ToolheadStyle` is in `namespace helix`, may need `using helix::ToolheadStyle;` or qualify it.

**Step 2: Update filament_path_canvas.cpp**

Change the data struct field (line 169):
```cpp
ToolheadStyle toolhead_style = ToolheadStyle::DEFAULT;
```

Add includes at top:
```cpp
#include "nozzle_renderer_a4t.h"
#include "settings_manager.h"  // for ToolheadStyle enum
```

At each render dispatch site (~lines 1513 and 2243), replace the if/else with:
```cpp
switch (data->toolhead_style) {
    case ToolheadStyle::STEALTHBURNER:
        draw_nozzle_faceted(layer, ...);
        break;
    case ToolheadStyle::A4T:
        draw_nozzle_a4t(layer, ...);
        break;
    default:
        draw_nozzle_bambu(layer, ...);
        break;
}
```

Update the attribute parser (~line 2496) to handle the new field name. Change from:
```cpp
} else if (strcmp(name, "faceted_toolhead") == 0) {
    data->use_faceted_toolhead = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
```
to supporting the style by name or int. Keep backward compat with `faceted_toolhead` attribute too.

Update the setter function (~line 2876):
```cpp
void ui_filament_path_canvas_set_toolhead_style(lv_obj_t* obj, ToolheadStyle style) {
    auto* data = get_data(obj);
    if (!data) return;
    if (data->toolhead_style != style) {
        data->toolhead_style = style;
        spdlog::debug("[FilamentPath] Toolhead style: {}", static_cast<int>(style));
        lv_obj_invalidate(obj);
    }
}
```

Also keep the old `set_faceted_toolhead` as a compatibility wrapper or remove it (check all callers are updated).

**Step 3: Update system_path_canvas (same pattern)**

Apply identical changes to `include/ui_system_path_canvas.h` and `src/ui/ui_system_path_canvas.cpp`:
- Header: change setter signature
- Impl: change data field, add includes, switch at render sites (~lines 892, 1067), update setter (~line 1462)

**Step 4: Update z_offset_indicator**

In `src/ui/ui_z_offset_indicator.cpp`:
- Change field (line 34): `ToolheadStyle toolhead_style = ToolheadStyle::DEFAULT;`
- Change render dispatch (lines 236-239): same switch pattern
- Change initialization (line 471): `data->toolhead_style = ToolheadStyle::DEFAULT;`
- Add includes for `nozzle_renderer_a4t.h` and `settings_manager.h`

**Step 5: Update heat glow tip_y calculation in filament_path_canvas**

At ~line 2252, there's a conditional for tip_y based on faceted toolhead. Add an A4T case:
```cpp
if (data->toolhead_style == ToolheadStyle::STEALTHBURNER) {
    tip_y = nozzle_y + (data->extruder_scale * 46) / 10 - 6;
} else if (data->toolhead_style == ToolheadStyle::A4T) {
    // A4T: similar body height to Stealthburner (uses same 10x scale)
    tip_y = nozzle_y + (data->extruder_scale * 46) / 10 - 6;
} else {
    tip_y = nozzle_y + data->extruder_scale * 2;
}
```
(Adjust the A4T tip_y offset after visual testing.)

**Step 6: Build and verify**

Run: `make -j`
Expected: Clean build. May have temporary errors from callers not yet updated (Task 4).

**Step 7: Commit**

```
refactor(canvas): replace bool faceted_toolhead with ToolheadStyle enum
```

---

### Task 4: Update Callers to Use New API

Update the sites that called `set_faceted_toolhead(true)` and `is_voron_printer()`.

**Files:**
- Modify: `src/ui/ui_ams_detail.cpp:491-492`
- Modify: `src/ui/ui_panel_ams_overview.cpp:172-173`
- Modify: any other callers of `set_faceted_toolhead` (search codebase)

**Step 1: Update ui_ams_detail.cpp**

Replace (~line 491):
```cpp
if (PrinterDetector::is_voron_printer()) {
    ui_filament_path_canvas_set_faceted_toolhead(canvas, true);
}
```
with:
```cpp
ui_filament_path_canvas_set_toolhead_style(
    canvas, SettingsManager::instance().get_effective_toolhead_style());
```

Add `#include "settings_manager.h"` if not present.

**Step 2: Update ui_panel_ams_overview.cpp**

Replace (~line 172):
```cpp
if (PrinterDetector::is_voron_printer()) {
    ui_system_path_canvas_set_faceted_toolhead(system_path_, true);
}
```
with:
```cpp
ui_system_path_canvas_set_toolhead_style(
    system_path_, SettingsManager::instance().get_effective_toolhead_style());
```

**Step 3: Search for any other callers**

Run: `grep -rn "set_faceted_toolhead\|use_faceted_toolhead" src/ include/`
Update any remaining references.

**Step 4: Build and verify**

Run: `make -j`
Expected: Clean build, no references to old API.

**Step 5: Commit**

```
refactor(ui): use ToolheadStyle enum at all toolhead selection sites
```

---

### Task 5: Add Settings UI Dropdown

Add a "Toolhead Style" dropdown to the settings panel.

**Files:**
- Modify: `ui_xml/settings_panel.xml` — add dropdown row
- Modify: `src/ui/ui_panel_settings.cpp` — add callback, register it, init dropdown value

**Docs:** Read `ui_xml/settings_panel.xml` to find the right section for an appearance setting. The Z movement dropdown at lines 123-127 is the template.

**Step 1: Add XML dropdown**

In `ui_xml/settings_panel.xml`, add near other appearance/display settings:
```xml
<setting_dropdown_row name="row_toolhead_style"
                      label="Toolhead Style" label_tag="Toolhead Style" icon="printer_3d_nozzle"
                      description="How the toolhead icon appears"
                      description_tag="How the toolhead icon appears"
                      options="Auto&#10;Default&#10;Stealthburner&#10;Armored Turtle"
                      options_tag="Auto&#10;Default&#10;Stealthburner&#10;Armored Turtle"
                      callback="on_toolhead_style_changed"/>
```

(Check available icon names — `printer_3d_nozzle` may need to be substituted with an existing icon.)

**Step 2: Add callback in ui_panel_settings.cpp**

After the `on_z_movement_style_changed` callback (~line 164), add:
```cpp
static void on_toolhead_style_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto style = static_cast<ToolheadStyle>(index);
    spdlog::info("[SettingsPanel] Toolhead style changed: {}", index);
    SettingsManager::instance().set_toolhead_style(style);
}
```

**Step 3: Register callback**

In the XML callback registration map (~line 295), add:
```cpp
{"on_toolhead_style_changed", on_toolhead_style_changed},
```

**Step 4: Initialize dropdown value in setup()**

After the Z movement dropdown init (~line 488), add:
```cpp
// === Toolhead Style Dropdown ===
lv_obj_t* toolhead_style_row = lv_obj_find_by_name(panel_, "row_toolhead_style");
if (toolhead_style_row) {
    lv_obj_t* toolhead_dropdown = lv_obj_find_by_name(toolhead_style_row, "dropdown");
    if (toolhead_dropdown) {
        auto style = SettingsManager::instance().get_toolhead_style();
        lv_dropdown_set_selected(toolhead_dropdown, static_cast<uint32_t>(style));
        spdlog::trace("[{}]   ✓ Toolhead style dropdown (style={})", get_name(),
                      static_cast<int>(style));
    }
}
```

**Step 5: Verify (no rebuild needed for XML changes)**

Run: `make -j` (for the C++ changes)
Launch: `./build/bin/helix-screen --test -vv`
Navigate to Settings panel, verify the dropdown appears and persists selection.

**Step 6: Commit**

```
feat(settings): add Toolhead Style dropdown (Auto/Default/Stealthburner/A4T)
```

---

### Task 6: Visual Testing and Polish

**Step 1: Launch and test each style**

Run the app with `--test -vv`, go to Settings, select each toolhead style, navigate to the AMS panel to see the toolhead render. Verify:
- Default (Bambu) renders correctly
- Stealthburner renders correctly
- A4T renders with dark body, green hexagons, fan, nozzle tip
- Auto selects based on printer type

**Step 2: Tune A4T visual appearance**

Adjust polygon coordinates, hex positions, colors, and nozzle tip offset based on visual testing. The A4T should be recognizable at small scales (20-40px).

**Step 3: Final commit**

```
fix(rendering): polish A4T toolhead visual appearance
```
