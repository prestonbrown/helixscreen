# Session Handoff Document

**Last Updated:** 2025-10-26 Very Late Night
**Current Focus:** Step Progress Widget Complete - Ready for Wizard Integration

---

## Recent Work (2025-10-26 Very Late Night - Session 3)

### Step Progress Widget Implementation ✅ COMPLETE

**Created reusable step-by-step progress indicator widget for wizards and multi-step operations.**

**What Was Built:**
- Hybrid XML+C++ widget with clean API
- Supports vertical and horizontal orientations
- Three visual states: PENDING (gray filled), ACTIVE (red filled), COMPLETED (green filled)
- Step numbers (1, 2, 3...) automatically toggle to checkmarks on completion
- Seamless connector lines (1px width) between steps, colored based on completion

**Files Created:**
- `include/ui_step_progress.h` - Public API
- `src/ui_step_progress.cpp` - Widget implementation (455 lines)
- `ui_xml/step_progress_test.xml` - Test panel
- `src/ui_panel_step_test.cpp` - Test panel implementation

**API Usage:**
```cpp
// Create widget
ui_step_t steps[] = {
    {"Nozzle heating", UI_STEP_STATE_COMPLETED},
    {"Prepare to retract", UI_STEP_STATE_ACTIVE},
    {"Retracting", UI_STEP_STATE_PENDING},
    {"Retract done", UI_STEP_STATE_PENDING}
};
lv_obj_t* progress = ui_step_progress_create(parent, steps, 4, false);  // false = vertical

// Update current step
ui_step_progress_set_current(progress, 2);  // Advances to step 3
```

**Key Technical Details:**
- Connector positioning uses `lv_obj_update_layout()` + `LV_OBJ_FLAG_IGNORE_LAYOUT`
- Border-aware positioning: 13px offset accounts for 2px border drawn inside circles
- Separate `connector_index` tracking prevents state confusion during updates
- Uses `LV_SYMBOL_OK` for cross-platform checkmark compatibility

**Next Steps:**
- Integrate into first-run wizard screens for progress tracking
- Add to leveling wizard when implemented
- Use in filament load/retract calibration workflows

---

## Earlier Work (2025-10-26 Very Late Evening - Session 2)

### Wizard Input Field Improvements + Design Token Cleanup ✅ COMPLETE

**Fixed critical wizard UX bug and cleaned up design system.**

**Bug Fixed:**
- Wizard input fields were completely invisible due to LVGL flex layout bug
- Root cause: `wizard_content` container with `flex_grow="1"` collapsed to zero height
- Solution: Added `scrollable="true"` to force dimension calculation (wizard_container.xml:67)

**Improvements Made:**
1. Enhanced input field visibility:
   - 3px white borders for definition
   - Placeholder text ("192.168.1.100", "7125")
   - Font size increased to montserrat_20
   - Darker background (#card_bg_dark) for contrast

2. Design token consolidation:
   - Removed 4 duplicate constants from globals.xml
   - Now reuse existing tokens: #header_height, #padding_card, #card_radius, "100%"
   - Only kept #input_border_width (3px - unique value)
   - Applied across all 6 wizard XML files

**Files Modified:**
- ui_xml/wizard_container.xml - Scrollable fix
- ui_xml/wizard_connection.xml - Enhanced styling
- ui_xml/globals.xml - Removed duplicates
- ui_xml/wizard_*.xml (5 files) - Consolidated constants

**Status:** All wizard screens now have consistent, visible input styling with minimal design token bloat.

---

## Earlier Work (2025-10-26 Late Evening - Session 1)

### Real Printer Connection Testing + First-Run Wizard Planning ✅ COMPLETE

**Successfully connected to real Voron V2 printer at 192.168.1.112!**

**Bug Fixes:**

1. **Async Timing Issue** (`src/main.cpp:633-668`):
   - **Problem**: Called `discover_printer()` before WebSocket actually connected
   - **Fix**: Moved discovery into `on_connected` callback

2. **Lambda Capture Error** (`src/main.cpp:637`):
   - **Problem**: Tried to capture static `printer_state` by reference
   - **Fix**: Removed capture (static vars don't need capture)

3. **JSON-RPC Params Bug** (`src/moonraker_client.cpp:157-171`):
   - **Problem**: Moonraker rejected empty params object `"params": {}`
   - **Fix**: Only include params field if not null/empty:
     ```cpp
     if (!params.is_null() && !params.empty()) {
         rpc["params"] = params;
     }
     ```

**Real Printer Test Results:**
- Connected to ws://192.168.1.112:7125/websocket
- Moonraker v0.9.3, hostname: voronv2
- Discovered: 2 heaters, 6 sensors, 6 fans, 3 LEDs
- 22 objects subscribed successfully

**First-Run Wizard Planning:**
- Added Phase 11 to ROADMAP.md with comprehensive requirements
- Wizard screens: Connection → Hardware Mapping → Summary → Save
- Auto-default behavior when only one component exists
- mDNS optional (not universally enabled), manual IP primary method

**Files Modified:**
- `helixconfig.json` - Changed moonraker_host to 192.168.1.112
- `src/main.cpp` - Fixed async callback timing
- `src/moonraker_client.cpp` - Fixed JSON-RPC params handling
- `docs/ROADMAP.md` - Added Phase 11 (First-Run Configuration Wizard)

### Earlier: Moonraker Integration Foundation ✅ COMPLETE (2025-10-26 Morning)
- Integrated libhv WebSocket library (static linking via parent repo)
- Created `MoonrakerClient` wrapper class with JSON-RPC support
- Created `PrinterState` reactive state manager with LVGL subjects
- Cross-platform build system (macOS/Linux-aware NPROC + linker flags)
- Increased `LV_DRAW_THREAD_STACK_SIZE` to 32KB (eliminates warning)
- Files: `include/moonraker_client.h`, `src/moonraker_client.cpp`, `include/printer_state.h`, `src/printer_state.cpp`, `Makefile`, `lv_conf.h`

---

## Project Status

**All UI components complete. Infrastructure ready for Moonraker integration.**

Navigation system robust. All panels render correctly across all screen sizes. Reactive state management infrastructure in place with LVGL subjects.

### What Works
- ✅ Navigation system with history stack
- ✅ All UI panels functional with mock data
- ✅ Responsive design (480×320 to 1280×720)
- ✅ Material Design icons with dynamic recoloring
- ✅ **Config** - JSON-based configuration with auto-migration
- ✅ **MoonrakerClient** - WebSocket client with auto-discovery
- ✅ **PrinterState** - Reactive state manager with subjects
- ✅ **Cross-platform build** - macOS/Linux-aware Makefile
- ✅ **Connection on Startup** - Connects and discovers printer automatically

### Next Steps (Priority Order)
1. 🎯 **Implement First-Run Wizard** (Phase 11 in ROADMAP.md)
   - Connection screen with IP/port entry
   - Hardware mapping screens (bed, hotend, fans, LEDs)
   - Config validation and storage
   - Settings panel integration for re-running wizard

2. 🔌 **Bind UI to real subjects** - Replace mock data with printer_state subjects in XML
   - Home panel: connection state, temps, print status
   - Controls panels: real-time position, temperatures

3. 🔌 **Implement control actions** - Wire buttons to gcode_script() calls
   - Motion: jog commands, homing
   - Temps: SET_HEATER_TEMPERATURE commands
   - Extrusion: EXTRUDE/RETRACT commands

4. 📁 **File operations** - Get real print files from Moonraker
   - server.files.list integration
   - Print select panel with real data
   - Thumbnail extraction

---

## Critical Architecture Patterns

### Navigation System

Always use `ui_nav_push_overlay()` and `ui_nav_go_back()`:

```cpp
// Show overlay panel
ui_nav_push_overlay(motion_panel);

// Back button
ui_nav_go_back();  // Handles stack, shows previous or HOME
```

Nav bar buttons clear stack automatically. State preserved when navigating back.

**CRITICAL:** Never hide `app_layout` - prevents navbar disappearing.

### Subject Initialization Order

Subjects MUST be initialized BEFORE XML creation:

```cpp
// 1. Register XML components
lv_xml_component_register_from_file("A:/ui_xml/globals.xml");

// 2. Initialize subjects FIRST
ui_nav_init();
ui_panel_home_init_subjects();

// 3. NOW create UI
lv_obj_t* screen = lv_xml_create(NULL, "app_layout", NULL);
```

### Event Callbacks

Use `<lv_event-call_function>`, NOT `<event_cb>`:

```xml
<lv_button name="my_button">
    <lv_event-call_function trigger="clicked" callback="my_handler"/>
</lv_button>
```

Register in C++ before XML loads:
```cpp
lv_xml_register_event_cb(NULL, "my_handler", my_handler_function);
```

### Component Names

Always add explicit `name` attributes to component tags:

```xml
<lv_obj name="content_area">
  <controls_panel name="controls_panel"/>  <!-- Explicit name required -->
</lv_obj>
```

### Name-Based Widget Lookup

Always use names, never indices:

```cpp
// ✓ CORRECT
lv_obj_t* widget = lv_obj_find_by_name(parent, "widget_name");

// ✗ WRONG
lv_obj_t* widget = lv_obj_get_child(parent, 3);
```

---

## Next Priority: Moonraker Integration 🔌

**All UI complete. Ready to connect to live printer.**

### Step 1: WebSocket Foundation
- Review existing HelixScreen Moonraker client code (parent repo)
- Adapt libhv WebSocket implementation
- Connect on startup, handle connection events

### Step 2: Printer Status Updates
- Subscribe to printer object updates
- Wire temperature subjects to live data
- Update home panel with real-time temps

### Step 3: Motion & Control Commands
- Jog buttons → `printer.gcode.script` (G0/G1)
- Temperature presets → M104/M140 commands
- Home buttons → G28 commands

### Step 4: Print Management
- File list → `server.files.list` API
- Print start/pause/resume/cancel commands
- Live print status updates

**Existing subjects (already wired):**
- Print progress, layer, elapsed/remaining time
- Nozzle/bed temps, speed, flow
- Print state (Printing/Paused/Complete)

---

## Testing Commands

```bash
# Build
make                          # Incremental build (auto-parallel)
make clean && make            # Clean rebuild

# Run
./build/bin/helix-ui-proto                    # Default (medium, home panel)
./build/bin/helix-ui-proto -s tiny            # 480×320
./build/bin/helix-ui-proto -s large           # 1280×720
./build/bin/helix-ui-proto -p controls        # Start at Controls
./build/bin/helix-ui-proto -p print-select    # Print select

# Controls
# Cmd+Q (macOS) / Win+Q (Windows) to quit
# 'S' key to save screenshot

# Screenshot
./scripts/screenshot.sh helix-ui-proto output-name [panel-name]
```

**Screen sizes:** tiny (480×320), small (800×480), medium (1024×600), large (1280×720)

**Panel names:** home, controls, motion, nozzle-temp, bed-temp, extrusion, print-select, file-detail, filament, settings, advanced

---

## Documentation

- **[STATUS.md](STATUS.md)** - Complete chronological development journal
- **[ROADMAP.md](docs/ROADMAP.md)** - Planned features
- **[LVGL9_XML_GUIDE.md](docs/LVGL9_XML_GUIDE.md)** - LVGL 9 XML reference
- **[QUICK_REFERENCE.md](docs/QUICK_REFERENCE.md)** - Common patterns

---

## Known Gotchas

### LVGL 9 XML Attribute Names

**No `flag_` prefix:**
```xml
<!-- ✓ CORRECT -->
<lv_button hidden="true" clickable="false"/>

<!-- ✗ WRONG -->
<lv_button flag_hidden="true" flag_clickable="false"/>
```

**Use `style_image_*`, not `style_img_*`:**
```xml
<!-- ✓ CORRECT -->
<lv_image style_image_recolor="#primary_color" style_image_recolor_opa="255"/>

<!-- ✗ WRONG -->
<lv_image style_img_recolor="#primary_color" style_img_recolor_opa="255"/>
```

**Use `scale_x`/`scale_y`, not `zoom`:**
```xml
<!-- ✓ CORRECT (256 = 100%) -->
<lv_image scale_x="128" scale_y="128"/>

<!-- ✗ WRONG -->
<lv_image zoom="128"/>
```

### Subject Type Must Match API

Image recoloring requires color subjects:
```cpp
// ✓ CORRECT
lv_subject_init_color(&subject, lv_color_hex(0xFFD700));
lv_obj_set_style_img_recolor(widget, color, LV_PART_MAIN);

// ✗ WRONG
lv_subject_init_string(&subject, buffer, NULL, size, "0xFFD700");
```

---

**For complete development history, see STATUS.md**
