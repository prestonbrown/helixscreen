# Multi-Printer Management (Developer Guide)

How the multi-printer system works internally -- config schema, soft restart lifecycle, UI components, and how to extend it.

---

## Overview

Multi-printer management allows users to configure, switch between, add, and delete multiple Klipper printers from a single HelixScreen device. Each printer has its own connection settings (Moonraker host/port), hardware configuration, macros, filament settings, and panel widgets.

Key properties:

- **Beta-gated** -- all entry points are wrapped in `<beta_feature>` XML tags, requiring both `show_beta_features` and `multi_printer_enabled` subjects to be true for full visibility
- **Config schema v4** -- per-printer data lives under `/printers/{id}/`, with `df()` routing dynamically to the active printer
- **Soft restart** -- switching printers tears down and reinitializes the entire printer state without restarting the application or LVGL display

### Key Files

| File | Purpose |
|------|---------|
| `include/config.h` | Config class: `df()`, CRUD methods, `slugify()`, `CURRENT_CONFIG_VERSION` |
| `src/system/config.cpp` | Config v4 schema, `migrate_v3_to_v4()`, multi-printer CRUD |
| `include/application.h` | `switch_printer()`, `add_printer_via_wizard()`, `cancel_add_printer_wizard()`, soft restart state |
| `src/application/application.cpp` | Soft restart lifecycle: `tear_down_printer_state()`, `init_printer_state()` |
| `include/ui_printer_switch_menu.h` | `PrinterSwitchMenu` context menu (extends `ContextMenu`) |
| `src/ui/ui_printer_switch_menu.cpp` | Navbar badge popup: printer list, checkmark active, add button |
| `include/ui_printer_list_overlay.h` | `PrinterListOverlay` (extends `OverlayBase`) |
| `src/ui/ui_printer_list_overlay.cpp` | Settings management: switch, add, delete with confirmation |
| `src/ui/ui_printer_manager_overlay.cpp` | "Manage Printers" button in Printer Manager overlay |
| `src/ui/ui_panel_settings.cpp` | "Printers" row in Settings panel |
| `src/ui/ui_nav_manager.cpp` | Printer badge click handler, switch/add callbacks, `PrinterSwitchMenu` ownership |
| `include/printer_state.h` | `active_printer_name_`, `multi_printer_enabled_` subjects |
| `src/printer/printer_state.cpp` | Subject initialization and setters |
| `include/wizard_config_paths.h` | Per-printer vs device-level config path constants |
| `ui_xml/printer_switch_menu.xml` | Context menu layout (backdrop, printer list, add button) |
| `ui_xml/printer_list_overlay.xml` | Overlay layout (overlay_panel, list container, add button) |
| `ui_xml/printer_list_item.xml` | Reusable row component (name, check icon, delete button, active state) |
| `ui_xml/navigation_bar.xml` | Navbar badge with dual `bind_flag_if_eq` gating |
| `ui_xml/printer_manager_overlay.xml` | Section 4: "Manage Printers" button in `<beta_feature>` |
| `ui_xml/settings_panel.xml` | "Printers" action row in `<beta_feature>` |
| `ui_xml/beta_feature.xml` | Reusable wrapper: auto-hides when `show_beta_features` is 0, adds orange left border + BETA badge |

---

## Architecture

```
Config (v4 schema)
  |  /printers/{id}/ per-printer data
  |  /active_printer_id → current printer slug
  |  df() → "/printers/{active_id}/"
  |
Application (soft restart orchestrator)
  |  switch_printer() → tear_down + init
  |  add_printer_via_wizard() → create entry + wizard + cancel recovery
  |
NavigationManager (callback bridge)
  |  set_printer_callbacks(switch_cb, add_cb)
  |  trigger_printer_switch(), trigger_add_printer()
  |  on_printer_badge_clicked() → PrinterSwitchMenu
  |
  +-- PrinterSwitchMenu (context menu, navbar badge)
  |     Popup near badge: lists printers with checkmark, add button
  |     Actions dispatched via queue_update() to avoid re-entrancy
  |
  +-- PrinterListOverlay (settings overlay)
  |     Full overlay: switch, add, delete (with confirmation modal)
  |     Also reachable from Settings > Printers
  |
  +-- PrinterManagerOverlay (Section 4)
        "Manage Printers" button → opens PrinterListOverlay
```

### Subject Bindings

| Subject | Type | Description |
|---------|------|-------------|
| `active_printer_name` | string | Human-readable name of the active printer, bound to navbar badge label |
| `multi_printer_enabled` | int (0/1) | 1 when more than one printer configured. Controls badge visibility. |
| `show_beta_features` | int (0/1) | Global beta features toggle. Required alongside `multi_printer_enabled` for badge. |

The navbar badge uses dual gating:

```xml
<ui_button name="nav_printer_badge" hidden="true">
  <bind_flag_if_eq subject="multi_printer_enabled" flag="hidden" ref_value="0"/>
  <bind_flag_if_eq subject="show_beta_features" flag="hidden" ref_value="0"/>
</ui_button>
```

Both subjects must be non-zero for the badge to appear.

---

## Config Schema (v4)

### Structure

```json
{
  "config_version": 4,
  "active_printer_id": "voron-2-4",
  "printers": {
    "voron-2-4": {
      "printer_name": "Voron 2.4",
      "type": "Voron 2.4 350mm",
      "wizard_completed": true,
      "moonraker_host": "192.168.1.100",
      "moonraker_port": 7125,
      "moonraker_api_key": false,
      "heaters": { "bed": "heater_bed", "hotend": "extruder" },
      "temp_sensors": { "bed": "heater_bed", "hotend": "extruder" },
      "fans": { "part": "fan", "hotend": "heater_fan hotend_fan" },
      "leds": { "strip": "", "selected": [] },
      "default_macros": { ... },
      "hardware": { "optional": [], "expected": [], "last_snapshot": {} },
      "filament": { ... },
      "panel_widgets": { ... },
      "printer_image": "shipped:voron-24r2"
    },
    "ender-3-pro": {
      "printer_name": "Ender 3 Pro",
      "wizard_completed": true,
      "moonraker_host": "192.168.1.101",
      "moonraker_port": 7125,
      ...
    }
  },
  "display": { ... },
  "input": { ... },
  "language": "en",
  "beta_features": true,
  ...
}
```

### df() Dynamic Routing

The `Config::df()` method returns the JSON pointer prefix for the active printer:

```cpp
std::string Config::df() {
    return "/printers/" + active_printer_id_ + "/";
}
```

All per-printer reads and writes use `df()` as a prefix:

```cpp
// Read printer's Moonraker host
std::string host = cfg->get<std::string>(cfg->df() + "moonraker_host", "127.0.0.1");

// Write printer's name
cfg->set<std::string>(cfg->df() + "printer_name", "My Voron");
cfg->save();
```

When `set_active_printer()` is called, `df()` immediately routes to the new printer's section. All existing code that uses `df()` transparently works with multi-printer -- no code changes needed for per-printer settings.

### Per-Printer vs Device-Level Paths

`wizard_config_paths.h` documents the distinction:

| Category | Path Style | Example |
|----------|-----------|---------|
| **Per-printer** (suffix) | `config->df() + path` | `config->df() + "moonraker_host"` → `/printers/voron/moonraker_host` |
| **Device-level** (absolute) | Direct path | `"/wifi/ssid"`, `"/display/sleep_sec"`, `"/language"` |

Per-printer settings include: connection (host, port, API key), heaters, fans, LEDs, sensors, macros, hardware capabilities, filament config, panel widgets, printer image.

Device-level settings include: WiFi, display, input/touch calibration, language, theme, brightness, sounds, beta_features.

### Migration v3 to v4

`migrate_v3_to_v4()` restructures legacy single-printer configs:

1. Reads the old `/printer` object
2. Generates a slug ID from the printer name (e.g., "Voron 2.4" becomes "voron-2-4") via `Config::slugify()`
3. Moves root-level `/filament` and `/panel_widgets` into the printer entry
4. Copies root-level `wizard_completed` into the printer entry
5. Migrates `/display/printer_image` to per-printer path
6. Creates `/printers/{slug}` map and sets `active_printer_id`
7. Removes the old `/printer` key

The migration is idempotent -- if `/printers` already exists, it is skipped.

### Config CRUD

```cpp
// List all printer IDs
std::vector<std::string> ids = cfg->get_printer_ids();

// Add a new printer
json printer_data = {{"wizard_completed", false}};
cfg->add_printer("new-printer", printer_data);

// Switch active printer (updates df() routing)
cfg->set_active_printer("new-printer");

// Remove a printer (refuses if it's the last one)
cfg->remove_printer("old-printer");

// Generate slug from name
std::string slug = Config::slugify("My Voron 2.4");  // "my-voron-2-4"
```

`remove_printer()` prevents removing the last printer and auto-selects the first remaining printer if the active one is removed.

---

## Soft Restart Lifecycle

Switching printers performs a "soft restart" -- the LVGL display stays alive, but all printer state, subjects, UI panels, Moonraker connections, and plugins are torn down and rebuilt.

### tear_down_printer_state() (Steps 0-20)

```
 0. Clear wizard cancel callback (prevents stale captures)
 1. Clear app_globals (moonraker_manager, api, client, history managers)
 2. NavigationManager::shutdown() (deactivate overlays, clear stacks, clear printer callbacks)
 3. Stop UpdateChecker auto-check timer
 4. Unload plugins
 5. Freeze UpdateQueue (ScopedFreeze — blocks background enqueue)
 5b. Disconnect WebSocket (no more producers)
 6. Drain update queue (discard pending async callbacks)
 6b. Clear JobQueueState global pointer
 7. Release history managers
 8. Unregister timelapse callback
 9. Unregister action prompt + AMS gcode callbacks
10. Clear AMS backends (subscription guards hold raw client pointers)
11. Deinit LedController
12. Release PanelFactory + SubjectInitializer
13. Kill all LVGL animations + clear ModalStack
14. StaticPanelRegistry::destroy_all() (destroys overlay/panel globals, releases ObserverGuards)
15. Release global observer guards (notification, active print media)
16. StaticSubjectRegistry::deinit_all() (deinit subjects in LIFO order)
16b. Destroy JobQueueState
17. ObserverGuard::invalidate_all() (mark all guards as stale)
18. Release MoonrakerManager
19. Reset KeyboardManager
20. Delete LVGL widget tree (lv_obj_del on app_layout)
```

### init_printer_state() (Steps 1-10)

```
 1. Reinitialize UpdateQueue (before moonraker — background threads need the queue)
 2. Init core subjects (PrinterState, AmsState, etc.)
 2b. Set multi-printer subjects (active_printer_name, multi_printer_enabled) from config
 3. Init Moonraker (creates client + API + history managers)
 4. Init panel subjects (with API injection + post-init)
 5. Recreate UI (app_layout from XML, wire navigation + printer callbacks)
 6. Run wizard if needed (new printer with wizard_completed=false)
 7. Create overlay panels (if not in wizard)
 8. Reload plugins
 9. Connect to new printer's Moonraker
10. ObserverGuard::revalidate_all() (all old guards cleared, new observers attached)
    Force full screen refresh
```

### Key Safety Mechanisms

**ScopedFreeze**: During teardown, the update queue is frozen (step 5) to prevent the WebSocket background thread from enqueuing new callbacks between `drain()` and widget destruction. The freeze thaws when it goes out of scope.

**ObserverGuard invalidate/revalidate**: At teardown step 17, `ObserverGuard::invalidate_all()` marks all existing guards as stale. During init, when old guards are reassigned (`guard = observe_*()`), the move-assignment safely releases instead of calling `lv_observer_remove()` on freed observer pointers. After init step 10, `revalidate_all()` re-enables normal guard behavior.

**Re-entrancy guard**: `m_soft_restart_in_progress` (bool in Application) prevents `switch_printer()`, `add_printer_via_wizard()`, and `cancel_add_printer_wizard()` from being called during an active soft restart.

---

## Switch Printer Flow

```
User taps navbar printer badge
  → NavigationManager::on_printer_badge_clicked()
  → PrinterSwitchMenu::show() (context menu near badge)
  → User taps a printer row
  → handle_printer_selected() → dispatch via queue_update()
  → NavigationManager callback → Application::switch_printer(printer_id)
    1. Set re-entrancy guard
    2. Config::set_active_printer(printer_id) + save
    3. tear_down_printer_state()
    4. init_printer_state()
    5. Navigate to Home panel
    6. Show toast: "Connected to {printer_name}"
    7. Clear re-entrancy guard
```

Alternate path: Settings > Printers > PrinterListOverlay > tap row:

```
PrinterListOverlay::handle_switch_printer()
  → queue_update: NavigationManager::go_back() + trigger_printer_switch()
  → Same Application::switch_printer() path
```

---

## Add Printer Flow

```
User taps "+ Add Printer" (in PrinterSwitchMenu or PrinterListOverlay)
  → Application::add_printer_via_wizard()
    1. Set re-entrancy guard
    2. Generate unique ID ("printer-N", loop to avoid collisions)
    3. Store previous printer ID for cancel recovery
    4. Create empty printer entry with wizard_completed=false
    5. Set as active printer, save config
    6. tear_down_printer_state()
    7. Register cancel callback (cancel_add_printer_wizard)
    8. init_printer_state()
       └── is_wizard_required() returns true → wizard launches
    9. Clear re-entrancy guard
```

### Cancel Recovery

If the user cancels the wizard:

```
cancel_add_printer_wizard()
  1. Remove the incomplete printer entry from config
  2. Restore previous active printer ID
  3. Save config, clear recovery state
  4. Defer via queue_update (inside click handler):
     a. Clear wizard state
     b. tear_down_printer_state()
     c. init_printer_state()
     d. Navigate to Home
```

The cancel callback is deferred via `queue_update()` because it is called from a wizard button click handler -- the wizard container must survive until the event callback returns.

### Crash Recovery

At startup (Phase 11b in `application.cpp`), the app detects stale incomplete printer entries:

1. If `is_wizard_required()` is true for the active printer
2. Search for another printer with `wizard_completed=true`
3. If found: remove the stale entry, switch to the completed printer, save config
4. If not found: the normal wizard flow runs for the remaining printer

---

## Delete Printer Flow

```
PrinterListOverlay → user taps delete icon on a row
  → handle_delete_printer()
  → modal_show_confirmation("Remove Printer", ..., ModalSeverity::Error)
  → User confirms
  → on_delete_confirm_cb()
    1. Close the confirmation modal
    2. Config::remove_printer(printer_id) + save
    3. If deleted printer was active:
       queue_update → go_back() + trigger_printer_switch(remaining.front())
    4. If deleted printer was inactive:
       queue_update → update multi_printer_enabled subject + repopulate list
```

`Config::remove_printer()` prevents deleting the last printer:

```cpp
if (data["printers"].size() <= 1) {
    spdlog::error("[Config] Cannot remove last printer");
    return;
}
```

---

## Beta Gating

All multi-printer UI entry points are wrapped in the `<beta_feature>` XML component:

```xml
<beta_feature>
  <ui_button text="Manage Printers">
    <event_cb trigger="clicked" callback="pm_manage_printers_clicked"/>
  </ui_button>
</beta_feature>
```

The `<beta_feature>` component (`ui_xml/beta_feature.xml`) is a reusable wrapper that:
- Hides its contents when `show_beta_features` subject is 0
- Adds an orange left border accent and floating BETA badge when visible

Gated entry points:
- **Navbar printer badge** -- dual binding on `multi_printer_enabled` AND `show_beta_features`
- **Settings > Printers** row -- wrapped in `<beta_feature>`
- **Printer Manager > Manage Printers** button -- wrapped in `<beta_feature>`

The `show_beta_features` subject is initialized from `Config::is_beta_features_enabled()` which returns true if the `beta_features` config key is true, or if running in `--test` mode.

---

## Threading & Safety

### Deferred Actions

All UI actions that trigger a soft restart must be deferred via `queue_update()`:

```cpp
// PrinterSwitchMenu dispatches via queue_update to avoid re-entrancy
void PrinterSwitchMenu::dispatch_switch_action(MenuAction action,
                                                const std::string& printer_id) {
    auto callback = switch_callback_;
    s_active_instance_ = nullptr;
    hide();
    if (callback) {
        helix::ui::queue_update([callback, action, printer_id]() {
            callback(action, printer_id);
        });
    }
}
```

This ensures the menu widget is fully cleaned up before the soft restart tears down the widget tree.

### ScopedFreeze During Teardown

Step 5 of `tear_down_printer_state()` freezes the update queue:

```cpp
auto queue_freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
```

This prevents the WebSocket background thread from enqueuing new callbacks between `update_queue_shutdown()` and widget destruction. The freeze remains active through step 20 (widget tree deletion) and thaws when the local variable goes out of scope.

### Re-entrancy Guard

```cpp
bool m_soft_restart_in_progress = false;
```

Checked at the top of `switch_printer()`, `add_printer_via_wizard()`, and `cancel_add_printer_wizard()`. If true, the call is logged and ignored.

### ObserverGuard Lifecycle

During soft restart, observer guards in surviving singletons (not destroyed by `StaticPanelRegistry`) hold freed observer pointers. The invalidate/revalidate protocol handles this:

1. `ObserverGuard::invalidate_all()` at end of teardown (step 17)
2. During init, when guards are reassigned (`guard = observe_*()`), `reset()` safely releases instead of calling `lv_observer_remove()` on freed memory
3. `ObserverGuard::revalidate_all()` at end of init (step 10) restores normal behavior

---

## Developer Extension Guide

### Adding Per-Printer Config Settings

Use the `df()` prefix for any setting that should be per-printer:

```cpp
// Read
int threshold = cfg->get<int>(cfg->df() + "my_feature/threshold", 50);

// Write
cfg->set<int>(cfg->df() + "my_feature/threshold", new_value);
cfg->save();
```

If adding a constant path suffix used by wizard screens, declare it in `wizard_config_paths.h`:

```cpp
namespace helix::wizard {
constexpr const char* MY_FEATURE_THRESHOLD = "my_feature/threshold";
}
// Usage: cfg->get<int>(cfg->df() + wizard::MY_FEATURE_THRESHOLD, 50)
```

Device-level settings (display, WiFi, language) use absolute paths and do NOT get the `df()` prefix.

### Handling Soft Restart in New Singletons

If you create a singleton that manages LVGL subjects:

1. **Register with StaticSubjectRegistry** inside `init_subjects()`:

```cpp
void MySingleton::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(
        "MySingleton", []() { MySingleton::instance().deinit_subjects(); });
}
```

2. **Implement `deinit_subjects()`** to call `lv_subject_deinit()` on each subject.

3. If your singleton creates ObserverGuards, they will be safely handled by the `invalidate_all()`/`revalidate_all()` protocol during soft restart.

4. If your singleton is a panel/overlay, register with `StaticPanelRegistry` so it is destroyed during teardown step 14.

### Testing Multi-Printer

In `--test` mode (SDL desktop build), press the **P** key:

- **First press**: Creates a test printer ("Voron 2.4" with ID "voron-24") and enables the `multi_printer_enabled` subject so the navbar badge appears
- **Subsequent presses**: Cycles through configured printers, triggering a full soft restart each time

This exercises the entire switch_printer flow without needing real hardware.

---

## UI Components

### PrinterSwitchMenu (Navbar Badge)

A `ContextMenu` subclass that appears near the navbar printer badge when tapped.

```
┌──────────────────┐
│ Switch Printer    │
│──────────────────│
│ ✓ Voron 2.4      │  ← active printer (checkmark)
│   Ender 3 Pro    │  ← tap to switch
│──────────────────│
│ + Add Printer     │  ← launches wizard
└──────────────────┘
```

The printer list is populated imperatively (not XML-bound) because it is dynamic and short-lived. Rows are created with `lv_obj_create()` + `lv_label_create()` in `populate_printer_list()`.

### PrinterListOverlay (Settings Management)

An `OverlayBase` subclass accessed from Settings > Printers or Printer Manager > Manage Printers.

```
┌──────────────────────────┐
│ ← Manage Printers        │
│──────────────────────────│
│ ┃ ✓ Voron 2.4     ● 🗑  │  ← checked state = active, green dot, delete
│   Ender 3 Pro      ● 🗑  │  ← tap row to switch, tap 🗑 to delete
│                          │
│ [ + Add Printer        ] │
└──────────────────────────┘
```

Each row is created from the `printer_list_item` XML component, which provides:
- `LV_STATE_CHECKED` styling (left accent border) for the active printer
- Check icon with conditional opacity
- Connection status dot
- Delete button (hidden when only one printer configured)

The list is repopulated on every `on_activate()` to reflect config changes.

### PrinterManagerOverlay (Manage Printers Button)

Section 4 of the Printer Manager overlay contains a "Manage Printers" button wrapped in `<beta_feature>`. Clicking it opens the `PrinterListOverlay`.

---

## Testing

```bash
# Run in test mode with multi-printer testing
./build/bin/helix-screen --test -vv
# Press P to create test printer and cycle between printers

# Config migration tests
./build/bin/helix-tests "[config]"
```
