# Macros Panel (Developer Guide)

The Macros Panel system: full-screen overlay for browsing and executing Klipper macros, parameter detection and input, home panel widgets, and standard macro slot resolution.

**Key files**: `include/ui_panel_macros.h`, `src/ui/ui_panel_macros.cpp`, `ui_xml/macro_panel.xml`, `ui_xml/macro_card.xml`

---

## Architecture Overview

The Macros Panel is an overlay that lists all discovered Klipper macros, handles dangerous-macro confirmation, detects parameters, and dispatches execution. Two home panel widgets provide quick access.

```
Home Panel
├── MacrosWidget (1x1) ──────────→ Opens MacrosPanel overlay
├── FavoriteMacroWidget (1x1) ──→ Executes a single configured macro
│
MacrosPanel (overlay)
├── macro_panel.xml ──→ overlay_panel with title "Macros"
│   ├── status_message ──→ bound to "macros_status" subject
│   ├── macro_list (scrollable) ──→ populated by C++
│   │   └── macro_card.xml × N ──→ one per visible macro
│   └── empty_state ──→ shown when no macros
│
├── populate_macro_list()
│   └── MoonrakerAPI::hardware().macros() → sorted → filtered → create_macro_card()
│
├── on_macro_card_clicked (static callback)
│   └── fetch_params_and_execute()
│       ├── Dangerous? → modal_show_confirmation() → fetch_params_and_run()
│       └── Safe? → fetch_params_and_run()
│           ├── KNOWN_NO_PARAMS → execute_macro()
│           ├── KNOWN_PARAMS → MacroParamModal → execute_with_params()
│           └── UNKNOWN → MacroParamModal (freeform) → execute_with_params()
│
└── execute_with_params()
    ├── SET_GCODE_VARIABLE for variable overrides
    └── MACRO_NAME KEY=VALUE... via api->execute_gcode()
```

### Key Files

| File | Purpose |
|------|---------|
| `include/ui_panel_macros.h` | MacrosPanel class (extends OverlayBase) |
| `src/ui/ui_panel_macros.cpp` | Implementation -- macro list, click handling, execution |
| `ui_xml/macro_panel.xml` | Layout: overlay_panel with scrollable macro_list + empty_state |
| `ui_xml/macro_card.xml` | Reusable card component: icon + name + chevron, clicked callback |
| `include/macro_param_cache.h` | Pre-parsed macro parameters (populated during discovery) |
| `include/macro_param_modal.h` | Modal for user input when macro has parameters |
| `include/standard_macros.h` | 10 standard macro slots with auto-detection |
| `include/macro_manager.h` | HelixScreen helper macro (helix_macros.cfg) install/update |
| `include/macro_modification_manager.h` | PRINT_START enhancement wizard (beta-gated) |
| `include/ui_settings_macro_buttons.h` | Settings overlay for quick buttons + standard macro config |
| `include/favorite_macro_widget.h` | Home panel 1x1 widget for favorite macro execution |
| `src/ui/panel_widgets/macros_widget.h` | Home panel 1x1 widget to open Macros overlay |
| `ui_xml/components/panel_widget_macros.xml` | XML for macros home panel widget |
| `ui_xml/components/panel_widget_favorite_macro.xml` | XML for favorite macro home widget |

---

## Macro Discovery and Display

Macros come from `MoonrakerAPI::hardware().macros()`, populated during printer discovery. The panel sorts them alphabetically and filters system macros (prefixed with `_`) by default, with a toggle to show them.

### Display Name Prettification

`prettify_macro_name()` converts raw Klipper macro names to human-readable form:

- `CLEAN_NOZZLE` becomes "Clean Nozzle"
- `_HELIX_BED_MESH` becomes "_Helix Bed Mesh" (system prefix preserved)

---

## Dangerous Macro Protection

These macros show a confirmation dialog before execution:

- `SAVE_CONFIG`
- `FIRMWARE_RESTART`
- `RESTART`
- `SHUTDOWN`
- `M112`
- `EMERGENCY_STOP`

The confirmation uses `modal_show_confirmation()` with appropriate severity level.

---

## Parameter Handling

MacroParamCache pre-parses parameters during macro discovery and classifies each macro into one of three knowledge levels:

| Level | Behavior |
|-------|----------|
| **KNOWN_NO_PARAMS** | Execute immediately, no modal |
| **KNOWN_PARAMS** | Show MacroParamModal with detected fields and defaults |
| **UNKNOWN** | Show MacroParamModal with freeform parameter input |

### Parameter Flavors

Parameters come in two types, handled differently at execution time:

| Type | Source | Execution |
|------|--------|-----------|
| **Variables** | `variable_*` in Klipper macro config | `SET_GCODE_VARIABLE` commands sent before macro call |
| **Params** | Jinja2 `params.*` references | Appended as `KEY=VALUE` to the macro call |

---

## Standard Macros System

10 semantic slots provide consistent access to common operations:

- LoadFilament, UnloadFilament, Purge, Pause, Resume, Cancel
- BedMesh, BedLevel, CleanNozzle, HeatSoak

### Resolution Priority

1. User configured (via Settings) — verified to exist on the printer before it is trusted
2. Auto-detected (pattern matching against discovered macros)
3. HELIX fallback (built-in helper macros from `helix_macros.cfg`)
4. Empty (slot not available)

A configured name the printer does not define is demoted at init and the slot
resolves as though unconfigured. In the Controls panel a demoted slot keeps its
button in place but greys it out — a button that disappears reads as a bug in
the screen, while a disabled one points at the assignment that needs fixing —
rather than sending a command Klipper would reject with "Unknown command".

See `STANDARD_MACROS_SPEC.md` for the full specification.

---

## Home Panel Integration

Two widget types appear on the home panel:

| Widget | Size | Behavior |
|--------|------|----------|
| **MacrosWidget** | 1x1 | Opens the full MacrosPanel overlay |
| **FavoriteMacroWidget** | 1x1, up to 5 slots | Executes a single configured macro directly |

### Macro Settings

`MacroButtonsOverlay` in settings provides:

- Quick button configuration (which macros appear on home panel)
- Standard macro slot overrides (override auto-detection per slot)

---

## Key Patterns

### Global Singleton Access

MacrosPanel uses the global panel pattern for static callback access:

```cpp
DEFINE_GLOBAL_PANEL(MacrosPanel, g_macros_panel, get_global_macros_panel)

void MacrosPanel::on_macro_card_clicked(lv_event_t* e) {
    auto& self = get_global_macros_panel();
    // ... find card in self.macro_entries_
}
```

### Overlay Navigation

Opening the MacrosPanel from any widget or panel:

```cpp
helix::ui::lazy_create_and_push_overlay<MacrosPanel>(
    get_global_macros_panel, macros_panel_, parent_screen_,
    "Macros", "CallerName", true);
```

### Async Safety

WebSocket callbacks capture copies of data, never references:

```cpp
std::string macro_copy = macro_name;
api->execute_gcode(gcode,
    [macro_copy]() { spdlog::info("Executed {}", macro_copy); },
    [macro_copy](const MoonrakerError& err) { ... });
```

### Parameter Modal with Alive Guard

The alive guard prevents use-after-free when the panel is destroyed while a modal is open:

```cpp
std::weak_ptr<bool> weak = alive_;
param_modal_.show_for_macro(lv_screen_active(), name, params,
    [this, weak, name](const MacroParamResult& result) {
        if (weak.expired()) return;  // Panel destroyed during modal
        execute_with_params(name, result);
    });
```

---

## XML Components

### macro_panel.xml

Extends `overlay_panel` with:

- Title "Macros" (translatable)
- Status message bound to `macros_status` subject
- Scrollable `macro_list` container (populated dynamically in C++)
- Empty state with icon + message (hidden by default, shown when no macros match)
- Disabled state bound to `nav_buttons_enabled` subject

### macro_card.xml

Extends `lv_button` with API props:

- `macro_name` (string) -- displayed as bold text
- `macro_description` (string) -- optional subtitle (hidden by default)
- Icon: `code_tags`, chevron right indicator
- Click callback: `on_macro_card_clicked`

---

## Testing

| Test File | Coverage |
|-----------|----------|
| `test_helix_macro_manager.cpp` | Helper macro install/update |
| `test_macro_param_cache.cpp` | Parameter caching |
| `test_macro_param_parser.cpp` | Jinja2 parameter parsing |
| `test_standard_macros.cpp` | Slot resolution and auto-detection |
| `test_subject_macros.cpp` | XML subject bindings |
| `test_notification_macros.cpp` | Toast notifications |
| `test_settings_macro_buttons_char.cpp` | Settings UI |
| `test_led_macro_backend.cpp` | LED integration |

---

## Beta-Gated: PRINT_START Enhancement Wizard

Gated behind `Config::is_beta_features_enabled()`.

- `MacroModificationManager` + `MacroEnhanceWizard`
- Step-by-step wizard to enhance PRINT_START for phase tracking
- Future: `create_backup` parameter support, pending Moonraker API support upstream

---

## Related Docs

- `STANDARD_MACROS_SPEC.md` -- Full spec for the 10 standard macro slots
- `MODAL_SYSTEM.md` -- Modal architecture used by confirmation dialogs and MacroParamModal
