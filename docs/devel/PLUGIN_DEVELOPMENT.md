# HelixScreen Plugin Development Guide

**Last Updated:** July 2026
**API Version:** 1.0
**Minimum HelixScreen Version:** 1.0

---

## Table of Contents

1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Plugin Structure](#plugin-structure)
4. [Plugin Lifecycle](#plugin-lifecycle)
5. [PluginAPI Reference](#pluginapi-reference)
6. [UI Development](#ui-development)
7. [Threading Model](#threading-model)
8. [Common Patterns](#common-patterns)
9. [Building Plugins](#building-plugins)
10. [Debugging](#debugging)
11. [Best Practices and Gotchas](#best-practices-and-gotchas)
12. [Complete Examples](#complete-examples)

---

## Overview

The HelixScreen plugin system allows third-party developers to extend the 3D printer touchscreen interface with custom functionality. Plugins are dynamically loaded shared libraries that can:

- **Monitor printer state** - Subscribe to temperature, print status, and other real-time data
- **Respond to events** - React to print start/pause/complete, filament changes, etc.
- **Inject UI widgets** - Add custom widgets to designated areas of existing panels
- **Register services** - Expose functionality for other plugins to use
- **Subscribe to Moonraker** - Receive raw Klipper object updates

### Prerequisites

- C++17 or later compiler
- LVGL 9.5 knowledge (for UI development)
- Familiarity with HelixScreen's declarative XML UI system
- Understanding of the Klipper/Moonraker architecture (for printer integration)

---

## Quick Start

This section gets you from zero to a working plugin as quickly as possible.

### Minimal "Hello World" Plugin

**Directory structure:**
```
plugins/
  hello-world/
    manifest.json
    hello_world.cpp
    Makefile
```

**manifest.json:**
```json
{
  "id": "hello-world",
  "name": "Hello World",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "A minimal example plugin",
  "helix_version": ">=1.0"
}
```

**hello_world.cpp:**
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_api.h"

using namespace helix::plugin;

// Store API pointer for use in callbacks
static PluginAPI* g_api = nullptr;

// Required: Plugin initialization
extern "C" bool helix_plugin_init(PluginAPI* api, const char* plugin_dir) {
    g_api = api;

    api->log_info("Hello World plugin initialized!");
    api->log_info("Plugin directory: " + std::string(plugin_dir));

    // Subscribe to printer connection events
    api->on_event(events::PRINTER_CONNECTED, [](const EventData& event) {
        g_api->log_info("Printer connected!");
    });

    return true;  // Return false to abort loading
}

// Required: Plugin cleanup
extern "C" void helix_plugin_deinit() {
    if (g_api) {
        g_api->log_info("Hello World plugin shutting down");
    }
    g_api = nullptr;
}

// Optional: API version for compatibility checking
extern "C" const char* helix_plugin_api_version() {
    return "1.0";
}
```

**Makefile:**
```makefile
CXX = g++
CXXFLAGS = -std=c++17 -fPIC -O2 -Wall -Wextra
LDFLAGS = -shared

# Adjust this path to your HelixScreen installation
HELIX_INCLUDE = /path/to/helixscreen/include

PLUGIN_NAME = hello-world
SOURCES = hello_world.cpp
OUTPUT = libhelix_$(PLUGIN_NAME).so

ifeq ($(shell uname),Darwin)
    OUTPUT = libhelix_$(PLUGIN_NAME).dylib
endif

all: $(OUTPUT)

$(OUTPUT): $(SOURCES)
	$(CXX) $(CXXFLAGS) -I$(HELIX_INCLUDE) $(LDFLAGS) -o $@ $^

clean:
	rm -f $(OUTPUT)

.PHONY: all clean
```

### Build and Test

```bash
cd plugins/hello-world
make

# Run HelixScreen. Plugins are discovered from ./plugins/ in the working
# directory at startup — there is no --plugins-dir flag. Each plugin must
# also be enabled in settings.json before it loads.
./helix-screen --test -vv
```

You should see in the logs:
```
[plugin:hello-world] Hello World plugin initialized!
[plugin:hello-world] Plugin directory: ./plugins/hello-world
```

---

## Plugin Structure

### Directory Layout

Each plugin lives in its own directory under the plugins folder:

```
plugins/
  my-plugin/
    manifest.json          # Required: Plugin metadata
    libhelix_my-plugin.so  # Required: Compiled plugin library
    my_widget.xml          # Optional: XML UI components
    assets/                # Optional: Images, fonts, etc.
      icon.png
    README.md              # Optional: Documentation
```

### manifest.json Schema

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Unique identifier (alphanumeric, hyphens, underscores) |
| `name` | string | Yes | Human-readable display name |
| `version` | string | Yes | Semantic version (e.g., "1.2.3") |
| `helix_version` | string | No | Required HelixScreen version (e.g., ">=1.0") |
| `author` | string | No | Plugin author name |
| `description` | string | No | Brief description |
| `entry_point` | string | No | Init function name (default: "helix_plugin_init") |
| `dependencies` | array | No | Other plugin IDs this plugin requires |
| `ui` | object | No | UI configuration (see below) |

**UI Configuration:**
```json
{
  "ui": {
    "settings_page": true,
    "navbar_panel": false,
    "injection_points": ["print_status_extras"]
  }
}
```

### Library Naming Conventions

The plugin manager searches for libraries in this order:

**Linux:**
1. `libhelix_<plugin_id>.so`
2. `lib<plugin_id>.so`
3. `<plugin_id>.so`
4. Any `.so` file in the directory

**macOS:**
1. `libhelix_<plugin_id>.dylib`
2. `lib<plugin_id>.dylib`
3. `<plugin_id>.dylib`
4. Any `.dylib` file in the directory

---

## Plugin Lifecycle

### Plugin States

Plugins exist in one of four states:

| State | Description |
|-------|-------------|
| **Discovered** | Plugin found in plugins directory with valid manifest |
| **Disabled** | Discovered but not in config's enabled list - silently skipped |
| **Enabled** | In config's enabled list, will attempt to load |
| **Loaded** | Successfully initialized and running |
| **Failed** | Enabled but failed to load (version mismatch, missing library, init error) |

### Enabling and Disabling Plugins

Plugins must be **explicitly enabled** in the configuration file. Simply placing a plugin in the `plugins/` directory does not activate it.

**Config file location:** `~/helixscreen/config/settings.json` (or platform equivalent)

**Enable a plugin:**
```json
{
  "plugins": {
    "enabled": ["hello-world", "led-effects", "spoolman"]
  }
}
```

**Disable a plugin:**
Remove it from the `enabled` array, or use the UI:
- If a plugin fails to load, a toast notification appears with a **[Disable]** button
- Go to **Settings > Plugins** to see all plugins and disable failed ones

**Why explicit enable?**
- Security: prevents accidentally running unknown code
- Performance: only loads what you need
- Control: easily test plugins by toggling in config

### Lifecycle Stages

```
┌─────────────┐
│  Discovery  │  Plugin manager scans plugins directory for manifest.json files
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Enable Check│  Is plugin ID in config's /plugins/enabled list?
└──────┬──────┘
       │ Yes                          │ No
       ▼                              ▼
┌─────────────┐                ┌─────────────┐
│    Load     │                │  (Skipped)  │  Plugin stays discovered but inactive
└──────┬──────┘                └─────────────┘
       │
       ▼
┌─────────────┐
│    Init     │  helix_plugin_init() called with PluginAPI
└──────┬──────┘
       │ Success                      │ Failure
       ▼                              ▼
┌─────────────┐                ┌─────────────┐
│   Running   │                │   Failed    │  Error logged, toast shown with [Disable]
└──────┬──────┘                └─────────────┘
       │
       ▼
┌─────────────┐
│   Deinit    │  helix_plugin_deinit() called for cleanup
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Unload    │  dlclose() unloads the library
└─────────────┘
```

### Handling Load Failures

When an enabled plugin fails to load:

1. **Toast notification** appears:
   - Single failure: `"plugin-id" failed to load [Disable]`
   - Multiple failures: `N plugins failed to load [Manage]`

2. **[Disable] button** removes the plugin from config's enabled list (no restart needed)

3. **Settings > Plugins** shows all plugins organized by state:
   - ✅ **Loaded** - Running plugins
   - ⚠️ **Disabled** - Discovered but not enabled
   - ❌ **Failed** - Enabled but couldn't load (with error details and disable button)

### Entry Point Contract

Every plugin must export these C functions:

```cpp
// Required: Called during plugin loading
// Return true to complete loading, false to abort
extern "C" bool helix_plugin_init(PluginAPI* api, const char* plugin_dir);

// Required: Called during plugin unloading
// Clean up any resources allocated during init/running
extern "C" void helix_plugin_deinit();

// Optional: Return API version for compatibility checking
// If provided, must match host's PLUGIN_API_VERSION ("1.0")
extern "C" const char* helix_plugin_api_version();
```

### Initialization Order

Plugins are loaded in dependency order using topological sort:

1. If plugin A depends on plugin B, B is loaded first
2. Circular dependencies cause both plugins to fail loading
3. Missing dependencies cause the dependent plugin to fail

### Cleanup Guarantees

When `helix_plugin_deinit()` is called:

1. All UI widgets injected by your plugin have already been removed
2. Event subscriptions are automatically unsubscribed
3. Registered services are automatically unregistered
4. Moonraker subscriptions are automatically cleaned up

You should still clean up:
- Any allocated memory you own
- File handles, network connections
- Background threads you started

---

## PluginAPI Reference

The `PluginAPI` class is your plugin's interface to HelixScreen. A pointer is passed to `helix_plugin_init()` and remains valid until `helix_plugin_deinit()` is called.

### Core Services

#### moonraker_api()

```cpp
MoonrakerAPI* moonraker_api() const;
```

Returns the high-level Moonraker API for common printer operations.

**Nullability:** May be `nullptr` if Moonraker is not connected. Always check before use.

```cpp
if (auto* api = g_api->moonraker_api()) {
    // execute_gcode(gcode, on_success, on_error, timeout_ms=0, silent=false, source)
    api->execute_gcode(
        "G28",                                       // Home all axes
        []() { /* on success */ },
        [](const MoonrakerError& e) { /* on error */ });
}
```

#### moonraker_client()

```cpp
helix::MoonrakerClient* moonraker_client() const;
```

Returns the low-level WebSocket client for raw Moonraker communication.

**Nullability:** May be `nullptr` if not connected.

#### printer_state()

```cpp
PrinterState& printer_state() const;
```

Returns the reactive printer state object. Use this to read current values or access LVGL subjects for binding.

**Nullability:** Always valid (never null).

```cpp
PrinterState& state = g_api->printer_state();
// Temperatures are exposed as LVGL subjects, not plain getters. The no-lifetime
// overload is for read-only reads (temps are stored in tenths of a degree).
lv_subject_t* temp = state.get_extruder_temp_subject("extruder");
int nozzle_deci = lv_subject_get_int(temp);   // e.g. 2105 == 210.5 °C
```

#### config()

```cpp
Config* config() const;
```

Returns the configuration manager for reading/writing settings.

**Nullability:** May be `nullptr` during early initialization.

#### plugin_id()

```cpp
const std::string& plugin_id() const;
```

Returns this plugin's ID (as declared in manifest.json).

---

### Event System

#### on_event()

```cpp
EventSubscriptionId on_event(const std::string& event_name, EventCallback callback);
```

Subscribe to an application event. Callbacks are invoked on the main thread.

**Parameters:**
- `event_name`: One of the `events::*` constants
- `callback`: Function to call when event occurs

**Returns:** Subscription ID for later unsubscription.

```cpp
auto sub_id = api->on_event(events::PRINT_STARTED, [](const EventData& event) {
    std::string filename = event.payload.value("filename", "unknown");
    g_api->log_info("Print started: " + filename);
});
```

#### off_event()

```cpp
bool off_event(EventSubscriptionId id);
```

Unsubscribe from an event. Returns `true` if subscription was found and removed.

**Note:** Subscriptions are automatically cleaned up when your plugin unloads.

### Event Reference Table

| Event Name | Payload Fields | Description |
|------------|----------------|-------------|
| `events::PRINTER_CONNECTED` | (none) | Moonraker WebSocket connected |
| `events::PRINTER_DISCONNECTED` | (none) | Moonraker WebSocket disconnected |
| `events::PRINT_STARTED` | `filename` | Print job started |
| `events::PRINT_PAUSED` | (none) | Print job paused |
| `events::PRINT_RESUMED` | (none) | Print job resumed |
| `events::PRINT_COMPLETED` | (none) | Print job completed successfully |
| `events::PRINT_CANCELLED` | (none) | Print job cancelled by user |
| `events::PRINT_ERROR` | `error` | Print job failed |
| `events::TEMPERATURE_UPDATED` | `heater`, `current`, `target` | Heater temperature changed |
| `events::FILAMENT_LOADED` | `slot`, `material`, `color` | Filament loaded into extruder |
| `events::FILAMENT_UNLOADED` | `slot` | Filament unloaded |
| `events::KLIPPER_STATE_CHANGED` | `state` | Klipper state (ready/shutdown/error/startup) |
| `events::THEME_CHANGED` | `theme` | Light/dark theme changed |
| `events::NAVIGATION_CHANGED` | `panel` | User navigated to different panel |

---

### Moonraker Subscriptions

#### subscribe_moonraker()

```cpp
MoonrakerSubscriptionId subscribe_moonraker(
    const std::vector<std::string>& objects,
    MoonrakerCallback callback
);
```

Subscribe to Klipper object updates. Unlike direct MoonrakerClient subscriptions, this method:

- Queues subscriptions if Moonraker isn't connected yet
- Automatically applies them when connection is established
- Cleans up automatically when plugin unloads

**Parameters:**
- `objects`: Klipper objects to subscribe to (e.g., `{"extruder", "heater_bed"}`)
- `callback`: Function receiving JSON updates

**Returns:** Subscription ID (use with `unsubscribe_moonraker()`).

```cpp
api->subscribe_moonraker({"extruder", "heater_bed"}, [](const json& update) {
    if (update.contains("extruder")) {
        float temp = update["extruder"].value("temperature", 0.0f);
        g_api->log_debug("Extruder temp: " + std::to_string(temp));
    }
});
```

> **WARNING:** The callback runs on a background thread. See [Threading Model](#threading-model) before updating UI.

#### unsubscribe_moonraker()

```cpp
bool unsubscribe_moonraker(MoonrakerSubscriptionId id);
```

Remove a Moonraker subscription. Returns `true` if found and removed.

---

### Subject Registration

#### register_subject()

```cpp
void register_subject(const std::string& name, lv_subject_t* subject);
```

Register an LVGL subject for reactive UI binding. Registered subjects can be referenced in XML layouts using `bind_text`, `bind_flag_if_eq`, etc.

**Parameters:**
- `name`: Subject identifier (convention: `"plugin_id.subject_name"`)
- `subject`: Pointer to your `lv_subject_t` (you own the memory)

```cpp
static lv_subject_t s_status_subject;

bool helix_plugin_init(PluginAPI* api, const char*) {
    lv_subject_init_pointer(&s_status_subject, "Idle");
    api->register_subject("my-plugin.status", &s_status_subject);
    return true;
}
```

Then in XML:
```xml
<text_body bind_text="my-plugin.status"/>
```

#### unregister_subject()

```cpp
bool unregister_subject(const std::string& name);
```

Unregister a previously registered subject.

---

### Service Registration

The service registry enables plugin-to-plugin communication.

#### register_service()

```cpp
void register_service(const std::string& name, void* service);
```

Register a service for other plugins to use.

**Parameters:**
- `name`: Service identifier (convention: `"plugin_id.service_name"`)
- `service`: Pointer to your service instance (you own the memory)

```cpp
class LedController {
public:
    void set_color(uint32_t rgb);
    void set_brightness(float percent);
};

static LedController s_controller;

bool helix_plugin_init(PluginAPI* api, const char*) {
    api->register_service("led-effects.controller", &s_controller);
    return true;
}
```

#### unregister_service()

```cpp
bool unregister_service(const std::string& name);
```

Remove a registered service.

#### get_service()

```cpp
void* get_service(const std::string& name) const;

// Template version for convenience
template<typename T>
T* get_service(const std::string& name) const;
```

Retrieve a service registered by another plugin.

```cpp
auto* led = api->get_service<LedController>("led-effects.controller");
if (led) {
    led->set_color(0xFF0000);  // Red
}
```

---

### UI Injection

#### inject_widget()

```cpp
bool inject_widget(
    const std::string& point_id,
    const std::string& xml_component,
    const WidgetCallbacks& callbacks = {}
);
```

Inject an XML component into a designated injection point.

**Parameters:**
- `point_id`: Injection point identifier (see table below)
- `xml_component`: Name of registered XML component
- `callbacks`: Optional lifecycle callbacks

**Returns:** `true` if injection succeeded.

```cpp
api->inject_widget("print_status_extras", "my_status_widget", {
    .on_create = [](lv_obj_t* widget) {
        g_api->log_debug("Widget created!");
    },
    .on_destroy = [](lv_obj_t* widget) {
        g_api->log_debug("Widget being destroyed");
    }
});
```

#### register_xml_component()

```cpp
bool register_xml_component(const std::string& plugin_dir, const std::string& filename);
```

Register an XML component from your plugin directory.

**Parameters:**
- `plugin_dir`: The directory path passed to `helix_plugin_init()`
- `filename`: XML file name (e.g., `"my_widget.xml"`)

```cpp
bool helix_plugin_init(PluginAPI* api, const char* plugin_dir) {
    if (!api->register_xml_component(plugin_dir, "status_widget.xml")) {
        api->log_error("Failed to register status_widget.xml");
        return false;
    }

    api->inject_widget("print_status_extras", "status_widget");
    return true;
}
```

#### has_injection_point()

```cpp
bool has_injection_point(const std::string& point_id) const;
```

Check if an injection point is currently available.

**Note:** Injection points are registered by panels when they load. If a panel hasn't been shown yet, its injection points won't be available.

---

### Logging

All logging methods are thread-safe and automatically prefix messages with your plugin ID.

```cpp
void log_info(const std::string& message) const;   // Visible with -v
void log_warn(const std::string& message) const;   // Always visible
void log_error(const std::string& message) const;  // Always visible
void log_debug(const std::string& message) const;  // Visible with -vv
```

**Example output:**
```
[plugin:my-plugin] Initializing...
```

---

## UI Development

### XML Component Registration

Plugins can define UI components using HelixScreen's declarative XML system.

**my_widget.xml:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<component>
  <view extends="lv_obj" name="my_status_widget"
        style_width="content" style_height="content"
        style_pad_all="#space_sm" style_radius="8"
        style_bg_color="#card_bg" style_bg_opa="cover">

    <lv_obj style_flex_flow="row" style_flex_gap_row="#space_xs">
      <icon name="icon" codepoint="0xF0425" style_text_color="#success_color"/>
      <text_body name="status" bind_text="my-plugin.status"/>
    </lv_obj>

  </view>
</component>
```

Register and inject in your plugin:

```cpp
bool helix_plugin_init(PluginAPI* api, const char* plugin_dir) {
    // Register the XML component
    api->register_xml_component(plugin_dir, "my_widget.xml");

    // Inject into home panel
    api->inject_widget("print_status_extras", "my_status_widget");

    return true;
}
```

### Injection Points

| Point ID | Location | Description |
|----------|----------|-------------|
| `print_status_extras` | Print Status panel | Extra widgets area in the print status overlay |

**Note:** `print_status_extras` is currently the **only** registered injection point
(registered in `src/ui/ui_panel_print_status.cpp`). Other points (a home-panel widget area,
a settings section) are not yet registered — `has_injection_point()` returns `false` for
them, so `inject_widget()` is a no-op. Always call `has_injection_point()` first, and expect
more points to be added in future versions.

### Widget Lifecycle Callbacks

```cpp
struct WidgetCallbacks {
    std::function<void(lv_obj_t*)> on_create;   // After widget added to container
    std::function<void(lv_obj_t*)> on_destroy;  // Before widget is deleted
};
```

Use these to:
- Bind subjects to widget elements in `on_create`
- Clean up observers or custom data in `on_destroy`

### Design Tokens

Always use design tokens from `globals.xml` for consistent theming:

| Category | Wrong | Correct |
|----------|-------|---------|
| Colors | `style_bg_color="#E0E0E0"` | `style_bg_color="#card_bg"` |
| Spacing | `style_pad_all="12"` | `style_pad_all="#space_md"` |
| Typography | `<lv_label>` | `<text_heading>`, `<text_body>`, `<text_small>` |

See [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md) for complete XML reference.

---

## Threading Model

> **CRITICAL:** Understanding the threading model is essential to avoid crashes and race conditions.

### Thread Overview

| Thread | Purpose | LVGL Safe? |
|--------|---------|------------|
| **Main thread** | LVGL rendering, event callbacks | Yes |
| **Background thread** | Moonraker WebSocket callbacks | **NO** |

### The Golden Rules

1. **Event callbacks run on the main thread** - Safe to update UI
2. **Moonraker callbacks run on a background thread** - NOT safe for LVGL
3. **All `lv_*()` functions must be called from the main thread only**

### The helix::ui::queue_update() Pattern

When you receive data on a background thread (e.g., Moonraker callback), use `helix::ui::queue_update()` to defer LVGL updates to the main thread:

```cpp
#include "ui_update_queue.h"

api->subscribe_moonraker({"extruder"}, [](const json& update) {
    // This runs on BACKGROUND thread!

    if (update.contains("extruder")) {
        float temp = update["extruder"].value("temperature", 0.0f);

        // Defer LVGL update to main thread
        helix::ui::queue_update([temp]() {
            // This runs on MAIN thread - LVGL safe
            lv_subject_set_int(&s_temp_subject, static_cast<int>(temp));
        });
    }
});
```

> **WARNING:** Never call `lv_subject_set_*()`, `lv_label_set_text()`, or any `lv_*()` function directly from a Moonraker callback. This will cause crashes or undefined behavior.

### What NOT to Do

```cpp
// BAD - Moonraker callback runs on background thread!
api->subscribe_moonraker({"extruder"}, [](const json& update) {
    // CRASH: Calling LVGL from background thread
    lv_label_set_text(my_label, "Updated!");  // DON'T DO THIS
    lv_subject_set_int(&subject, 42);          // DON'T DO THIS
});

// GOOD - Defer to main thread
api->subscribe_moonraker({"extruder"}, [](const json& update) {
    helix::ui::queue_update([]() {
        lv_subject_set_int(&subject, 42);  // Safe on main thread
    });
});
```

---

## Common Patterns

### Monitor Printer State

```cpp
static PluginAPI* g_api = nullptr;
static EventSubscriptionId s_temp_sub = 0;

bool helix_plugin_init(PluginAPI* api, const char*) {
    g_api = api;

    // Check current temperature immediately (subjects store tenths of a degree)
    PrinterState& state = api->printer_state();
    int current_deci = lv_subject_get_int(state.get_extruder_temp_subject("extruder"));
    api->log_info("Current nozzle temp: " + std::to_string(current_deci / 10.0));

    // Subscribe to temperature changes
    s_temp_sub = api->on_event(events::TEMPERATURE_UPDATED, [](const EventData& e) {
        std::string heater = e.payload.value("heater", "");
        float temp = e.payload.value("current", 0.0f);
        float target = e.payload.value("target", 0.0f);

        if (heater == "extruder" && temp > 200.0f) {
            g_api->log_warn("Nozzle is hot!");
        }
    });

    return true;
}

void helix_plugin_deinit() {
    // Subscriptions are auto-cleaned, but explicit cleanup is good practice
    if (g_api && s_temp_sub != 0) {
        g_api->off_event(s_temp_sub);
    }
}
```

### Respond to Print Events

```cpp
bool helix_plugin_init(PluginAPI* api, const char*) {
    api->on_event(events::PRINT_STARTED, [](const EventData& e) {
        std::string file = e.payload.value("filename", "unknown");
        g_api->log_info("Starting print: " + file);

        // Maybe trigger LED effect, play sound, etc.
    });

    api->on_event(events::PRINT_COMPLETED, [](const EventData&) {
        g_api->log_info("Print complete!");

        // Maybe trigger celebration LED pattern
        if (auto* led = g_api->get_service<LedController>("led-effects.controller")) {
            led->play_effect("celebration");
        }
    });

    api->on_event(events::PRINT_ERROR, [](const EventData& e) {
        std::string error = e.payload.value("error", "Unknown error");
        g_api->log_error("Print failed: " + error);
    });

    return true;
}
```

### Inter-Plugin Communication

**Provider plugin (led-effects):**
```cpp
class LedEffectsService {
public:
    void set_mode(const std::string& mode) { current_mode_ = mode; }
    void set_color(uint32_t rgb) { color_ = rgb; }
    std::string get_mode() const { return current_mode_; }

private:
    std::string current_mode_ = "off";
    uint32_t color_ = 0xFFFFFF;
};

static LedEffectsService s_service;

bool helix_plugin_init(PluginAPI* api, const char*) {
    api->register_service("led-effects.service", &s_service);
    return true;
}
```

**Consumer plugin:**
```cpp
bool helix_plugin_init(PluginAPI* api, const char*) {
    // Check if LED effects plugin is available
    auto* led = api->get_service<LedEffectsService>("led-effects.service");
    if (led) {
        api->log_info("LED Effects available, current mode: " + led->get_mode());
        led->set_mode("rainbow");
    } else {
        api->log_info("LED Effects plugin not installed");
    }

    return true;
}
```

---

## Building Plugins

### Compiler Flags

Required flags for plugin compilation:

```makefile
CXXFLAGS = -std=c++17 -fPIC -O2 -Wall -Wextra
LDFLAGS = -shared
```

**Important:**
- `-fPIC`: Required for position-independent code in shared libraries
- `-std=c++17`: Minimum C++ standard version
- `-shared`: Create a shared library

### Complete Makefile Template

```makefile
# Plugin Makefile Template
CXX ?= g++
CXXFLAGS = -std=c++17 -fPIC -O2 -Wall -Wextra
LDFLAGS = -shared

# HelixScreen include path
HELIX_ROOT ?= /path/to/helixscreen
HELIX_INCLUDE = $(HELIX_ROOT)/include

# Plugin settings
PLUGIN_NAME = my-plugin
SOURCES = $(wildcard *.cpp)
OBJECTS = $(SOURCES:.cpp=.o)

# Output library name (platform-specific)
ifeq ($(shell uname),Darwin)
    OUTPUT = libhelix_$(PLUGIN_NAME).dylib
    LDFLAGS += -dynamiclib
else
    OUTPUT = libhelix_$(PLUGIN_NAME).so
endif

# Include paths
INCLUDES = -I$(HELIX_INCLUDE) -I$(HELIX_ROOT)/lib/lvgl \
           -I$(HELIX_ROOT)/lib/spdlog/include \
           -I$(HELIX_ROOT)/lib/libhv/include   # JSON via #include "hv/json.hpp"

# Build targets
all: $(OUTPUT)

$(OUTPUT): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OUTPUT) $(OBJECTS)

install: $(OUTPUT)
	mkdir -p $(DESTDIR)/plugins/$(PLUGIN_NAME)
	cp $(OUTPUT) manifest.json $(DESTDIR)/plugins/$(PLUGIN_NAME)/
	@if [ -d xml ]; then cp -r xml/* $(DESTDIR)/plugins/$(PLUGIN_NAME)/; fi

.PHONY: all clean install
```

### Cross-Compilation for Raspberry Pi

```makefile
# Pi cross-compilation
pi:
	$(MAKE) CXX=aarch64-linux-gnu-g++ \
	        CXXFLAGS="$(CXXFLAGS) -march=armv8-a" \
	        OUTPUT=libhelix_$(PLUGIN_NAME).so
```

### Cross-Compilation for Adventurer 5M

```makefile
# AD5M cross-compilation (MIPS architecture)
ad5m:
	$(MAKE) CXX=mipsel-linux-gnu-g++ \
	        CXXFLAGS="$(CXXFLAGS) -march=mips32r2 -mfp32" \
	        OUTPUT=libhelix_$(PLUGIN_NAME).so
```

---

## Debugging

### Verbosity Flags

**Always use verbosity flags when testing plugins!**

| Flag | Level | Usage |
|------|-------|-------|
| (none) | WARN | Never use for debugging |
| `-v` | INFO | Basic logging |
| `-vv` | DEBUG | Detailed logging (recommended) |
| `-vvv` | TRACE | Very verbose |

```bash
./helix-screen --test -vv
```

### Common Error Messages

| Error Type | Message | Solution |
|------------|---------|----------|
| `MANIFEST_PARSE_ERROR` | "JSON parse error: ..." | Fix JSON syntax in manifest.json |
| `MANIFEST_MISSING_FIELD` | "Missing required field: id" | Add required fields to manifest |
| `LIBRARY_NOT_FOUND` | "No .so/.dylib file found" | Build your plugin library |
| `LOAD_FAILED` | "dlopen error: ..." | Check library dependencies, linking |
| `SYMBOL_NOT_FOUND` | "helix_plugin_init not found" | Add `extern "C"` to entry points |
| `INIT_FAILED` | "Plugin init returned false" | Debug your init function |
| `VERSION_MISMATCH` | "API version mismatch" | Update plugin to current API version |
| `MISSING_DEPENDENCY` | "Dependency not found: ..." | Install required plugin or remove dep |
| `DEPENDENCY_CYCLE` | "Plugin involved in dependency cycle" | Fix circular dependencies |

### Testing Without a Printer

Always use `--test` flag when no real printer is connected:

```bash
./helix-screen --test -vv
```

This enables mock data for printer state, temperatures, etc.

### Debugging Tips

1. **Add timestamps to logs** for timing issues:
   ```cpp
   api->log_debug("[" + std::to_string(time(nullptr)) + "] Event received");
   ```

2. **Check if services exist** before using:
   ```cpp
   if (!api->has_injection_point("print_status_extras")) {
       api->log_warn("Injection point not available yet");
   }
   ```

3. **Validate manifest.json** with a JSON linter before testing

4. **Use on_destroy callbacks** to verify cleanup:
   ```cpp
   .on_destroy = [](lv_obj_t*) {
       g_api->log_debug("Widget cleanup - verify no leaks");
   }
   ```

---

## Best Practices and Gotchas

### Do

- Store the `PluginAPI*` pointer globally for use in callbacks
- Use `extern "C"` for all exported functions
- Check for `nullptr` before using `moonraker_api()`, `moonraker_client()`, `config()`
- Use `helix::ui::queue_update()` for LVGL updates from background threads
- Prefix subjects and services with your plugin ID
- Use design tokens in XML for consistent theming
- Handle exceptions in callbacks (uncaught exceptions may crash HelixScreen)
- Clean up resources in `helix_plugin_deinit()`

### Don't

- Call `lv_*()` functions from Moonraker callbacks (use `helix::ui::queue_update()`)
- Assume injection points are always available
- Block in event callbacks (keep them fast)
- Use raw pointers to PluginAPI after `helix_plugin_deinit()`
- Forget `extern "C"` on entry points (causes `SYMBOL_NOT_FOUND`)
- Hardcode colors - use design tokens for theme compatibility
- Create circular dependencies between plugins
- Assume Moonraker is always connected

### Common Mistakes

**Mistake: Forgetting extern "C"**
```cpp
// WRONG - C++ name mangling breaks symbol lookup
bool helix_plugin_init(PluginAPI* api, const char* dir);

// CORRECT
extern "C" bool helix_plugin_init(PluginAPI* api, const char* dir);
```

**Mistake: LVGL from background thread**
```cpp
// WRONG - Moonraker callback is background thread
api->subscribe_moonraker({"extruder"}, [](const json& j) {
    lv_label_set_text(label, "crash incoming");
});

// CORRECT
api->subscribe_moonraker({"extruder"}, [](const json& j) {
    helix::ui::queue_update([]() {
        lv_subject_set_string(&subject, "safe update");
    });
});
```

**Mistake: Not checking for nullptr**
```cpp
// WRONG - may crash if Moonraker not connected
api->moonraker_api()->execute_gcode("G28", []() {}, [](const MoonrakerError&) {});

// CORRECT
if (auto* mrapi = api->moonraker_api()) {
    mrapi->execute_gcode("G28", []() {}, [](const MoonrakerError&) {});
}
```

---

## Complete Examples

### Hello World (Minimal)

See [Quick Start](#quick-start) section for the complete minimal example.

### Temperature Widget (UI Injection)

This example creates a custom temperature display widget and injects it into the home panel.

**plugins/temp-widget/manifest.json:**
```json
{
  "id": "temp-widget",
  "name": "Temperature Widget",
  "version": "1.0.0",
  "author": "HelixScreen",
  "description": "Displays nozzle temperature with custom styling",
  "ui": {
    "injection_points": ["print_status_extras"]
  }
}
```

**plugins/temp-widget/temp_display.xml:**
```xml
<?xml version="1.0" encoding="UTF-8"?>
<component>
  <view extends="lv_obj" name="temp_display"
        style_width="160" style_height="80"
        style_pad_all="#space_md" style_radius="12"
        style_bg_color="#card_bg" style_bg_opa="cover"
        style_flex_flow="column" style_flex_main_place="center"
        style_flex_cross_place="center">

    <text_small style_text_color="#text_muted">Nozzle</text_small>

    <lv_obj style_flex_flow="row" style_flex_cross_place="center"
            style_flex_gap_column="#space_xs">
      <icon codepoint="0xF18B6" style_text_color="#warning_color"/>
      <text_heading name="temp_value" bind_text="temp-widget.nozzle_temp"/>
      <text_body style_text_color="#text_muted">C</text_body>
    </lv_obj>

  </view>
</component>
```

**plugins/temp-widget/temp_widget.cpp:**
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_api.h"
#include "ui_update_queue.h"
#include "lvgl.h"

#include <sstream>
#include <iomanip>

using namespace helix::plugin;

static PluginAPI* g_api = nullptr;
static std::string g_plugin_dir;

// Subject for reactive temperature display
static lv_subject_t s_nozzle_temp_subject;
static char s_temp_buffer[16] = "---";

static void update_temperature(float temp) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << temp;
    strncpy(s_temp_buffer, oss.str().c_str(), sizeof(s_temp_buffer) - 1);

    helix::ui::queue_update([]() {
        lv_subject_set_pointer(&s_nozzle_temp_subject, s_temp_buffer);
    });
}

extern "C" bool helix_plugin_init(PluginAPI* api, const char* plugin_dir) {
    g_api = api;
    g_plugin_dir = plugin_dir;

    // Initialize subject with default value
    lv_subject_init_pointer(&s_nozzle_temp_subject, s_temp_buffer);
    api->register_subject("temp-widget.nozzle_temp", &s_nozzle_temp_subject);

    // Register XML component
    if (!api->register_xml_component(plugin_dir, "temp_display.xml")) {
        api->log_error("Failed to register temp_display.xml");
        return false;
    }

    // Subscribe to temperature events (main thread - safe for subjects)
    api->on_event(events::TEMPERATURE_UPDATED, [](const EventData& event) {
        if (event.payload.value("heater", "") == "extruder") {
            float temp = event.payload.value("current", 0.0f);
            update_temperature(temp);
        }
    });

    // Subscribe to Moonraker for real-time updates
    api->subscribe_moonraker({"extruder"}, [](const json& update) {
        if (update.contains("extruder")) {
            float temp = update["extruder"].value("temperature", 0.0f);
            update_temperature(temp);
        }
    });

    // Inject widget when home panel is available
    api->on_event(events::NAVIGATION_CHANGED, [](const EventData& event) {
        if (event.payload.value("panel", "") == "home") {
            if (g_api->has_injection_point("print_status_extras")) {
                g_api->inject_widget("print_status_extras", "temp_display");
            }
        }
    });

    // Try immediate injection (in case home is already showing)
    if (api->has_injection_point("print_status_extras")) {
        api->inject_widget("print_status_extras", "temp_display");
    }

    api->log_info("Temperature Widget initialized");
    return true;
}

extern "C" void helix_plugin_deinit() {
    if (g_api) {
        g_api->unregister_subject("temp-widget.nozzle_temp");
        g_api->log_info("Temperature Widget shutdown");
    }
    g_api = nullptr;
}

extern "C" const char* helix_plugin_api_version() {
    return "1.0";
}
```

### LED Effects Plugin (Full-Featured Reference)

This outline demonstrates a more complex plugin with services, events, and inter-plugin communication.

**Key features:**
- Exposes a service for other plugins to control LEDs
- Subscribes to print events to trigger effects
- Provides settings UI
- Communicates with Klipper via G-code

```cpp
// plugins/led-effects/led_effects.cpp (outline)

class LedEffectsController {
public:
    void set_effect(const std::string& effect);
    void set_color(uint32_t rgb);
    void set_brightness(float percent);
    std::string current_effect() const;

private:
    std::string m_effect = "off";
    uint32_t m_color = 0xFFFFFF;
    float m_brightness = 1.0f;
};

static LedEffectsController s_controller;
static PluginAPI* g_api = nullptr;

extern "C" bool helix_plugin_init(PluginAPI* api, const char* plugin_dir) {
    g_api = api;

    // Register service for other plugins
    api->register_service("led-effects.controller", &s_controller);

    // Automatic effects based on print state
    api->on_event(events::PRINT_STARTED, [](const EventData&) {
        s_controller.set_effect("printing");
        send_led_gcode();
    });

    api->on_event(events::PRINT_COMPLETED, [](const EventData&) {
        s_controller.set_effect("complete");
        send_led_gcode();
    });

    api->on_event(events::PRINT_ERROR, [](const EventData&) {
        s_controller.set_effect("error");
        send_led_gcode();
    });

    // Register settings UI. NOTE: "settings_section" is not a registered injection
    // point yet (only "print_status_extras" is), so this inject_widget() is a no-op
    // today — guarded here to show the intended pattern once it lands.
    api->register_xml_component(plugin_dir, "led_settings.xml");
    if (api->has_injection_point("settings_section")) {
        api->inject_widget("settings_section", "led_settings");
    }

    return true;
}

static void send_led_gcode() {
    if (auto* mrapi = g_api->moonraker_api()) {
        std::string gcode = "SET_LED LED=status_led " +
                           s_controller.get_led_params();
        mrapi->execute_gcode(gcode, []() {}, [](const MoonrakerError&) {});
    }
}
```

---

## Known Limitations

These are documented limitations accepted in the current design.

### Moonraker Subscription Persistence

Once a plugin registers a Moonraker subscription via `MoonrakerClient::register_notify_update()`, the subscription cannot be removed until the client is destroyed. Calling `unsubscribe_moonraker()` marks the subscription as removed in plugin tracking, and the `alive_flag_` pattern ensures callbacks are skipped. The actual WebSocket subscription persists until reconnect. This is minor overhead for long-running sessions.

### Subject Name Collisions

Subjects are registered globally. Two plugins using the same subject name will conflict. Always prefix with your plugin ID: `"my-plugin.subject_name"`. There is no technical enforcement — convention is sufficient.

### Single Injection Point Instance

Each injection point can only exist once in the UI at a time. Injected widgets are removed when navigating away from the panel. This matches HelixScreen's single-panel navigation model. Plugins can re-inject on `NAVIGATION_CHANGED` events.

### Subject Unregistration

Subjects registered via `api->register_subject()` are tracked but not actually unregistered from the XML system during cleanup — `lib/helix-xml/` provides no `lv_xml_unregister_subject()`. That is our own gap, not an upstream one: LVGL 9.5 has no XML engine at all, so nobody is going to add it for us (see `HELIX_XML_FORK.md`). Re-loading a plugin with the same subject names works fine (registration is idempotent). Only affects development scenarios with frequent plugin reloads.

---

## Future Directions

These are enhancement ideas captured for future consideration. Implementation should be driven by actual user needs.

### Filter System

Allow plugins to intercept and modify data, not just observe. Events are fire-and-forget; filters would allow transformation (e.g., G-code preprocessing, temperature calibration offsets). Would require a new `FilterManager` class. Implement when a real use case emerges.

### Plugin Hot-Reload

Reload plugins without restarting the application. Deferred due to complexity: LVGL widget cleanup, subject/observer lifecycle, service dependencies, state preservation, and `dlclose()` limitations. Current approach (restart app after changes) is simple and reliable.

### Plugin Marketplace

In-app discovery and installation of community plugins. Would require registry infrastructure, code signing, version compatibility matrix. Simpler alternatives: documented manual installation, curated plugin lists, GitHub releases, or a CLI install tool.

### Plugin Sandboxing

Currently plugins are trusted code with full access (similar to VS Code extensions, Klipper macros, OctoPrint plugins). Sandboxing would require separate processes with IPC, capability-based permissions, and significantly increased complexity. The trust-based model is appropriate for the user base.

---

## Additional Resources

- [LVGL9_XML_GUIDE.md](LVGL9_XML_GUIDE.md) - Complete XML component reference
- [ARCHITECTURE.md](ARCHITECTURE.md) - HelixScreen system architecture
- [DEVELOPMENT.md](DEVELOPMENT.md) - Development environment setup
- [TESTING.md](TESTING.md) - Testing infrastructure

---

*This documentation is for HelixScreen Plugin API version 1.0. Please check for updates when upgrading HelixScreen versions.*
