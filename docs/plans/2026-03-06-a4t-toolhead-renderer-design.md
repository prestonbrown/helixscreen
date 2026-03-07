# A4T Toolhead Renderer + User-Selectable Style

**Date**: 2026-03-06
**Status**: Approved

## Summary

Add an Armored Turtle (A4T) toolhead renderer and convert the toolhead style system from a boolean (Bambu vs Stealthburner) to a user-selectable enum with auto-detection fallback.

## Renderer Design

### Visual (1000x1000 design space)

- Dark rectangular body (#1A1A1A) with beveled bottom corners
- Extruder block on top (narrower, #2A2A2A) with gear circle detail
- Central fan circle (dark)
- 8-10 hexagonal honeycomb accents in A4T green (#B5CC18) flanking the fan
- Nozzle tip via `nr_draw_nozzle_tip()`

### Rendering approach

Hybrid: polygons for body/extruder outlines (ear-clipping triangulation from faceted renderer), procedural `draw_hexagon()` for regular hex shapes, `draw_circle()` for fan.

### Files

- `include/nozzle_renderer_a4t.h` (already created)
- `src/rendering/nozzle_renderer_a4t.cpp`

## Selection System

### Enum

```cpp
enum class ToolheadStyle { AUTO = 0, DEFAULT = 1, STEALTHBURNER = 2, A4T = 3 };
```

### SettingsManager (follows ZMovementStyle pattern)

- Subject: `lv_subject_t toolhead_style_subject_`
- Config path: `/appearance/toolhead_style`
- Methods: `get_toolhead_style()`, `set_toolhead_style()`, `get_effective_toolhead_style()`
- Options string: `"Auto\nDefault\nStealthburner\nArmored Turtle"`
- `get_effective_toolhead_style()` resolves AUTO using `PrinterDetector::is_voron_printer()` (returns STEALTHBURNER for Voron, DEFAULT otherwise — A4T not auto-detected yet)

## Integration Changes

### Canvas structs

Replace `bool use_faceted_toolhead` with `ToolheadStyle toolhead_style` in:
- `ui_filament_path_canvas.cpp`
- `ui_system_path_canvas.cpp`
- `ui_z_offset_indicator.cpp`

### Setter functions

`set_faceted_toolhead(obj, bool)` → `set_toolhead_style(obj, ToolheadStyle)` in canvas headers/impls.

### Render dispatch

```cpp
switch (style) {
    case ToolheadStyle::STEALTHBURNER: draw_nozzle_faceted(...); break;
    case ToolheadStyle::A4T:           draw_nozzle_a4t(...); break;
    default:                           draw_nozzle_bambu(...); break;
}
```

### Callers

Replace `is_voron_printer()` checks with `get_effective_toolhead_style()` in:
- `ui_ams_detail.cpp`
- `ui_panel_ams_overview.cpp`

### Settings UI

Add `<setting_dropdown_row>` to `settings_panel.xml` with callback `on_toolhead_style_changed`.
