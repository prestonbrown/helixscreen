# Architecture Guide

This document explains the HelixScreen prototype's system design, data flow patterns, and architectural decisions.

## Overview

HelixScreen uses a modern, declarative approach to embedded UI development that completely separates presentation from logic:

```
XML Layout Definitions (ui_xml/*.xml)
    ↓ bind_text / bind_value / bind_flag
Reactive Subject System (lv_subject_t)
    ↓ lv_subject_set_* / copy_*
C++ Application Logic (src/*.cpp)
```

**Key Innovation:** The entire UI is defined in XML files. C++ code only handles initialization and reactive data updates—zero layout or styling logic.

## Architectural Principles

### 1. Declarative UI Definition

All layout, styling, and component structure is defined in XML:

```xml
<!-- Complete panel definition in XML -->
<component>
  <view extends="lv_obj" style_bg_color="#bg_dark" style_pad_all="20">
    <lv_label text="Nozzle Temperature" style_text_color="#text_primary"/>
    <lv_label bind_text="temp_text" style_text_font="montserrat_28"/>
  </view>
</component>
```

**Benefits:**
- UI changes don't require recompilation
- Visual designers can modify layouts without C++ knowledge
- Complete separation of concerns between presentation and logic

### 2. Reactive Data Binding

LVGL 9's Subject-Observer pattern enables automatic UI updates:

```cpp
// C++ is pure logic - zero layout code
ui_panel_nozzle_init_subjects();
lv_xml_create(screen, "nozzle_panel", NULL);
ui_panel_nozzle_update(210);  // All bound widgets update automatically
```

**Benefits:**
- No manual widget searching or updating
- Type-safe data updates
- One update propagates to multiple UI elements
- Clean separation between data and presentation

### 3. LVGL Theme Integration

HelixScreen uses LVGL 9's built-in theme system for automatic widget styling:

**Architecture:** XML → C++ → LVGL Theme

```xml
<!-- ui_xml/globals.xml - Single source of truth for theme values -->
<consts>
  <color name="primary_color" value="..."/>
  <color name="secondary_color" value="..."/>

  <!-- Theme-specific color variants for light/dark mode -->
  <color name="app_bg_color_light" value="..."/>
  <color name="app_bg_color_dark" value="..."/>
  <color name="text_primary_light" value="..."/>
  <color name="text_primary_dark" value="..."/>
  <color name="header_text_light" value="..."/>
  <color name="header_text_dark" value="..."/>

  <str name="font_body" value="..."/>
  <str name="font_heading" value="..."/>
  <str name="font_small" value="..."/>
</consts>
```

```cpp
// src/ui_theme.cpp - Reads XML constants at runtime
void ui_theme_init(lv_display_t* display, bool dark_mode) {
    // Read light/dark color variants from XML (NO hardcoded colors!)
    const char* bg_light = lv_xml_get_const(NULL, "app_bg_color_light");
    const char* bg_dark = lv_xml_get_const(NULL, "app_bg_color_dark");

    // Override runtime constant based on theme preference
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    lv_xml_register_const(scope, "app_bg_color", dark_mode ? bg_dark : bg_light);

    // Initialize LVGL default theme
    lv_theme_default_init(display, primary_color, secondary_color, dark_mode, base_font);
}
```

**Benefits:**
- ✅ **No recompilation needed** - Edit `globals.xml` to change theme colors
- ✅ **Automatic styling** - Widgets inherit coordinated styles from theme
- ✅ **Dark/Light mode** - Runtime theme switching support
- ✅ **Responsive padding** - Theme auto-adjusts spacing based on screen resolution
- ✅ **State-based styling** - Automatic pressed/disabled/checked states

**Theme Customization:**
- **Colors:** `primary_color`, `secondary_color`, `text_primary`, `text_secondary` defined in globals.xml
- **Fonts:** `font_heading`, `font_body`, `font_small` for manual widget styling when needed
- **Mode:** Dark/light mode controlled via config file or command-line flags

**Config Persistence:**
Theme preference saved to `helixconfig.json` and restored on next launch:
```json
{
  "dark_mode": true,
  ...
}
```

## Component Hierarchy

```
app_layout.xml
├── navigation_bar.xml      # 5-button vertical navigation
└── content_area
    ├── home_panel.xml       # Print status overview
    ├── controls_panel.xml   # Motion/temperature/extrusion launcher
    │   ├── motion_panel.xml
    │   ├── nozzle_temp_panel.xml
    │   ├── bed_temp_panel.xml
    │   └── extrusion_panel.xml
    ├── print_select_panel.xml
    ├── filament_panel.xml
    ├── settings_panel.xml
    └── advanced_panel.xml
```

**Design Patterns:**
- **App Layout** - Root container with navigation + content area
- **Panel Components** - Self-contained UI screens with reactive data
- **Sub-Panel Overlays** - Motion/temp controls that slide over main content
- **Global Navigation** - Persistent 5-button navigation bar

## Data Flow Architecture

### Subject Initialization Pattern

**Critical:** Subjects must be initialized BEFORE creating XML:

```cpp
// 1. Register XML components
lv_xml_register_component_from_file("A:/ui_xml/globals.xml");
lv_xml_register_component_from_file("A:/ui_xml/home_panel.xml");

// 2. Initialize subjects (BEFORE XML creation)
ui_nav_init();
ui_panel_home_init_subjects();

// 3. NOW create UI - bindings will find initialized subjects
lv_xml_create(screen, "app_layout", NULL);
```

**Why this order matters:**
- XML bindings look up subjects by name during creation
- If subjects don't exist, bindings fail silently with empty values
- C++ initialization creates subjects with proper default values

### Reactive Update Flow

```cpp
// Business logic updates subject
lv_subject_set_string(temp_text_subject, "210°C");

// LVGL automatically:
// 1. Notifies all observers (bound widgets)
// 2. Updates widget properties (text, values, flags)
// 3. Triggers redraws as needed

// Zero manual widget management required
```

### Event Handling Pattern

XML defines event bindings, C++ implements handlers:

```xml
<!-- XML: Declarative event binding -->
<lv_button>
    <event_cb trigger="clicked" callback="on_temp_increase"/>
</lv_button>
```

```cpp
// C++: Pure business logic
void on_temp_increase(lv_event_t* e) {
    int current = get_target_temp();
    set_target_temp(current + 5);

    // UI updates automatically via subject binding
    lv_subject_set_int(temp_target_subject, current + 5);
}
```

## Memory Management

### Subject Lifecycle

- **Creation:** During `ui_*_init_subjects()` functions
- **Lifetime:** Persistent throughout application runtime
- **Updates:** Via `lv_subject_set_*()` functions from any thread
- **Cleanup:** Automatic when application exits

### Widget Management

- **Creation:** Automatic during `lv_xml_create()`
- **Lifetime:** Managed by LVGL parent-child hierarchy
- **Updates:** Automatic via subject-observer bindings
- **Cleanup:** Automatic when parent objects are deleted

### LVGL Memory Patterns

LVGL uses automatic memory management:
- Widget memory allocated during creation
- Parent widgets automatically free child widgets
- No manual `free()` calls needed for UI elements
- Use LVGL's built-in reference counting for shared resources

## Thread Safety

### ⚠️ CRITICAL: LVGL Main Thread Requirement

**LVGL is NOT thread-safe.** All widget creation and modification MUST happen on the main thread.

### Safe Operations from Any Thread

**Subject updates are thread-safe:**
```cpp
// Safe from any thread
void update_temperature_from_background(int temp) {
    lv_subject_set_int(temp_current_subject, temp);
    // UI updates will happen on next LVGL timer cycle
}
```

**Why this works:** Subjects use internal locking and defer UI updates to the main thread.

### Unsafe Operations (Main Thread Only)

**Widget manipulation requires main thread:**
```cpp
// Main thread only
void handle_ui_event(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKED);  // NOT safe from background threads
}
```

### Backend Integration Pattern: lv_async_call()

**Problem:** Backend threads (networking, file I/O, WiFi scanning) need to update UI but cannot call LVGL APIs directly.

**Solution:** Use `lv_async_call()` to marshal widget updates to the main thread:

```cpp
// Backend callback running in std::thread
void WiFiManager::handle_scan_complete(const std::string& data) {
    // Parse results (safe - no LVGL calls)
    auto networks = parse_networks(data);

    // Create data for dispatch
    struct CallbackData {
        std::vector<WiFiNetwork> networks;
        std::function<void(const std::vector<WiFiNetwork>&)> callback;
    };
    auto* cb_data = new CallbackData{networks, scan_callback_};

    // Dispatch to LVGL main thread
    lv_async_call([](void* user_data) {
        auto* data = static_cast<CallbackData*>(user_data);

        // NOW safe to create/modify widgets
        data->callback(data->networks);  // Calls populate_network_list()

        delete data;  // Clean up
    }, cb_data);
}
```

**Key Points:**
1. **Backend thread:** Parse data, prepare callback data structure
2. **lv_async_call():** Queues lambda to execute on main thread
3. **Main thread lambda:** Creates/modifies widgets safely
4. **Memory management:** Heap-allocate data, delete in lambda

**Without this pattern:** Race conditions, segfaults, undefined behavior when backend thread creates widgets while LVGL is rendering.

**Reference Implementation:** `src/wifi_manager.cpp:102-190` (all event handlers use this pattern)

### When to Use lv_async_call()

✅ **Use when:**
- Background thread needs to create widgets
- Backend callback needs to call `lv_obj_*()` functions
- Network/file operations complete and need to update UI

❌ **Don't need when:**
- Already on main thread (event handlers, timers)
- Only updating subjects (they're thread-safe)
- No LVGL API calls in the callback

## LVGL Configuration

### Required Features

Key settings in `lv_conf.h` for XML support:

```c
#define LV_USE_XML 1                           // Enable XML UI support
#define LV_USE_SNAPSHOT 1                      // Enable screenshot API
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 1     // Required by XML parser
#define LV_FONT_MONTSERRAT_16 1                // Text fonts
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
```

### Display Driver Integration

**SDL2 Simulator:**
- Uses `lv_sdl_window_create()` for desktop development
- Automatic event handling via SDL2 backend
- Multi-display positioning support via environment variables

**Future Embedded Targets:**
- Framebuffer driver for direct hardware rendering
- Touch input via evdev integration
- Same XML/Subject code runs unchanged

## Design Decisions & Trade-offs

### Why XML Instead of Code?

**Advantages:**
- ✅ Rapid iteration without recompilation
- ✅ Designer-friendly declarative syntax
- ✅ Complete separation of presentation and logic
- ✅ Global theming capabilities
- ✅ Reduced C++ complexity

**Trade-offs:**
- ❌ XML support is experimental in LVGL 9
- ❌ Additional layer of abstraction
- ❌ Limited debugging tools for XML issues
- ❌ Requires UTF-8 encoding for all files

**Verdict:** The benefits outweigh the trade-offs for a touch UI where visual design changes frequently.

### Why Subject-Observer Instead of Direct Widget Updates?

**Advantages:**
- ✅ One data change updates multiple UI elements
- ✅ Type-safe data binding
- ✅ Automatic UI consistency
- ✅ Easier to test business logic separately

**Trade-offs:**
- ❌ Additional conceptual complexity
- ❌ Indirect relationship between data and UI
- ❌ Subject name string matching (not compile-time checked)

**Verdict:** The reactive pattern scales better as UI complexity grows and provides cleaner separation of concerns.

### Why LVGL 9 Instead of Native Platform UI?

**Advantages:**
- ✅ Single codebase for all platforms
- ✅ Embedded-optimized (low memory, no GPU required)
- ✅ Touch-first design patterns
- ✅ Extensive widget library
- ✅ Active development and community

**Trade-offs:**
- ❌ Custom look-and-feel (not native platform appearance)
- ❌ Learning curve for LVGL-specific patterns
- ❌ Limited platform integration (no native menus, etc.)

**Verdict:** Perfect for embedded touch interfaces where native platform UI isn't available or suitable.

## Critical Implementation Patterns

### Component Instantiation Names

**Always add explicit `name` attributes** to component instantiations:

```xml
<!-- app_layout.xml -->
<lv_obj name="content_area">
  <controls_panel name="controls_panel"/>  <!-- Explicit name required -->
  <home_panel name="home_panel"/>
</lv_obj>
```

**Why:** Component names in `<view name="...">` definitions do NOT propagate to `<component_tag/>` instantiations. Without explicit names, `lv_obj_find_by_name()` returns NULL.

### Widget Lookup by Name

Use `lv_obj_find_by_name()` instead of index-based child access:

```cpp
// In XML: <lv_label name="temp_display" bind_text="temp_text"/>
// In C++:
lv_obj_t* label = lv_obj_find_by_name(panel, "temp_display");
if (label != NULL) {
    // Safe to use widget
}
```

**Benefits:**
- Robust against XML layout changes
- Self-documenting code
- Explicit error handling when widgets don't exist

### Image Scaling in Flex Layouts

**When scaling images immediately after layout changes**, call `lv_obj_update_layout()` first:

```cpp
// WRONG: Container reports 0x0 size
lv_coord_t w = lv_obj_get_width(container);  // Returns 0
ui_image_scale_to_cover(img, container);     // Fails

// CORRECT: Force layout calculation first
lv_obj_update_layout(container);
lv_coord_t w = lv_obj_get_width(container);  // Returns actual size
ui_image_scale_to_cover(img, container);     // Works correctly
```

**Why:** LVGL uses deferred layout calculation for performance. Immediate size queries after layout changes return stale values.

### Navigation History Stack

Use the navigation system for consistent overlay management:

```cpp
// When showing overlay
ui_nav_push_overlay(motion_panel);  // Pushes current to history, shows overlay

// In back button callback
if (!ui_nav_go_back()) {
    // Fallback: manual navigation if history is empty
    ui_nav_show_panel(home_panel);
}
```

**Benefits:**
- Automatic history management
- Consistent back button behavior
- State preservation when navigating back

## Performance Characteristics

### XML Parsing Performance

- **One-time cost** during application startup
- Component registration is fast (simple file parsing)
- Widget creation is standard LVGL performance
- **No runtime XML parsing** after initialization

### Subject Update Performance

- **O(n) complexity** where n = number of bound widgets
- Optimized for small numbers of observers per subject
- **Batched updates** - multiple subject changes before next redraw
- **Efficient for typical UI** with 10-50 bound elements per panel

### Memory Footprint

- **Minimal XML overhead** - parsed structure discarded after creation
- **Subject storage** - ~100 bytes per subject (reasonable for 50-100 subjects)
- **Widget memory** - standard LVGL allocation patterns
- **Total overhead** - estimated <10KB for XML/Subject systems

## Future Architecture Considerations

### Moonraker Integration

The architecture is designed to accommodate real-time printer data:

```cpp
// WebSocket data updates subjects
void on_printer_temp_update(float nozzle, float bed) {
    lv_subject_set_string(nozzle_temp_subject, format_temp(nozzle));
    lv_subject_set_string(bed_temp_subject, format_temp(bed));
    // UI updates automatically
}
```

### State Persistence

Subject values can be saved/restored for session persistence:

```cpp
void save_ui_state() {
    config["target_temp"] = lv_subject_get_int(temp_target_subject);
    config["panel_mode"] = lv_subject_get_string(nav_current_subject);
}

void restore_ui_state() {
    lv_subject_set_int(temp_target_subject, config["target_temp"]);
    lv_subject_set_string(nav_current_subject, config["panel_mode"]);
}
```

### Platform Adaptation

The XML/Subject architecture adapts easily to different display sizes:

- **Small displays** - Use `style_flex_flow="column"` for vertical layouts
- **Large displays** - Use `style_flex_flow="row"` for horizontal layouts
- **Theme scaling** - Adjust font sizes and padding in `globals.xml`
- **Content adaptation** - Show/hide elements with conditional flag bindings

## Related Documentation

- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build system and daily workflow
- **[CONTRIBUTING.md](CONTRIBUTING.md)** - Code standards and patterns
- **[LVGL 9 XML Guide](docs/LVGL9_XML_GUIDE.md)** - Complete XML syntax reference
- **[Quick Reference](docs/QUICK_REFERENCE.md)** - Common patterns and examples
- **[BUILD_SYSTEM.md](docs/BUILD_SYSTEM.md)** - Build configuration and patches