# Architecture Guide

This document explains the HelixScreen prototype's system design, data flow patterns, and architectural decisions.

> **Visual diagrams:** See [`docs/architecture/`](../architecture/README.md) for Mermaid and D2 diagrams covering system overview, data flow, threading, UI layer, startup sequence, and singleton map.

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
nozzle_panel.init_subjects();
lv_xml_create(screen, "nozzle_panel", NULL);
nozzle_panel.set_temp(210);  // All bound widgets update automatically
```

**Benefits:**
- No manual widget searching or updating
- Type-safe data updates
- One update propagates to multiple UI elements
- Clean separation between data and presentation

### 3. Custom HelixScreen Theme

HelixScreen uses a custom LVGL theme that wraps the default theme for enhanced styling:

**Architecture:** XML → C++ → Custom Theme → LVGL Default Theme

```xml
<!-- ui_xml/globals.xml - Single source of truth for theme values -->
<consts>
  <color name="primary_color" value="..."/>
  <color name="secondary_color" value="..."/>

  <!-- Theme-specific color variants for light/dark mode -->
  <color name="app_bg_color_light" value="..."/>
  <color name="app_bg_color_dark" value="..."/>
  <color name="card_bg_light" value="..."/>
  <color name="card_bg_dark" value="..."/>
  <color name="text_primary_light" value="..."/>
  <color name="text_primary_dark" value="..."/>

  <str name="font_body" value="..."/>
  <str name="font_heading" value="..."/>
</consts>
```

```cpp
// src/helix_theme.c - Custom theme wrapper
static void helix_theme_apply_cb(lv_theme_t* theme, lv_obj_t* obj) {
    // Apply default theme first
    lv_theme_apply(helix_theme->default_theme, obj);

    // Override input widgets with computed background color
    if(lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &helix_theme->input_bg_style, 0);
    }
    // Similar for dropdown, roller, spinbox...
}
```

```cpp
// src/ui/ui_theme.cpp - Initializes custom theme
void ui_theme_init(lv_display_t* display, bool dark_mode) {
    // Read colors from XML (NO hardcoded colors!)
    lv_color_t card_bg = parse_color(dark_mode ? card_bg_dark : card_bg_light);

    // Initialize custom HelixScreen theme (wraps default)
    lv_theme_t* theme = helix_theme_init(display, primary, secondary,
                                          dark_mode, font, screen_bg, card_bg, grey);
    lv_display_set_theme(display, theme);
}
```

**Key Features:**
- ✅ **No recompilation needed** - Edit `globals.xml` to change theme colors
- ✅ **Automatic styling** - Input widgets get computed backgrounds automatically
- ✅ **Computed colors** - Input backgrounds are lighter/darker than cards based on mode
- ✅ **Responsive** - Scales padding/sizing for different screen resolutions
- ✅ **Maintainable** - Uses LVGL public API, no fragile private structure patching
- ✅ **Dark/Light mode** - Runtime theme switching support
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

## Namespace Organization

All HelixScreen code lives under the `helix::` namespace:

| Namespace | Contents | Example |
|-----------|----------|---------|
| `helix::` | Core singletons, state, managers | `helix::PrinterState`, `helix::Config` |
| `helix::ui::` | UI functions, update queue | `helix::ui::queue_update()` |

**Rules:**
- **No `using` declarations in headers** — always use fully-qualified names in `.h` files
- **`using namespace helix;`** is acceptable in `.cpp` files only
- **Enum classes** — all enums use `enum class` within `helix::` (e.g., `helix::PanelId`, `helix::PrintState`)
- **JSON alias** — consolidated in `json_fwd.h` (`#include "json_fwd.h"` for forward declaration)

## ⚠️ CRITICAL: Reactive-First Principle - "The HelixScreen Way"

**ALL UI control MUST be reactive via subjects. Direct widget manipulation is an anti-pattern.**

### ✅ The Correct Way: Reactive UI Control

Control UI elements by updating subjects in C++, binding to them in XML:

```cpp
// C++ - Pure data updates, zero widget manipulation
lv_subject_t connection_test_passed;
lv_subject_init_int(&connection_test_passed, 0);  // Button starts disabled

// Later: Update subject when connection succeeds
lv_subject_set_int(&connection_test_passed, 1);  // Button becomes enabled automatically
```

```xml
<!-- XML - Reactive bindings control UI state -->
<lv_button name="next_button">
  <!-- Button automatically updates when subject changes -->
  <lv_obj-bind_flag_if_eq subject="connection_test_passed" flag="clickable" ref_value="0" negate="true"/>
  <lv_obj-bind_flag_if_eq subject="connection_test_passed" flag="user_1" ref_value="0"/>
</lv_button>
<lv_style selector="LV_STATE_USER_1" style_opa="128"/>  <!-- Disabled style -->
```

**Benefits:**
- ✅ UI automatically stays in sync with application state
- ✅ Zero manual widget searching/updating
- ✅ Testable - can verify subject values without UI
- ✅ Reusable - multiple widgets can bind to same subject

###  ❌ ANTI-PATTERN: Direct Widget Manipulation

**DO NOT** search for widgets by name/ID and manipulate them from C++:

```cpp
// ❌ WRONG - Direct widget manipulation (ANTI-PATTERN)
lv_obj_t* button = lv_obj_find_by_name(screen, "next_button");
lv_obj_add_state(button, LV_STATE_DISABLED);      // Manual state management
lv_obj_set_style_opa(button, 128, 0);              // Manual styling

// ❌ WRONG - Searching for labels to update text
lv_obj_t* label = lv_obj_find_by_name(panel, "temp_display");
lv_label_set_text(label, "210°C");                 // Manual text update
```

**Why this is wrong:**
- ❌ Couples C++ code to specific widget names/structure
- ❌ Breaks if XML layout changes
- ❌ Difficult to test (requires UI to exist)
- ❌ Fragile - easy to forget updates, cause inconsistent state
- ❌ Violates separation of concerns

### Reactive Patterns for Common UI Tasks

| UI Task | ❌ Anti-Pattern | ✅ Reactive Way |
|---------|----------------|----------------|
| Update text | `lv_label_set_text(label, "...")` | `bind_text` in XML, `lv_subject_set_string()` in C++ |
| Enable/disable | `lv_obj_add_state(obj, DISABLED)` | `bind_flag_if_eq` in XML, update subject in C++ |
| Show/hide | `lv_obj_add_flag(obj, HIDDEN)` | `bind_flag_if_eq` for `hidden` flag |
| Update value | `lv_slider_set_value(slider, val)` | `bind_value` in XML, `lv_subject_set_int()` in C++ |
| Visual feedback | Manual style changes | `bind_flag_if_eq` + conditional styles |

### When Direct Access IS Acceptable

The ONLY acceptable use of `lv_obj_find_by_name()` is during **initialization** for special cases:

```cpp
// ✅ OK - One-time initialization during panel setup
void ui_panel_init() {
    lv_obj_t* dropdown = lv_obj_find_by_name(panel, "hardware_dropdown");
    ui_dropdown_populate(dropdown, get_available_hardware());  // One-time setup
}
```

**After initialization, ALL updates must be reactive.**

## Component Hierarchy

```
app_layout.xml
├── navigation_bar.xml      # 6-button vertical navigation
└── content_area            # 6 panels + 25+ overlays (see ui_xml/*.xml)
```

**Design Patterns:**
- **App Layout** - Root container with navigation + content area
- **Panel Components** - Self-contained UI screens with reactive data
- **Sub-Panel Overlays** - Motion/temp controls that slide over main content
- **Global Navigation** - Persistent 6-button navigation bar

## ⚠️ PREFERRED: Class-Based Architecture

**All new code should use class-based patterns.** This applies to:
- **UI panels** (PanelBase for main panels, OverlayBase for overlays)
- **Modals** (Modal)
- **Backend managers** (WiFiManager, EthernetManager, MoonrakerClient)
- **Domain state classes** (Printer*State decomposition)
- **Services and utilities**

### Why Class-Based?

| Benefit | Description |
|---------|-------------|
| **RAII** | Resources acquired in constructor, released in destructor - no leaks |
| **Encapsulation** | State and behavior together, clear ownership |
| **Testability** | Mock via interface inheritance, isolate dependencies |
| **Lifecycle** | Explicit init/start/stop/destroy - no hidden state |

**For implementation examples, see [DEVELOPER_QUICK_REFERENCE.md](DEVELOPER_QUICK_REFERENCE.md#class-patterns).**

### Panel & Overlay Base Classes

All main panels inherit from `PanelBase`, overlays from `OverlayBase`:

```cpp
class MyPanel : public PanelBase {
public:
    MyPanel();
    ~MyPanel() override;

    void init_subjects() override;   // Create subjects + self-register cleanup
    void setup(lv_obj_t* panel, lv_obj_t* parent) override;
    void on_activate() override;
    void on_deactivate() override;

private:
    lv_subject_t my_subject_{};
    char buf_[128]{};         // Static storage for string subjects
};
```

**Key features:**
- Two-phase init: `init_subjects()` first, then `setup()` after XML creation
- `register_observer()` tracks all observers via `ObserverGuard` for RAII cleanup
- Lifecycle hooks: `on_activate()` / `on_deactivate()` called by `NavigationManager`
- Dependency injection: receives `PrinterState&` and `MoonrakerAPI*`
- Self-registration: `init_subjects()` MUST register cleanup with `StaticSubjectRegistry`

### ❌ AVOID: Function-Based Patterns

The old C-style wrapper APIs (`ui_panel_*_init()`, `ui_panel_*_show()`) have been **removed**. All panels and overlays use class-based patterns:

```cpp
// ✅ Current pattern - class-based with lifecycle
auto& motion = MotionPanel::instance();
motion.init_subjects();
// ... XML creation ...
motion.on_activate();
```

## Domain Decomposition: PrinterState

The central `PrinterState` class was decomposed into 13 focused domain classes, each owning LVGL subjects for a specific concern. `PrinterState` delegates to these via composition, keeping its public API but distributing the implementation.

```
PrinterState (orchestrator)
├── PrinterTemperatureState      # Nozzle, bed, chamber temps + targets
│   └── ExtruderInfo[]           # Per-extruder: name, temp/target subjects (heap-allocated)
├── PrinterMotionState           # Position, speed, homed axes
├── PrinterFanState              # Fan speeds, types
├── PrinterPrintState            # Print progress, filename, layers, ETA
├── PrinterCalibrationState      # PID, Z-offset, bed mesh status
├── PrinterCapabilitiesState     # QGL, probe, firmware features
├── PrinterExcludedObjectsState  # Object exclusion during prints
├── PrinterNetworkState          # WiFi, Ethernet, hostname
├── PrinterVersionsState         # Klipper, MCU, Moonraker versions
├── PrinterLedState              # LED controls and effects
├── PrinterHardwareValidationState  # Hardware health checks
├── PrinterPluginStatusState     # Plugin system state
└── PrinterCompositeVisibilityState # UI visibility rules
```

### Why Domain Decomposition?

| Concern | Before (God Class) | After (Domains) |
|---------|---------------------|------------------|
| **Lines** | 1514 in one file | ~100-200 per domain |
| **Subjects** | All mixed together | Grouped by concern |
| **Testing** | Hard to isolate | Test each domain independently |
| **Threading** | One big mutex | Domain-scoped thread safety |
| **Navigation** | Scroll 1500 lines | Find the right 150-line file |

### Domain Class Pattern

Each domain class follows a consistent structure:

```cpp
class PrinterTemperatureState {
public:
    void init_subjects();                     // Initialize LVGL subjects
    void deinit_subjects();                   // Clean shutdown
    void reset_for_testing();                 // Reset for unit tests

    // Subject accessors (for binding in XML or observers)
    lv_subject_t* nozzle_temp_subject();
    lv_subject_t* bed_temp_subject();

    // Setters (called from WebSocket thread via helix::ui::queue_update)
    void set_nozzle_temp(int temp);
    void set_bed_temp(int temp);

private:
    lv_subject_t nozzle_temp_{};
    lv_subject_t bed_temp_{};
    bool initialized_ = false;
};
```

**Key rules:**
- Domain classes own their subjects (init/deinit lifecycle)
- `PrinterState` forwards public API calls to the appropriate domain
- Callers never need to know about domain classes directly
- Each domain registers with `StaticSubjectRegistry` for shutdown cleanup

**Files:** `include/printer_*_state.h`, `src/printer/printer_*_state.cpp`

### ToolState Singleton

`ToolState` (`include/tool_state.h`) manages tool information for multi-tool printers (tool changers, multi-extruder setups). It is a standalone singleton, separate from `PrinterState`, because tool tracking spans both temperature and filament management domains.

**Lifecycle:**

```
init_subjects()                    Register LVGL subjects (active_tool, tool_count, tools_version)
    ↓
init_tools(PrinterDiscovery)       Populate ToolInfo vector from discovered "tool T*" objects
    ↓
update_from_status(json)           Called on each Moonraker status update (tool states, offsets)
    ↓
deinit_subjects()                  Clean shutdown before lv_deinit()
```

**Key types:**

```cpp
enum class DetectState { PRESENT, ABSENT, UNAVAILABLE };

struct ToolInfo {
    int index;                          // Tool number (0, 1, 2, ...)
    std::string name;                   // "T0", "T1", etc.
    std::optional<std::string> extruder_name;  // Associated extruder
    std::optional<std::string> heater_name;    // Override heater
    float gcode_x_offset, gcode_y_offset, gcode_z_offset;
    bool active, mounted;
    DetectState detect_state;
    int backend_index;                  // AMS backend index (-1 = direct drive)
    int backend_slot;                   // Slot in that backend (-1 = dynamic)
};
```

**Subjects:**

| Subject | Type | Description |
|---------|------|-------------|
| `active_tool` | int | Currently active tool index (0-based) |
| `tool_count` | int | Number of discovered tools |
| `tools_version` | int | Bumped when tool list changes (UI rebuild trigger) |

**Single-tool fallback:** On non-toolchanger printers, `ToolState` holds a single implicit T0 entry. UI code can always query `ToolState::tool_count()` to decide whether to show multi-tool controls.

**Files:** `include/tool_state.h`, `src/printer/tool_state.cpp`

---

## Panel Widget System

The home panel exposes a row of modular "widgets" — small cards that each display one aspect of printer state: fan speeds, temperatures, LED, power, network, thermistors, and more. Each widget is a self-contained C++ object that owns its own XML component and observer lifecycle.

### What Are PanelWidgets?

PanelWidgets live in `src/ui/panel_widgets/` (implementations) and headers alongside each `.cpp`. Current widgets:

| Widget ID | Class | Displays |
|-----------|-------|---------|
| `fan_stack` | `FanStackWidget` | Part / hotend / aux fan speeds with spinning icon animations |
| `temp_stack` | `TempStackWidget` | Nozzle and bed temperatures |
| `temperature` | `TemperatureWidget` | Single temperature display |
| `thermistor` | `ThermistorWidget` | Thermistor sensor readings |
| `led` | `LedWidget` | LED on/off toggle with brightness-reactive icon |
| `power` | `PowerWidget` | Power device toggle |
| `network` | `NetworkWidget` | Network connection status |

Widgets that are pure XML data binding (filament, probe, humidity, etc.) do NOT need a `PanelWidget` subclass — they work via subject bindings defined in their XML component alone.

### PanelWidget Base Class

`include/panel_widget.h` defines the interface:

```cpp
class PanelWidget {
public:
    virtual void init_subjects() {}       // Create subjects before lv_xml_create()
    virtual void set_config(const nlohmann::json& config) {}  // Per-widget config
    virtual std::string get_component_name() const;           // XML component to create
    virtual void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) = 0;
    virtual void detach() = 0;            // Reset observers, clear pointers
    virtual void on_activate() {}         // Panel became visible
    virtual void on_deactivate() {}       // Panel went offscreen
    virtual void set_row_density(size_t widgets_in_row) {}    // Adjust for layout density
    virtual const char* id() const = 0;  // Stable widget ID string
};
```

### Widget Factory Pattern

Each widget registers a factory function at startup via `register_widget_factory()`. The registry (`include/panel_widget_registry.h`) pairs an ID string with a factory lambda:

```cpp
// From fan_stack_widget.cpp — called once at startup
void register_fan_stack_widget() {
    register_widget_factory("fan_stack", []() {
        auto& ps = get_printer_state();
        return std::make_unique<FanStackWidget>(ps);
    });

    // XML event callbacks must be registered before any XML is parsed
    lv_xml_register_event_cb(nullptr, "on_fan_stack_clicked", FanStackWidget::on_fan_stack_clicked);
}
```

`PanelWidgetManager` (`include/panel_widget_manager.h`) is the central coordinator:
- `init_widget_subjects()` — calls each widget's `init_subjects()` before XML creation
- `populate_widgets(panel_id, container)` — creates XML components and calls `attach()` for each enabled widget
- `setup_gate_observers(panel_id, rebuild_cb)` — observes hardware availability subjects; rebuilds the widget row when capabilities change
- `notify_config_changed(panel_id)` — triggers a rebuild after config changes (e.g., widget reorder)

### HomePanel Integration

`HomePanel::populate_widgets()` (`src/ui/ui_panel_home.cpp`) delegates entirely to `PanelWidgetManager`:

```cpp
void HomePanel::populate_widgets() {
    lv_obj_t* container = lv_obj_find_by_name(panel_, "widget_container");

    // Detach and clear previous widgets
    for (auto& w : active_widgets_) { w->detach(); }
    active_widgets_.clear();

    // Manager handles XML creation, factory invocation, and attach()
    active_widgets_ = PanelWidgetManager::instance().populate_widgets("home", container);

    cache_widget_references();
}
```

Gate observers call `populate_widgets()` automatically when hardware capabilities or klippy state change, so the widget row adapts to the connected printer without any manual dispatch.

### Version-Observer Self-Binding Pattern

The key architectural pattern used by interactive widgets: instead of HomePanel notifying each widget when hardware discovery completes, widgets observe a **version subject** that bumps whenever the relevant hardware list changes. On each bump, the widget calls its `bind_*()` method to reset and recreate all per-item observers.

**Why this approach:**
- Widgets are fully self-contained — no external dispatch needed
- The version subject fires immediately on `observe_int_sync()` registration, giving an initial bind with no special init path
- Reconnection / rediscovery automatically re-binds to the new hardware

**FanStackWidget example:**

```cpp
void FanStackWidget::attach_stack(lv_obj_t* /*widget_obj*/) {
    // ...cache label pointers...

    // Observe fans_version to re-bind when fans are discovered or change
    version_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        printer_state_.get_fans_version_subject(), this,
        [weak_alive](FanStackWidget* self, int /*version*/) {
            if (weak_alive.expired()) return;
            self->bind_fans();  // Reset observers, read current fans, create new observers
        });
}

void FanStackWidget::bind_fans() {
    // 1. Reset existing per-fan observers
    part_observer_.reset();
    hotend_observer_.reset();
    aux_observer_.reset();

    // 2. Read current hardware config
    const auto& fans = printer_state_.get_fans();
    if (fans.empty()) return;

    // 3. Create new per-fan observers using SubjectLifetime for dynamic subject safety
    SubjectLifetime lifetime;
    lv_subject_t* subject = printer_state_.get_fan_speed_subject(part_fan_name_, lifetime);
    part_observer_ = helix::ui::observe_int_sync<FanStackWidget>(
        subject, this, [weak_alive](FanStackWidget* self, int speed) {
            if (weak_alive.expired()) return;
            self->update_label(self->part_label_, speed);
        }, lifetime);
}
```

**LedWidget example** — observes `led_config_version_` on `LedController`:

```cpp
void LedWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    // ...

    // Fires immediately on add, triggering initial bind_led() — no separate init call needed
    led_version_observer_ = helix::ui::observe_int_sync<LedWidget>(
        led_ctrl.get_led_config_version_subject(), this,
        [weak_alive](LedWidget* self, int /*version*/) {
            if (weak_alive.expired()) return;
            self->bind_led();
        });
}
```

**PowerWidget** uses the same pattern but observes `power_device_count` instead of a version counter — the count itself changes whenever devices are discovered.

The old pattern (removed) was a `HomePanel::reload_from_config()` dispatch loop that called `PanelWidget::reload_from_config()` on each widget. The version-observer pattern replaced it: each widget is responsible for its own rebinding.

---

## Data Flow Architecture

### Subject Initialization Pattern

**Critical:** Subjects must be initialized BEFORE creating XML:

```cpp
// 1. Register XML components
lv_xml_register_component_from_file("A:/ui_xml/globals.xml");
lv_xml_register_component_from_file("A:/ui_xml/home_panel.xml");

// 2. Initialize subjects (BEFORE XML creation) — each self-registers cleanup
helix::NavigationManager::instance().init_subjects();
home_panel.init_subjects();

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

- **Creation:** During each class's `init_subjects()` method (e.g., `PrinterState::init_subjects()`, `HomePanel::init_subjects()`)
- **Lifetime:** Persistent throughout application runtime (static subjects) or until hardware changes (dynamic subjects — see below)
- **Updates:** Via `lv_subject_set_*()` functions from the main thread only; background threads must use `helix::ui::queue_update()`
- **Cleanup:** Each `init_subjects()` self-registers its `deinit_subjects()` with `StaticSubjectRegistry` or `StaticPanelRegistry`; explicit cleanup runs before `lv_deinit()`

### Dynamic Subject Safety (SubjectLifetime)

Per-fan, per-sensor, and per-extruder subjects are **dynamic** — they are destroyed and recreated when hardware is rediscovered after a disconnect/reconnect. Observing a dynamic subject without a `SubjectLifetime` token causes a **use-after-free crash**: `lv_subject_deinit()` frees the subject's observer list, but `ObserverGuard` still holds a dangling pointer into that list.

| ❌ Crash | ✅ Safe |
|----------|---------|
| `auto* s = state.get_fan_speed_subject(name);` | `SubjectLifetime lt;` |
| `obs = observe_int_sync(s, this, handler);` | `auto* s = state.get_fan_speed_subject(name, lt);` |
| | `obs = observe_int_sync(s, this, handler, lt);` |

**Dynamic subject sources** (always require a `SubjectLifetime` token when observing):

| Source | Method |
|--------|--------|
| `PrinterFanState` | `get_fan_speed_subject(name, lifetime)` — per-fan speeds |
| `TemperatureSensorManager` | `get_temp_subject(name, lifetime)` — per-sensor temperatures |
| `PrinterTemperatureState` | `get_extruder_temp_subject(name, lifetime)`, `get_extruder_target_subject(name, lifetime)` |

**Static subjects** (singleton lifetime, no token needed): `get_fan_speed_subject()` with no args, `get_bed_temp_subject()`, `get_nozzle_temp_subject()`, etc.

See `include/ui_observer_guard.h` for full `SubjectLifetime` documentation. For real usage, see `FanStackWidget::bind_fans()` in `src/ui/panel_widgets/fan_stack_widget.cpp`.

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

### ⚠️ **REQUIRED:** RAII for Custom Widget Memory

**MANDATORY PATTERN for all custom widgets that allocate memory:**

Custom widgets must use RAII (Resource Acquisition Is Initialization) for exception-safe memory management. Manual `lv_malloc/lv_free` is **forbidden** due to leak risks from exceptions or early returns.

**Required header:** `#include "ui_widget_memory.h"`

**Pattern 1: Widget user_data (most common):**

```cpp
// Widget state structure
struct MyWidgetState {
    int value;
    lv_obj_t* button;
};

// ✅ REQUIRED: Delete callback uses RAII wrapper
static void my_widget_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    // Transfer ownership to RAII wrapper - automatic cleanup
    lvgl_unique_ptr<MyWidgetState> state(
        (MyWidgetState*)lv_obj_get_user_data(obj)
    );
    lv_obj_set_user_data(obj, nullptr);

    // Even if cleanup code throws exceptions, state is freed
    cleanup_resources();
}

// ✅ REQUIRED: Widget creation uses RAII helper
lv_obj_t* my_widget_create(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) return nullptr;

    // Allocate using RAII helper
    auto state_ptr = lvgl_make_unique<MyWidgetState>();
    if (!state_ptr) {
        lv_obj_delete(obj);
        return nullptr;
    }

    // Get raw pointer for initialization
    MyWidgetState* state = state_ptr.get();
    state->value = 0;
    state->button = nullptr;

    // Transfer ownership to LVGL widget
    lv_obj_set_user_data(obj, state_ptr.release());

    // Register cleanup
    lv_obj_add_event_cb(obj, my_widget_delete_cb, LV_EVENT_DELETE, nullptr);

    return obj;
}
```

**Pattern 2: Standalone widget structures (e.g., ui_temp_graph):**

```cpp
// ✅ REQUIRED: Creation returns unique_ptr ownership
ui_temp_graph_t* ui_temp_graph_create(lv_obj_t* parent) {
    auto graph_ptr = std::make_unique<ui_temp_graph_t>();
    if (!graph_ptr) return nullptr;

    // Initialize...
    graph_ptr->chart = lv_chart_create(parent);
    if (!graph_ptr->chart) {
        return nullptr; // graph_ptr auto-freed
    }

    // Transfer ownership to caller
    return graph_ptr.release();
}

// ✅ REQUIRED: Destruction uses RAII wrapper
void ui_temp_graph_destroy(ui_temp_graph_t* graph) {
    if (!graph) return;

    // Transfer ownership to RAII wrapper
    std::unique_ptr<ui_temp_graph_t> graph_ptr(graph);

    // Cleanup LVGL widgets
    if (graph_ptr->chart) {
        lv_obj_del(graph_ptr->chart);
    }

    // graph_ptr automatically freed via ~unique_ptr()
}
```

**Pattern 3: Nested allocations (e.g., ui_step_progress):**

```cpp
struct WidgetData {
    char** label_buffers;  // Array of strings
    int count;
};

// Allocate nested arrays using RAII
auto data_ptr = lvgl_make_unique<WidgetData>();
auto label_buffers_ptr = lvgl_make_unique_array<char*>(count);

// Initialize...
data_ptr->label_buffers = label_buffers_ptr.get();
data_ptr->count = count;

for (int i = 0; i < count; i++) {
    auto label = lvgl_make_unique_array<char>(128);
    data_ptr->label_buffers[i] = label.release();
}

// Release ownership
label_buffers_ptr.release();
lv_obj_set_user_data(obj, data_ptr.release());
```

**Why RAII is mandatory:**
1. **Exception Safety:** If code between malloc and free throws, memory leaks
2. **Early Returns:** Manual free is skipped if function returns early
3. **Maintenance:** RAII is self-documenting and enforces cleanup
4. **Future-Proof:** Adding exception-throwing code won't introduce leaks

**❌ FORBIDDEN ANTI-PATTERN:**

```cpp
// ❌ WRONG: Manual malloc/free is NOT ALLOWED
lv_obj_t* my_widget_create(lv_obj_t* parent) {
    MyWidgetState* state = (MyWidgetState*)lv_malloc(sizeof(MyWidgetState));
    if (!state) return nullptr;

    // If this throws exception, state leaks!
    do_something_that_might_throw();

    lv_obj_set_user_data(obj, state);
    return obj;
}

static void my_widget_delete_cb(lv_event_t* e) {
    MyWidgetState* state = ...;

    // If this throws exception, state leaks!
    cleanup_resources();

    lv_free(state);  // ❌ Never reached if exception thrown
}
```

**See also:** `include/ui_widget_memory.h` for full API documentation

**Examples:**
- `src/ui/ui_jog_pad.cpp` - Simple widget user_data
- `src/ui/ui_step_progress.cpp` - Complex nested allocations
- `src/ui/ui_temp_graph.cpp` - Standalone structure with custom destroy

### Static Object Destructors and Logging

**Problem:** Static/global objects are destroyed during `exit()` in undefined order across translation units (static destruction order fiasco). If your destructor tries to use spdlog, it may crash because spdlog's global logger might already be destroyed.

**Solution:** Use `fprintf(stderr, ...)` instead of spdlog in destructors of static/global objects:

```cpp
MyManager::~MyManager() {
    // Use fprintf - spdlog may be destroyed during static cleanup
    fprintf(stderr, "[MyManager] Shutting down\n");
    cleanup_resources();
}
```

**When this applies:**
- Destructors of objects stored in static/global variables (e.g., `static std::unique_ptr<WiFiManager>`)
- Any destructor that might run during `exit()` or program termination

**Reference implementations:**
- `src/api/wifi_manager.cpp:71-72`
- `src/api/ethernet_manager.cpp:38-41`
- `src/api/ethernet_backend_*.cpp` (all backend destructors)

**Note:** This is separate from the weak_ptr pattern used for async callback safety - that protects against managers being explicitly destroyed via `.reset()` while async operations are queued.

### ⚠️ Timer Lifecycle Management

**LVGL timers are NOT automatically cleaned up.** Timers created with `lv_timer_create()` continue running until explicitly deleted with `lv_timer_delete()`. If the object passed as `user_data` is destroyed without deleting the timer, the timer will fire with a dangling pointer causing use-after-free crashes.

**Recommended: Use LvglTimerGuard RAII wrapper**

```cpp
#include "ui_timer_guard.h"

class MyPanel {
    LvglTimerGuard update_timer_;

    void start_updates() {
        // Timer automatically deleted when MyPanel is destroyed
        update_timer_.reset(lv_timer_create(update_cb, 1000, this));
    }

    void stop_updates() {
        update_timer_.reset();  // Explicitly stop timer
    }
};
```

**Alternative: Manual cleanup with lv_is_initialized() guard**

For panels/classes that manage timers manually:

```cpp
MyPanel::~MyPanel() {
    // Check LVGL is still running (avoids crash during static destruction)
    if (lv_is_initialized()) {
        if (my_timer_) {
            lv_timer_delete(my_timer_);
            my_timer_ = nullptr;
        }
    }
}
```

**Timer patterns:**

| Pattern | Safe? | Notes |
|---------|-------|-------|
| One-shot with `lv_timer_delete(t)` in callback | YES | Timer self-destructs |
| One-shot with `lv_timer_set_repeat_count(t, 1)` | YES | LVGL auto-deletes |
| LvglTimerGuard member | YES | RAII cleanup |
| Manual delete in destructor with `lv_is_initialized()` check | YES | Explicit cleanup |
| Timer stored in member, no cleanup | **NO** | Use-after-free risk |

**See also:** `include/ui_timer_guard.h` for full API documentation

### ⚠️ Shutdown Order: StaticPanelRegistry & StaticSubjectRegistry

**Problem:** The "Static Destruction Order Fiasco" - C++ doesn't guarantee destruction order of static/global objects across translation units. When `lv_deinit()` runs, it deletes widgets which try to remove their observers from subjects. If singleton subjects (PrinterState, AmsState, SettingsManager) haven't been deinitialized first, this causes crashes in `lv_observer_remove` due to linked list corruption.

**Solution:** Two self-registration pattern registries ensure proper cleanup order:

| Registry | Purpose | What it cleans up |
|----------|---------|-------------------|
| **StaticPanelRegistry** | UI panels/overlays with widgets | EmergencyStopOverlay, StatusBar, Keypad, Wizard subjects |
| **StaticSubjectRegistry** | Core state singletons with subjects | PrinterState, AmsState, SettingsManager, FilamentSensorManager |

**Shutdown Order (in Application::shutdown()):**

```
1. StaticPanelRegistry::destroy_all()     ← Panels destroy their own subjects
2. StaticSubjectRegistry::deinit_all()    ← Core singleton subjects deinitialized
3. lv_deinit()                            ← Now safe - all observers disconnected
```

**Self-Registration Pattern (MANDATORY):**

Each component's `init_subjects()` method MUST self-register its own cleanup. Registration is **never** done externally (e.g., in SubjectInitializer). This prevents forgotten registrations that cause shutdown crashes.

```cpp
// Inside the component's own init_subjects():
void PrinterState::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;

    // Self-register cleanup (co-located with init)
    StaticSubjectRegistry::instance().register_deinit(
        "PrinterState", []() { PrinterState::instance().deinit_subjects(); });
}
```

SubjectInitializer just calls `init_subjects()` — it does NOT register cleanup:
```cpp
// In SubjectInitializer::init_core_and_state():
PrinterState::instance().init_subjects();   // Self-registers internally
AmsState::instance().init_subjects();       // Self-registers internally
ToolState::instance().init_subjects();      // Self-registers internally
```

**deinit_subjects() Pattern:**

Each singleton with LVGL subjects must implement `deinit_subjects()`:

```cpp
void AmsState::deinit_subjects() {
    if (!initialized_) return;

    spdlog::debug("[AMS State] Deinitializing subjects");

    // Call lv_subject_deinit() on every subject
    lv_subject_deinit(&ams_type_);
    lv_subject_deinit(&ams_action_);
    // ... all other subjects ...

    initialized_ = false;
}
```

**Key Points:**
- **Reverse order:** Both registries deinitialize in reverse registration order (LIFO)
- **Idempotent:** Guard with `initialized_` flag to prevent double-cleanup
- **Panels before singletons:** Panels have observers on singleton subjects, so panels must be destroyed first
- **Debug logging:** Always log cleanup for debugging shutdown issues

**Reference Implementations:**
- `src/application/static_subject_registry.cpp` - Core singleton registry
- `src/application/static_panel_registry.cpp` - Panel/overlay registry
- `src/application/subject_initializer.cpp` - Registration during init
- `src/application/application.cpp:shutdown()` - Correct call order

## Thread Safety

### ⚠️ CRITICAL: LVGL Main Thread Requirement

**LVGL is NOT thread-safe.** All widget creation and modification MUST happen on the main thread.

### ⚠️ Subject Updates Are NOT Thread-Safe

**CRITICAL MISCONCEPTION:** You might assume `lv_subject_set_*()` is thread-safe because it's just updating a value. **THIS IS WRONG.**

```cpp
// ❌ DANGEROUS - looks safe but isn't!
void update_from_websocket_thread(int temp) {
    lv_subject_set_int(temp_subject, temp);  // Triggers observers!
    // If LVGL is rendering → assertion failure → infinite loop
}
```

**Why this fails:** Subject updates trigger bound observers. Those observers often:
1. Call `lv_label_set_text()` → triggers `lv_obj_invalidate()`
2. Call `lv_obj_add_flag()` → triggers `lv_obj_invalidate()`
3. Any widget modification during `lv_timer_handler()` rendering → **ASSERTION FAILURE**

The LVGL assertion `!disp->rendering_in_progress` will fire, and on embedded targets this causes an infinite `while(1)` loop.

**Real-world example:** WebSocket callback calls `FilamentSensorManager::discover_sensors()` which calls `lv_subject_set_int()`. If LVGL happens to be rendering, the app hangs and stops responding to pings.

### Safe Pattern: Defer to Main Thread

**Always use `helix::ui::queue_update()` for subject updates from background threads:**

```cpp
#include "ui_update_queue.h"

// ✅ CORRECT - defers to main thread via queue_update()
void update_from_websocket_thread(int temp) {
    helix::ui::queue_update([temp]() {
        lv_subject_set_int(&temp_subject, temp);  // Safe: runs on main thread
    });
}

// With captured data (unique_ptr overload for RAII):
auto data = std::make_unique<MyData>(value, text);
helix::ui::queue_update(std::move(data), [](MyData* d) {
    lv_subject_set_int(&my_subject, d->value);
    // d is automatically deleted after callback
});
```

**Why `helix::ui::queue_update()` not `lv_async_call()`?** LVGL's native `lv_async_call()` can fire *during* the render phase, causing assertion failures. Our `queue_update()` processes all queued lambdas at the START of each `lv_timer_handler()` cycle, *before* rendering begins.

**Reference Implementation:** See `printer_state.cpp` for the `set_*_internal()` pattern used by:
- `set_printer_capabilities()` → `set_printer_capabilities_internal()`
- `set_klipper_version()` → `set_klipper_version_internal()`
- `set_klippy_state()` → `set_klippy_state_internal()`

### Unsafe Operations (Main Thread Only)

**Widget manipulation requires main thread:**
```cpp
// Main thread only
void handle_ui_event(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKED);  // NOT safe from background threads
}
```

### Backend Integration Pattern: helix::ui::queue_update()

**Problem:** Backend threads (networking, file I/O, WiFi scanning) need to update UI but cannot call LVGL APIs directly.

**Solution:** Use `helix::ui::queue_update()` to marshal widget updates to the main thread:

```cpp
#include "ui_update_queue.h"

// Backend callback running in std::thread
void WiFiManager::handle_scan_complete(const std::string& data) {
    // Parse results (safe - no LVGL calls)
    auto networks = parse_networks(data);

    // Dispatch to LVGL main thread (safe - runs before render)
    helix::ui::queue_update([networks = std::move(networks), cb = scan_callback_]() {
        // NOW safe to create/modify widgets
        cb(networks);  // Calls populate_network_list()
    });
}
```

**Key Points:**
1. **Backend thread:** Parse data, capture into lambda
2. **`helix::ui::queue_update()`:** Queues lambda for main thread, runs BEFORE rendering
3. **Main thread lambda:** Creates/modifies widgets safely
4. **Memory management:** Lambda captures handle lifetime automatically

**Without this pattern:** Race conditions, segfaults, undefined behavior when backend thread creates widgets while LVGL is rendering.

**Reference Implementation:** `src/api/wifi_manager.cpp` (all event handlers use this pattern)

### When to Use helix::ui::queue_update()

✅ **ALWAYS use when on a background thread and:**
- Need to create/modify widgets (`lv_obj_*()` functions)
- Need to update subjects (`lv_subject_set_*()`) - **subjects trigger observers!**
- WebSocket callbacks (libhv event loop thread)
- Network/file I/O completion handlers
- Timer callbacks from non-LVGL timers

❌ **Don't need when:**
- Already on main thread (LVGL event handlers, `lv_timer_create()` callbacks)
- Pure computation with no LVGL calls at all
- Just logging or updating non-LVGL state

**Key insight:** If you're in a callback from libhv, std::thread, or any networking library, assume you're on a background thread and use `helix::ui::queue_update()`.

### Multi-Backend AmsState Coordination

`AmsState` uses a `std::recursive_mutex` to protect the `backends_` vector and all backend operations. When a backend emits an event on a background thread, the handler acquires the mutex, reads backend state, then posts subject updates via `helix::ui::queue_update()`. Each backend's event callback captures its index at registration time, so events are routed to the correct per-backend subject storage without ambiguity.

For secondary backends (index 1+), slot subjects live in `BackendSlotSubjects` structs rather than the flat `slot_colors_[]`/`slot_statuses_[]` arrays. The `sync_backend(int)` and `update_slot_for_backend(int, int)` methods handle this routing. All subject writes happen on the LVGL thread via `helix::ui::queue_update()`, so no additional synchronization is needed for the subject values themselves.

## Sensor Framework

HelixScreen's sensor framework provides a unified architecture for discovering,
configuring, and displaying sensor data from multiple sources.

### Discovery Architecture

Sensors come from three different sources:

| Source | Discovery Method | Example Sensors |
|--------|------------------|-----------------|
| Klipper objects | `printer.objects.list` | Humidity, Probe, Width, Switch |
| Klipper config | `configfile.config` | Accelerometers |
| Moonraker API | Moonraker endpoints | Color sensors (TD-1) |

### Manager Pattern

Each sensor category has a singleton manager implementing `ISensorManager`.
Managers implement only the discovery methods for their data source.

### Threading Model

⚠️ Moonraker callbacks run on libhv's thread, NOT the main LVGL thread.

- `discover*()`, `load_config()`, `set_sensor_*()` → Main thread only
- `update_from_status()` → Thread-safe (mutex + helix::ui::queue_update)
- `save_config()` → Thread-safe (read-only with mutex)

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

## Test Mode Architecture

Test mode (`--test` flag) enables hardware mocking for development without a real printer. Key architectural principle: **production builds NEVER fall back to mocks** - mocks require explicit opt-in.

**Factory Pattern:** Backend factories check `RuntimeConfig::should_mock_*()` before creating implementations:
```cpp
if (config.should_mock_wifi()) {
    return std::make_unique<WifiBackendMock>();  // Test mode
}
return std::make_unique<WifiBackendMacOS>();     // Production
```

**For usage and CLI flags:** See [DEVELOPMENT.md § Test Mode Development](DEVELOPMENT.md#test-mode-development).

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

**See [DEVELOPER_QUICK_REFERENCE.md](DEVELOPER_QUICK_REFERENCE.md#component-names-critical) for quick syntax reference.**

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

**For common implementation patterns and code snippets, see [DEVELOPER_QUICK_REFERENCE.md](DEVELOPER_QUICK_REFERENCE.md).**

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

## Extended Systems

These systems extend the core architecture for specific features:

### Modal System (ui_dialog)

All modal dialogs use the `ui_dialog` XML component for consistent theming and layout. The system standardizes dialog creation across the codebase:

- **`ui_dialog`** - Custom XML widget registered via `ui_dialog_register()`. Uses LVGL's built-in button grey for background, automatically adapting to light/dark mode. Used as the base container in all modal XML files.
- **`modal_button_row`** - Reusable XML component for consistent button layouts (OK/Cancel, Yes/No, etc.) at the bottom of modals.
- **Modal pattern** - Class-based modals inherit lifecycle management. Dynamically created modal buttons are wired imperatively since they are generated at runtime (see Legitimate Exception #11).

**XML usage:**
```xml
<ui_dialog>
  <text_heading text="Confirm Action"/>
  <text_body bind_text="dialog_message"/>
  <modal_button_row name="buttons"/>
</ui_dialog>
```

**Files:** `include/ui_dialog.h`, `ui_xml/modal_button_row.xml`, `ui_xml/*_modal.xml`, `ui_xml/*_dialog.xml`

### Spoolman Management

Full spool lifecycle management via Spoolman integration:

- **SpoolmanPanel** (`ui_panel_spoolman.cpp`) — Virtualized spool list with search, context menu, edit modal
- **SpoolWizardOverlay** (`ui_spool_wizard.cpp`) — 3-step creation wizard (Vendor → Filament → Spool Details)
- **SpoolmanContextMenu** — Right-click/long-press actions per spool row
- **SpoolEditModal** — Inline editing of spool properties

API calls route through `server.spoolman.proxy` JSON-RPC via Moonraker. The wizard supports dual-source vendor/filament data (server + SpoolmanDB external catalog) with deduplication. Creation is atomic with best-effort rollback.

Spoolman is now decoupled from AMS backends — spool assignments are tracked per-tool via `ToolState` with persistence to `config/tool_spools.json`. This allows Spoolman to work independently of any filament changer hardware.

### Config Migration System

Versioned schema migration for `helixconfig.json` that automatically upgrades configuration between releases. The `Config` class tracks a `config_version` integer (currently `CURRENT_CONFIG_VERSION = 2`) and applies migrations sequentially on load:

- **Version tracking** - Each config file stores its schema version; missing version implies v0
- **Sequential migrations** - Migrations run in order (v0->v1->v2) on startup, each transforming the JSON structure
- **Key consolidation** - Example: flat keys (`display_rotate`, `display_sleep_sec`, `touch_calibrated`) migrated into nested objects (`/display/rotate`, `/input/calibration/valid`)
- **Non-destructive** - Existing new-format values are preserved; only old-format keys are migrated and removed
- **Auto-save** - Config is saved after migration completes

**Files:** `include/config.h`, `src/system/config.cpp`
**Tests:** 20+ migration test cases in `tests/unit/test_config.cpp`

### Exclude Objects System

Allows users to exclude individual objects from the current print (Klipper's `exclude_object` module):

- **PrinterExcludedObjectsState** - Domain class managing excluded/defined object lists with a version subject that increments on changes. Uses `SubjectManager` for subject lifecycle and `std::unordered_set` for excluded object names (sets are not natively supported by LVGL subjects).
- **PrintExcludeObjectManager** - Coordinates the confirmation flow for excluding objects, preventing accidental exclusion.
- **ExcludeObjectsListOverlay** - Scrollable list overlay showing all defined objects with status indicators (current/idle/excluded), tap-to-exclude, and optional G-code object thumbnails via `GCodeObjectThumbnailRenderer`.
- **ExcludeObjectModal** - Confirmation modal before excluding an object.

**Files:** `include/printer_excluded_objects_state.h`, `include/ui_exclude_objects_list_overlay.h`, `include/ui_print_exclude_object_manager.h`, `ui_xml/exclude_objects_list_overlay.xml`, `ui_xml/exclude_object_modal.xml`

### Frequency Response Chart Widget

Custom chart widget for visualizing accelerometer frequency response data during input shaper calibration:

- **Platform-adaptive rendering** - Configures for EMBEDDED (50 points, simplified), BASIC (50 points), or STANDARD (200 points with animations) tiers
- **Multi-series support** - Separate data series for X/Y axis measurements with independent visibility toggle
- **Peak marking** - Vertical markers at detected resonance frequencies
- **Auto-downsampling** - Data exceeding the platform's max points is downsampled while preserving frequency range endpoints
- **Shaper overlay chips** - Interactive toggles to show/hide recommended shaper frequency bands

**Files:** `include/ui_frequency_response_chart.h`, `src/ui/ui_frequency_response_chart.cpp`

### Markdown Viewer Widget (ui_markdown)

Theme-aware markdown rendering widget registered as an XML custom component:

- **XML integration** - Used as `<ui_markdown bind_text="subject_name"/>` or `<ui_markdown text="# Static content"/>`
- **Theme-aware** - Reads colors from design tokens for headings, body, links, and code blocks
- **Subject binding** - Supports `bind_text` for reactive markdown content updates
- **RAII cleanup** - Automatic resource management via user data pattern
- **lv_markdown backend** - Wraps the `lv_markdown` library (git submodule)

**Files:** `include/ui_markdown.h`, `src/ui/ui_markdown.cpp`

### Update System

Async update checking and in-place installation system:

- **UpdateChecker** - Singleton that checks GitHub releases API and R2 CDN for newer versions. Rate-limited to 1 check per hour. Background thread for HTTP, results marshaled to LVGL thread via `helix::ui::queue_update()`.
- **Three update channels** - Stable (GitHub releases), Beta (pre-releases), Dev (custom URL)
- **Download pipeline** - Confirm -> Download -> Verify (SHA-256 + ELF architecture validation) -> Install (`install.sh`)
- **Safety guards** - Downloads blocked during active prints, explicit user confirmation required
- **Version dismissal** - Users can dismiss specific versions (persisted to config)
- **Auto-check** - 15-second initial delay, then 24-hour periodic checks
- **Notification modal** - Uses `ui_markdown` widget for release notes display
- **LVGL subjects** - Status, progress, version text all bound to UI via subjects

**Files:** `include/system/update_checker.h`, `src/system/update_checker.cpp`, `ui_xml/update_notify_modal.xml`, `ui_xml/update_download_modal.xml`

### Print Start Phase Detection

Modular system for detecting preparation phases (homing, heating, leveling) during PRINT_START:

- **PrintStartCollector** - Monitors G-code responses for phase detection with priority chain: HELIX:PHASE signals → profile signal formats → PRINT_START marker → completion marker → profile regex → built-in fallback
- **PrintStartProfile** - JSON-driven printer-specific profiles with signal format matching and regex patterns. Supports `sequential` (known firmware) and `weighted` (generic) progress modes
- **PreprintPredictor** - Weighted-average predictor using historical timing data to estimate remaining preparation time. Tracks last 3 entries (FIFO) with per-phase weighting and 15-minute anomaly rejection

**See [PRINT_START_PROFILES.md](PRINT_START_PROFILES.md) for the full developer guide.**

### Moonraker Plugin

Optional plugin for enhanced print phase tracking. See [moonraker-plugin/README.md](../moonraker-plugin/README.md) for installation and details.

- **helix_print.py** - Tracks print phases (heating, mesh, purge, etc.)
- **HelixPluginInstaller** - Auto-detects and installs plugin (local) or shows install command (remote)

### LED Control System

Unified LED management across five backends with automatic state-based lighting:

- **LedController** - Singleton orchestrating five backends: `NativeBackend` (Klipper neopixel/dotstar/led), `LedEffectBackend` (led_effect plugin animations), `WledBackend` (WLED network strips via Moonraker HTTP bridge), `MacroBackend` (user-configured macro devices), `OutputPinBackend` (output_pin brightness-only or on/off devices)
- **LedAutoState** - Observes printer state subjects (print status, klippy state, extruder target) and automatically applies LED actions for six states: idle, heating, printing, paused, error, complete
- **PrinterLedState** - Domain class tracking one LED strip for home panel display (RGBW subjects)
- **LedControlOverlay** - Full control overlay with color presets, effects, WLED presets, macro buttons
- **LedSettingsOverlay** - Configuration overlay for strip selection, auto-state mapping, macro device management

**Files:** `include/led/`, `src/led/`, `include/printer_led_state.h`, `include/ui_settings_led.h`, `ui_xml/led_*.xml`

**See [LED_CONTROL.md](LED_CONTROL.md) for the full developer guide.**

### Print History Management

Centralized caching shared between history panels:

- **PrintHistoryManager** - Single source of truth for job history, avoids duplicate API calls
- **FileHistoryStatus** - Enum for print status indicators (Completed, Cancelled, Error, etc.)
- **Observer pattern** - Panels register for change notifications, cleanup on destruction

### Timelapse State

`TimelapseState` is a singleton that handles timelapse event dispatch and state management. It subscribes to WebSocket `notify_timelapse_event` notifications, tracks frame capture counts and render progress via LVGL subjects, and emits throttled toast notifications during rendering.

**Files:** `include/timelapse_state.h`, `src/printer/timelapse_state.cpp`
**See [TIMELAPSE.md](TIMELAPSE.md) for the full developer guide.**

### Active Print Media

Handles async file operations during print selection:

- **ActivePrintMediaManager** - Manages file streaming state
- **StreamingPolicy** - Prevents conflicting async operations
- **BusyOverlay** - Visual feedback during long-running operations

### Watchdog (Embedded)

Crash recovery for embedded deployments:

- **helix_watchdog** - Separate process, minimal dependencies
- **Crash dialog** - Renders directly to framebuffer (no LVGL)
- **Auto-restart** - Monitors heartbeat, restarts on crash/hang

## Legitimate Exceptions to UI Patterns

During code audits, the following patterns may appear to violate declarative UI guidelines but are **acceptable exceptions**. Future audits should not flag these:

### 1. DELETE Event Handlers
`lv_obj_add_event_cb(obj, cb, LV_EVENT_DELETE, ...)` is required for RAII cleanup of widget user_data. Cannot be done in XML.

### 2. Canvas/Drawing Code
Files like `nozzle_renderer_*.cpp`, `ui_filament_path_canvas.cpp`, `ui_bed_mesh.cpp` use hardcoded colors for physical/material rendering (brass nozzle, charcoal frame). These are not theme colors.

### 3. Dynamic Widget Creation
Widgets created at runtime (step progress indicators, AMS mini status, keyboard overlays) cannot use XML bindings since they're generated programmatically.

### 4. Gesture State Machines
Keyboard long-press detection, jog pad touch handling require imperative state management for gesture recognition.

### 5. Bootstrap Components
Fatal error screen (`ui_fatal_error.cpp`) runs before theme is loaded - must use hardcoded colors.

### 6. Fallback Color Pattern
```cpp
const char* str = lv_xml_get_const("color_token");
lv_color_t c = str ? ui_theme_parse_hex_color(str) : lv_color_hex(0xFALLBACK);
```
This is correct - `str` contains an actual hex value from XML, not a token name.

### 7. fprintf in Destructors
spdlog may be destroyed during static destruction. Use `fprintf(stderr, ...)` in destructors of static/global objects.

### 8. Async Context new/delete
`helix::ui::queue_update()` with the `unique_ptr` overload handles memory automatically. The raw lambda overload captures by value or move, avoiding manual `new`/`delete`.

### 9. CLI printf
Code in `cli_args.cpp` runs before logging infrastructure is initialized - printf is acceptable.

### 10. Experimental Test Code
Test scaffolding in `experimental/src/*.cpp` uses printf for test output - not production code.

### 11. Modal Dialog Buttons
Dynamically created modals (confirmation dialogs, warnings) wire buttons imperatively since they're created at runtime.

### 12. snprintf with sizeof()
```cpp
char buf[256];
snprintf(buf, sizeof(buf), "format...", args);
```
This is safe - truncates rather than overflows. Not a security issue.

### 13. StreamingPolicy State Management
`StreamingPolicy` uses imperative state (`start()`/`complete()`) to track async operations. Cannot be declarative since it manages operation lifecycle, not UI state.

### 14. BusyOverlay Show/Hide
`BusyOverlay::show()`/`hide()` are imperative because they provide async feedback during operations. The overlay exists temporarily, not as persistent UI state.

---

## Related Documentation

This document focuses on system design, patterns, and architectural decisions ("why"). For implementation details:

- **[DEVELOPER_QUICK_REFERENCE.md](DEVELOPER_QUICK_REFERENCE.md)** - Code snippets, common patterns, quick lookups ("how")
- **[LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md)** - Complete XML syntax reference
- **[DEVELOPMENT.md](DEVELOPMENT.md)** - Build system and daily workflow
- **[DEVELOPMENT.md#contributing](DEVELOPMENT.md#contributing)** - Code standards and git workflow
- **[BUILD_SYSTEM.md](BUILD_SYSTEM.md)** - Build configuration and patches