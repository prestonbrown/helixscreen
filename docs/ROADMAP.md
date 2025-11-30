# LVGL 9 XML UI Prototype - Development Roadmap

**Last Updated:** 2025-11-30

---

## 🎯 Current Priorities

1. **Phase 8:** USB File Upload (detect USB drives, import G-code files)
2. **Settings Panel:** Complete configuration UI
3. **Phase 12:** Production readiness

**Completed Phases:** 1, 2, 3, 4, 5, 11, 13, 14
**Mostly Complete:** 6, 7, 8, 15
**In Progress:** 9, 10

---

## ✅ Phase 1: Foundation (COMPLETED)

- [x] LVGL 9.3 setup with XML support
- [x] Navigation bar with proper flex layout
- [x] Home panel with declarative XML
- [x] Reactive data binding (Subject-Observer pattern)
- [x] Theme system with global constants
- [x] FontAwesome icon integration with auto-generation
- [x] Screenshot utility supporting multiple binaries
- [x] Comprehensive documentation
- [x] C++ wrappers for XML panels with clean API
- [x] LVGL logging integration

## ✅ Phase 2: Navigation & Blank Panels (COMPLETED)

**Priority: High** - Enable panel switching with reactive navbar highlighting

- [x] **Navigation State Management**
  - Active panel tracking via integer subject
  - Navbar button click handlers (C++ event handlers)
  - Panel visibility toggling (show/hide based on active state)
  - Reactive navbar icon highlighting (red #ff4444 for active, white #ffffff for inactive)

- [x] **Blank Placeholder Panels**
  - Home panel (with printer status content)
  - Controls panel (blank placeholder)
  - Filament panel (blank placeholder)
  - Settings panel (blank placeholder)
  - Advanced panel (blank placeholder)

- [x] **Navigation Implementation**
  - Updated navigation_bar.xml with FontAwesome icons
  - Created C++ wrapper (ui_nav.cpp) with Subject-Observer pattern
  - Panel switching via clickable navigation icons
  - Icon color updates based on active panel

**Key Learnings:**
- Never call `SDL_PollEvent()` manually - violates LVGL display driver abstraction
- Must create mouse input device with `lv_sdl_mouse_create()` for clicks
- Event handlers in C++ with `lv_obj_add_event_cb()` for `LV_EVENT_CLICKED`
- Labels must have `LV_OBJ_FLAG_EVENT_BUBBLE` and not be clickable for clicks to reach buttons

## 📋 Phase 3: Print Select Panel Core (COMPLETED)

**Status: COMPLETE** - Card view with detail overlay working

- [x] **Print Select Panel** - File browser with dynamic cards
  - ✅ Dynamic XML card component system (print_file_card.xml)
  - ✅ 4-column grid layout (204×280px cards optimized for 1024×800)
  - ✅ Thumbnail display with centering (180×180px images)
  - ✅ Filename truncation with ellipsis
  - ✅ Mock print file data (30 test files)
  - ✅ Utility functions for time/filament formatting
  - ✅ Metadata labels with icons (clock + time, leaf + weight)
  - ✅ Clickable cards opening detail overlay
  - ✅ Full-screen detail view with large thumbnail, options, action buttons
  - ✅ Filament type dropdown (11 filament types)
  - ✅ Automatic bed leveling checkbox
  - ✅ Delete + Print action buttons

**Completed:** 2025-10-12 Evening

---

## ✅ Phase 4: Print Select Panel Polish (COMPLETED)

**Priority: High** - List view, sorting, empty state, confirmations

- [x] **View Modes**
  - [x] View toggle button (icon-only: fa-list ↔ fa-th-large)
  - [x] List view with sortable table layout
  - [x] Instant toggle between card and list views (show/hide containers)

- [x] **Sortable List View**
  - [x] Column headers simplified to 4 columns: Filename, Size, Modified, Print Time
  - [x] Click header to sort ascending/descending
  - [x] Visual sort indicators (▲/▼ arrows in header labels)
  - [x] Default sort: Filename, ascending
  - [x] Toggle direction on same column, switch to ascending on new column
  - [x] Reactive re-rendering with std::sort() and update functions

- [x] **Empty State**
  - [x] Display when no files available
  - [x] "No files available for printing" message (montserrat_20)
  - [x] "Upload gcode files to get started" secondary text (montserrat_16)
  - [x] Centered in content area (400×200px container)
  - [x] Shared between card and list views

- [x] **Confirmation Dialogs**
  - [x] Reusable confirmation dialog XML component (confirmation_dialog.xml)
  - [x] Semi-transparent backdrop overlay (#000000 opacity 180)
  - [x] 400×200px centered dialog card
  - [x] API properties for dynamic title and message
  - [x] Cancel + Confirm buttons (160px each)
  - [x] Click backdrop or Cancel to dismiss
  - [x] Confirm triggers callback function

- [x] **Icon System**
  - [x] Added ICON_LIST and ICON_TH_LARGE to ui_fonts.h
  - [x] Generated #icon_list and #icon_th_large in globals.xml
  - [x] Updated generate-icon-consts.py script

- [x] **Utility Functions**
  - [x] format_file_size() - Converts bytes to "1.2 MB" format
  - [x] format_modified_date() - Formats timestamps to "Jan 15 14:30"

**Completed:** 2025-10-12 Evening

**Key Decisions:**
- Simplified list view from 7 columns to 4 (removed Thumbnail, Filament, Slicer for clarity)
- View toggle shows opposite mode icon (intuitive: shows where you'll go, not where you are)
- Default view: Card mode (visual preference)
- Default sort: Filename ascending (alphabetical)
- Per-session view preference (remembered until restart)

---

## ✅ Phase 5: Controls Panel (COMPLETE)

**Priority: High** - Manual printer control with Bambu X1C-style sub-screens

**Status:** All sub-screens complete: Motion, Nozzle Temp, Bed Temp, Extrusion, Fan Control.

### Phase 1: Launcher Panel ✅ COMPLETE (2025-10-12 Night)

- [x] **6-Card Launcher Menu**
  - [x] 2×3 grid layout (400×200px cards)
  - [x] Proper flex row_wrap for card wrapping
  - [x] Vertical scrolling for overflow
  - [x] Card icons + titles + subtitles
  - [x] Click handlers for all 6 cards
  - [x] C++ integration (ui_panel_controls.cpp/h)
  - [x] Fan Control card (now fully functional)

- [x] **Design Specification**
  - [x] 70-page comprehensive UI design document (controls-panel-v1.md)
  - [x] Complete sub-screen mockups and specifications
  - [x] Component file structure defined
  - [x] API patterns documented

- [x] **Icon System Updates**
  - [x] Added 10 new icon constants to ui_fonts.h
  - [x] Regenerated globals.xml with 27 total icons
  - [x] Used existing fa_icons_64 glyphs as placeholders

**Completed:** 2025-10-12 Night

### Phase 2: Reusable Components ✅ COMPLETE (2025-10-12 Late Night)

- [x] **Numeric Keypad Modal Component**
  - [x] XML component (numeric_keypad_modal.xml) with backdrop + modal
  - [x] 700px width, right-docked design
  - [x] Header bar: back button + dynamic title + OK button
  - [x] Input display with large font (montserrat_48) + unit label
  - [x] 3×4 button grid: [7][8][9], [4][5][6], [1][2][3], [EMPTY][0][BACKSPACE]
  - [x] C++ wrapper (ui_component_keypad.cpp/h) with callback API
  - [x] Event handlers for all buttons (digits, backspace, OK, cancel)
  - [x] Input validation (min/max clamping)
  - [x] Backdrop click to dismiss
  - [x] Single reusable instance pattern
  - [x] FontAwesome backspace icon (U+F55A) in fa_icons_32
  - [x] Font regeneration workflow with lv_font_conv

**Files Created:**
- `ui_xml/numeric_keypad_modal.xml` - Full modal component
- `src/ui_component_keypad.cpp` - Implementation with state management
- `include/ui_component_keypad.h` - Public API with config struct
- `package.json` - Updated font ranges for backspace icon

**Files Modified:**
- `ui_xml/globals.xml` - Added keypad dimension and color constants
- `assets/fonts/fa_icons_32.c` - Regenerated with backspace icon
- `src/main.cpp` - Keypad initialization and test demo

**Completed:** 2025-10-12 Late Night

---

### Phase 3: Motion Sub-Screen ✅ COMPLETE (2025-11-15)

- [x] **Movement Controls**
  - [x] Button grid jog pad (3×3 grid with center home)
  - [x] Z-axis buttons (↑10, ↑1, ↓1, ↓10)
  - [x] Distance selector (0.1mm, 1mm, 10mm, 100mm radio buttons)
  - [x] Real-time position display (X/Y/Z with reactive subjects)
  - [x] Home buttons (All, X, Y, Z)
  - [x] Back button navigation
  - [x] Wired to Moonraker gcode_script() API

### Phase 4: Temperature Sub-Screens ✅ COMPLETE (2025-11-15)

- [x] **Nozzle Temperature Screen**
  - [x] Current/Target temperature display with live updates
  - [x] Preset buttons (Off, PLA 210°, PETG 240°, ABS 250°)
  - [x] Custom button opens numeric keypad
  - [x] Temperature graph with real-time data
  - [x] Confirm button with green styling
  - [x] Back button navigation

- [x] **Heatbed Temperature Screen**
  - [x] Current/Target temperature display with live updates
  - [x] Preset buttons (Off, PLA 60°, PETG 80°, ABS 100°)
  - [x] Custom button opens numeric keypad
  - [x] Temperature graph with real-time data
  - [x] Confirm button with green styling
  - [x] Back button navigation

### Phase 5: Extrusion Sub-Screen ✅ COMPLETE (2025-11-15)

- [x] **Extrude/Retract Controls**
  - [x] Amount selector (5mm, 10mm, 25mm, 50mm radio buttons)
  - [x] Extrude button (green, icon + text)
  - [x] Retract button (orange, icon + text)
  - [x] Temperature status card (nozzle temp with checkmark/warning)
  - [x] Safety warning when nozzle < 170°C
  - [x] Back button navigation

### Phase 6: Advanced Features (MOSTLY COMPLETE)

- [x] **Fan Control Sub-Screen** ✅ COMPLETE (2025-11-30)
  - [x] Part cooling fan slider (0-100%) with live value updates
  - [x] Preset buttons (Off, 50%, 75%, 100%)
  - [x] Current speed display with reactive subjects
  - [x] Real-time observer from PrinterState
  - [x] Moonraker API integration (`set_fan_speed`)

- [ ] **Motors Disable**
  - [ ] Confirmation dialog
  - [ ] Send M84 command
  - [ ] Visual feedback

**Design Reference:** Based on Bambu Lab X1C controls system with sub-screens

---

## 📱 Phase 6: Additional Panel Content (FUTURE)

**Priority: Medium** - Build out remaining panel functionality

- [ ] **Storage Source Tabs** (Print Select Panel)
  - Internal/SD card tab navigation
  - Tab bar in panel header
  - Reactive tab switching
  - File list updates per storage source

- [x] **Print Status Panel** ✅ COMPLETE (2025-11-18, enhanced 2025-11-29)
  - ✅ Progress bar with real Moonraker data
  - ✅ Print time/remaining time with reactive updates
  - ✅ Bed/nozzle temperatures with live updates from printer
  - ✅ Pause/Resume/Cancel buttons (wired to Moonraker API)
  - ✅ Wired to Print button on file detail view
  - ✅ Responsive flex layout (fixed previous overlap issues)
  - ✅ **Exclude Object support** (2025-11-29):
    - Long-press object in G-code viewer to exclude
    - Confirmation modal before exclusion
    - 5-second undo window with toast notification
    - Bidirectional sync with Klipper EXCLUDE_OBJECT
    - Visual indication (red/strikethrough) in renderer

- [ ] **Filament Panel** - Filament management
  - Load/unload controls
  - Filament profiles
  - Color/material selection

- [ ] **Settings Panel** - Configuration
  - Network settings
  - Display settings (brightness, theme)
  - Printer settings
  - System info

- [~] **Advanced/Tools Panel** - Advanced features (PARTIAL)
  - [x] Bed mesh visualization ✅ (3D TinyGL renderer, accessible from Settings)
  - [ ] Console/logs
  - [ ] File manager
  - [ ] System controls

## 🎨 Phase 6: Enhanced UI Components (FUTURE)

**Priority: Medium** - Richer interactions and reusable widgets

- [ ] **Integer Subjects for Numeric Displays**
  - Progress bars with `bind_value`
  - Sliders with bi-directional binding
  - Arc/gauge widgets

- [ ] **Custom Widgets as XML Components**
  - Temperature display widget
  - Print progress widget
  - Status badge component

- [x] **Additional Modal Patterns** ✅ PARTIALLY COMPLETE
  - [x] ModalBase RAII class for safe lifecycle management (2025-11-17)
  - [x] Toast/snackbar notifications - floating design, top-right position (2025-11-27)
  - [x] Toast with action button (undo pattern) - reactive visibility binding (2025-11-29)
  - [ ] Error message dialogs
  - [ ] Loading indicators

- [ ] **Lists and Scrolling**
  - Virtual scrolling for large lists (100+ items)
  - Settings menu items
  - Log viewer with auto-scroll

## 🔗 Phase 7: Panel Transitions & Polish (PARTIALLY COMPLETE)

**Priority: Low** - Visual refinement and animations

- [x] **Panel Transitions** ✅ PARTIALLY COMPLETE (2025-11-18)
  - [x] Slide animations for overlay panels
  - [ ] Fade in/out animations
  - [x] Proper cleanup when switching panels (via PanelBase)

- [ ] **Button Feedback**
  - Navbar button press effects
  - Hover states (if applicable)
  - Ripple effects

- [ ] **View Transitions**
  - Smooth card ↔ list view transitions
  - Detail overlay fade-in/fade-out
  - Dialog animations

## 🔌 Phase 8: Backend Integration (IN PROGRESS)

**Priority: High** - Connect to Klipper/Moonraker

- [x] **WebSocket Client for Moonraker** ✅ COMPLETE (2025-10-26)
  - [x] Connection management (libhv WebSocketClient)
  - [x] JSON-RPC protocol with auto-incrementing request IDs
  - [x] Auto-discovery chain (objects.list → server.info → printer.info → subscribe)
  - [x] Object categorization (heaters, sensors, fans, LEDs)
  - [x] Persistent notification callbacks
  - [x] Reconnection logic ✅ COMPLETE (2025-11-01)

- [x] **Configuration System** ✅ COMPLETE (2025-10-26)
  - [x] Config singleton with JSON storage
  - [x] Auto-generates helixconfig.json with defaults
  - [x] Multi-printer structure support
  - [x] JSON pointer-based get/set API

- [x] **Moonraker API Phase 1: Core Infrastructure** ✅ COMPLETE (2025-11-01)
  - [x] Connection state machine (5 states with automatic transitions)
  - [x] Request timeout management with PendingRequest tracking
  - [x] Comprehensive error handling (MoonrakerError structure)
  - [x] Configuration-driven timeouts (all values in helixconfig.json)
  - [x] MoonrakerAPI facade layer for high-level operations
  - [x] Automatic cleanup on disconnect with error callbacks

- [~] **Printer State Management** (PARTIAL)
  - [x] PrinterState reactive subjects created
  - [x] Notification callback wired to update_from_notification()
  - [x] Connection state tracking (enhanced with 5-state machine)
  - [ ] Parse and bind all printer objects to subjects
  - [ ] Handle state changes and errors

- [ ] **Moonraker API Phase 2-5** (PLANNED)
  - [ ] Phase 2: File management (list, metadata, delete, upload)
  - [ ] Phase 3: Job control (start, pause, resume, cancel)
  - [ ] Phase 4: Multi-extruder support (dynamic heater discovery)
  - [ ] Phase 5: System administration (updates, power, monitoring)

- [x] **File Operations** ✅ PARTIAL (2025-11-02)
  - [x] List print files (server.files.list) - fully implemented
  - [x] Get file metadata (print time, filament, thumbnails) - fully implemented
  - [x] Delete files - already implemented
  - [x] Start prints - already implemented
  - [x] Thumbnail URL construction - implemented (HTTP download deferred)
  - [ ] Upload files
  - [ ] Thumbnail HTTP downloads (needs libhv HttpClient integration)

- [x] **Real-time Updates** ✅ COMPLETE (2025-11-18)
  - [x] Temperature monitoring - Home panel shows live nozzle/bed temps
  - [x] Print progress - Print status panel shows live progress
  - [x] LED control - Wired to Moonraker with reactive bindings
  - [x] Connection state - Disconnected overlay on home panel

## 🎭 Phase 9: Theming & Accessibility (PARTIALLY COMPLETE)

**Priority: Low** - Visual refinement

- [x] **Theme Variants**
  - [x] Light/Dark mode toggle (runtime theme switching)
  - [x] Theme color variant system (XML-based)
  - [ ] High contrast mode
  - [ ] Custom color schemes

- [x] **Responsive Layouts** ✅ COMPLETE
  - [x] Support multiple resolutions (breakpoint system)
  - [x] LVGL theme breakpoint patch (480/800 breakpoints)
  - [x] Comprehensive test suite (50 tests)
  - [x] DPI scaling for different hardware profiles
  - [ ] Orientation changes

- [ ] **Animations**
  - Button press effects
  - Panel transitions
  - Loading animations
  - Success/error feedback

- [ ] **Accessibility**
  - Larger touch targets
  - High contrast options
  - Status announcements

## 🧪 Phase 10: Testing & Optimization (ONGOING)

**Priority: Ongoing** - Quality assurance

- [ ] **Memory Profiling**
  - Check for leaks
  - Optimize subject usage
  - Profile panel switching

- [ ] **Performance Testing**
  - Frame rate monitoring
  - UI responsiveness
  - Touch latency

- [x] **Build Quality** ✅ COMPLETE
  - [x] Zero LVGL XML warnings
  - [x] Zero spdlog/fmt deprecation warnings
  - [x] Independent spdlog submodule (fmt 11.2.0)
  - [x] Clean build output
  - [x] Doxygen API documentation generation

- [x] **Responsive Theme Testing** ✅ COMPLETE
  - [x] 50-test comprehensive suite
  - [x] Breakpoint classification tests
  - [x] DPI scaling tests (4 hardware profiles)
  - [x] Theme toggle verification

- [x] **Moonraker Mock Testing** ✅ PARTIAL (2025-11-29)
  - [x] Unit tests for mock motion commands (G28, G0/G1)
  - [x] Mock Moonraker client infrastructure
  - [ ] Full API coverage for all mocked endpoints

- [ ] **Cross-platform Testing**
  - Raspberry Pi target
  - BTT Pad target
  - Different screen sizes

- [ ] **Edge Case Handling**
  - Connection loss
  - Print errors
  - File system errors

## ✅ Phase 11: First-Run Configuration Wizard (COMPLETED)

**Priority: N/A** - Completed 2025-11-30

**Status: COMPLETE** - All 7 wizard steps fully implemented with class-based architecture

**Summary:**
- ✅ **Step 1: WiFi Setup** - Network scanning, password entry, async callbacks
- ✅ **Step 2: Moonraker Connection** - IP/port entry, RFC 1035 validation, connection testing
- ✅ **Step 3: Printer Identify** - Type selection, auto-detection with confidence scoring
- ✅ **Step 4: Heater Selection** - Bed/hotend heater dropdowns from discovery
- ✅ **Step 5: Fan Selection** - Hotend fan, part cooling fan dropdowns
- ✅ **Step 6: LED Selection** - Optional LED strip selection
- ✅ **Step 7: Summary** - Read-only review of all selections, finish button

**Key Infrastructure:**
- Reusable `wizard_header_bar.xml` component with contextual subtitles
- Class-based architecture (all steps migrated to WizardXxxStep classes)
- `ui_wizard_helpers.h` for dropdown population and config save/restore
- Responsive constants for small/medium/large displays

### User Story
New users need a guided setup wizard to:
1. Connect to their Moonraker instance (manual IP entry)
2. Map auto-discovered printer components to UI defaults
3. Save these mappings for consistent behavior

### Requirements (All Complete)

- [x] **First-Run Detection** ✅
  - [x] Detect missing/incomplete configuration on startup
  - [x] Show wizard automatically on first run
  - [x] Allow re-run from settings panel ("Reset Configuration")
  - [x] Skip wizard if config is complete

- [x] **Moonraker Connection Screen** ✅
  - [x] Manual IP address entry field with RFC 1035 validation
  - [x] Port configuration field (default 7125)
  - [x] "Test Connection" button with spinner/status feedback
  - [x] Connection status display (success/error messages)
  - [x] "Next" button (disabled until successful connection via `connection_test_passed` subject)
  - [x] Save connection settings to helixconfig.json
  - [ ] Future enhancement: mDNS/Bonjour auto-discovery scan

- [x] **Hardware Mapping Wizard** ✅
  - [x] Auto-detect available components via printer.objects.list
  - [x] Multi-screen wizard flow (one category per screen):
    1. ✅ **Heated Bed Selection** - Dropdown filtered by "bed" keyword
    2. ✅ **Hotend Selection** - Dropdown filtered by "extruder"/"hotend"
    3. ✅ **Fan Selection** - Hotend fan + part cooling fan dropdowns
    4. ✅ **LED Selection** - Optional LED strip with "None" option
  - [x] Back/Next navigation buttons on each screen
  - [x] Summary screen showing all selections before saving
  - [x] Save mappings to config file under printer-specific section

- [x] **Configuration Storage** ✅
  - [x] helixconfig.json schema with hardware mappings
  - [x] Config save/restore via `ui_wizard_helpers.h`
  - [x] `wizard_config_paths.h` defines all JSON pointer paths

- [x] **UI Components** ✅
  - [x] Wizard container with progress indicator, back/next/finish buttons
  - [x] Textarea input fields with validation
  - [x] Dropdown/roller widgets for component selection
  - [x] Success/error status messages via `wizard_header_bar.xml` subtitle

### Design Notes

- **Auto-default behavior:** If only one obvious component exists (e.g., single heater_bed, single extruder), pre-select it and skip that wizard screen
- **Validation:** Ensure selections are valid before allowing user to proceed
- **User control:** Always allow manual override of auto-detected defaults
- **mDNS Discovery:** Future enhancement - Moonraker supports zeroconf component but not universally enabled. Manual IP entry is primary method.

### Dependencies

- Requires Phase 8 (Backend Integration) complete
- MoonrakerClient auto-discovery functional
- Config system with JSON pointer API

### Testing

- [x] Test with no config file (first run)
- [x] Test with partial config (migration)
- [x] Test with complete config (skip wizard)
- [x] Test connection failures (invalid IP, port, timeout)
- [x] Test with various printer configurations

---

## 🚀 Phase 12: Production Readiness (FUTURE)

**Priority: Future** - Deployment prep

- [ ] **Error Handling**
  - Graceful degradation
  - Error recovery
  - User notifications

- [ ] **Logging System**
  - Structured logging
  - Log levels
  - Log rotation

- [ ] **Build System Improvements**
  - Cross-compilation setup
  - Packaging for targets
  - Install scripts

---

## ✅ Phase 13: G-code Pre-Print Modification (COMPLETED)

**Priority: N/A** - Completed 2025-11-29

**Status: COMPLETE** - Full implementation with 10-stage architecture

### Overview
Users can enable/disable optional print operations (bed leveling, QGL, Z-tilt, nozzle clean) before starting a print. Supports both:
1. **Pre-print sequencing**: Run operations before printing starts
2. **G-code modification**: Comment out embedded operations in the file itself

### Components ✅

- [x] **GcodeOpsDetector** (`gcode_ops_detector.cpp`)
  - [x] Detect bed leveling (G29, BED_MESH_CALIBRATE, KAMP_CALIBRATE)
  - [x] Detect QGL (QUAD_GANTRY_LEVEL)
  - [x] Detect Z-tilt (Z_TILT_ADJUST)
  - [x] Detect nozzle clean (CLEAN_NOZZLE, NOZZLE_CLEAN)
  - [x] Return line numbers for each detected operation

- [x] **GcodeFileModifier** (`gcode_file_modifier.cpp`)
  - [x] Comment out specific lines to disable operations
  - [x] Generate modified G-code content
  - [x] Upload modified file to .helix_temp directory

- [x] **CommandSequencer** (`command_sequencer.cpp`)
  - [x] Coordinate multi-step print preparation
  - [x] Handle async operations (home → QGL → Z-tilt → mesh → clean → print)
  - [x] Progress callbacks for UI feedback

- [x] **PrinterCapabilities** (`printer_capabilities.cpp`)
  - [x] Track what features printer supports (from Klipper config)
  - [x] User-configurable overrides (force enable/disable)
  - [x] Reactive subjects for UI binding

### UI Integration ✅
- [x] Print file detail checkboxes (Bambu-style):
  - [x] "Automatic Bed Leveling" (visibility bound to printer capability)
  - [x] "Quad Gantry Level" (visibility bound to printer capability)
  - [x] "Z-Tilt Adjust" (visibility bound to printer capability)
  - [x] "Clean Nozzle" (visibility bound to printer capability)
- [x] "Preparing" overlay with spinner and progress bar
- [x] Conflict detection: warn when disabling embedded operations

### Implementation Stages (All Complete)
| Stage | Feature | Commit |
|-------|---------|--------|
| 1 | GcodeOpsDetector with line tracking | ✅ |
| 2 | GcodeFileModifier for commenting out | `4c4f6ef` |
| 3 | PrinterCapabilities with reactive subjects | ✅ |
| 4 | CommandSequencer for pre-print ops | `2c6a22d` |
| 5 | UI checkboxes bound to capabilities | `8c76c4f` |
| 6 | Pre-print sequence execution | `2c6a22d` |
| 7 | Conflict detection (embedded ops) | `e6a7628` |
| 8 | User capability overrides | `369aa73` |
| 9 | Concurrent print prevention | `1a25afd` |
| 10 | Memory-safe streaming for large files | `2fc776b` |

### Design Doc
See `docs/PRINT_OPTIONS_IMPLEMENTATION_PLAN.md` for full specification.

---

## ✅ Phase 14: Class-Based Panel Architecture (COMPLETED)

**Priority: N/A** - Completed 2025-11-17, enhanced 2025-11-29

**Summary:** Migrated all panels from function-based to class-based architecture using PanelBase.

- [x] **PanelBase class** - Encapsulates panel lifecycle (init, show, hide, destroy)
- [x] **All 13+ panels migrated** - Home, Controls, Motion, Temps, Print Select, etc.
- [x] **Wizard steps migrated** - All 7 wizard steps use class-based pattern
- [x] **Deprecated wrappers removed** - Clean break from old function-based API
- [x] **ObserverGuard RAII pattern** (2025-11-29) - Safe observer subscription lifecycle
  - Automatic unsubscription on destruction
  - Applied to HomePanel, PrintStatusOverlay, FilamentOverlay, ExtrusionOverlay
  - Fixes crash from LVGL observer immediate-fire pattern

**Benefits:**
- RAII lifecycle management
- Consistent show/hide behavior
- Easier testing and maintenance
- Clear ownership semantics
- Safe observer cleanup (no dangling callbacks)

**Reference:** `docs/archive/PANEL_MIGRATION.md`, `docs/archive/PANEL_REFACTORING_PLAN.md`

---

## 📐 Phase 15: Reactive UI Architecture (MOSTLY COMPLETE)

**Priority: Medium** - Eliminate imperative UI flag manipulation in favor of declarative XML bindings

**Status: MOSTLY COMPLETE** - Core refactoring done, remaining items have complexity > benefit

### Overview
Convert imperative `lv_obj_add_flag()`/`lv_obj_remove_flag()` calls to reactive XML bindings using `<lv_obj-bind_flag_if_eq>` elements. This makes UI state declarative and self-documenting.

### Completed ✅

- [x] **Temperature Panel X-axis Labels** (`922e918`)
  - Subject: `nozzle_graph_points`, `bed_graph_points`
  - Labels hidden until 60+ data points using `<lv_obj-bind_flag_if_lt>`

- [x] **Extrusion Panel Safety Warning** (`2384501`)
  - Subject: `extrusion_safety_warning_visible`
  - Warning shown/hidden via reactive binding

- [x] **Filament Panel Safety Warning** (`2384501`)
  - Subject: `filament_safety_warning_visible`
  - Warning shown/hidden via reactive binding

- [x] **Print Status G-code Viewer Mode** (`2384501`)
  - Subject: `gcode_viewer_mode`
  - Gradient, thumbnail, and viewer visibility controlled reactively

- [x] **Notification History Empty State** (`2384501`)
  - Subject: `notification_has_entries`
  - Content/empty state visibility controlled reactively

- [x] **Controls Panel Clickable Cards** (`8b1881c`)
  - XML attribute: `clickable="true"` on 5 cards
  - Removed 5 `lv_obj_add_flag(LV_OBJ_FLAG_CLICKABLE)` calls

- [x] **Settings Panel Clickable Card** (`8b1881c`)
  - XML attribute: `clickable="true"` on bed_mesh card
  - Removed 1 `lv_obj_add_flag(LV_OBJ_FLAG_CLICKABLE)` call

- [x] **Print Select View Mode Toggle** (`8b1881c`)
  - Subject: `print_select_view_mode` (0=CARD, 1=LIST)
  - Card/list container visibility controlled reactively

### Skipped (Complexity > Benefit)

- Empty state with compound conditions (empty AND view_mode)
- Sort indicators (8 compound conditions for 4 columns × 2 directions)
- Directory card metadata (dynamic per-card creation)

### Key Pattern
```xml
<!-- In XML: Declare visibility rule -->
<lv_obj name="warning_container">
  <lv_obj-bind_flag_if_eq subject="safety_warning_visible" flag="hidden" ref_value="0"/>
</lv_obj>

<!-- In C++: Just update the subject -->
lv_subject_set_int(&safety_warning_subject_, show ? 1 : 0);
```

### Benefits
- UI behavior visible in XML layout files
- C++ code focuses on business logic, not presentation
- Easier to understand and maintain
- Self-documenting state management

---

## Recent Work (2025-11-14 to 2025-11-30)

| Feature | Phase | Date |
|---------|-------|------|
| **KlippyState unit tests** | 10 | 2025-11-30 |
| **Mock simulation: fan control, Z offset, restart** | 10 | 2025-11-30 |
| **Test fixtures for mock initialization** | 10 | 2025-11-30 |
| **G-code viewer sizing & progress bar fixes** | 8 | 2025-11-30 |
| **Fan Control Sub-Screen COMPLETE** | 5 | 2025-11-30 |
| **First-Run Wizard COMPLETE (all 7 steps)** | 11 | 2025-11-30 |
| **Exclude Object feature (touch-to-exclude with undo)** | 6, 8 | 2025-11-29 |
| **Bed mesh deferred render fix (hidden panels)** | 6 | 2025-11-29 |
| Deferred render pattern documentation | - | 2025-11-29 |
| **ObserverGuard RAII pattern for panels** | 14 | 2025-11-29 |
| **Unit tests for Moonraker mock motion commands** | 10 | 2025-11-29 |
| Fix LVGL observer immediate-fire crash | 14 | 2025-11-29 |
| Documentation cleanup (outdated/redundant docs) | - | 2025-11-29 |
| **Reactive UI refactoring (Priorities 4+5)** | 15 | 2025-11-29 |
| **Reactive X-axis time labels for temp graphs** | 15 | 2025-11-29 |
| **G-code memory-safe streaming (Stage 10)** | 13 | 2025-11-29 |
| **Concurrent print prevention (Stage 9)** | 13 | 2025-11-29 |
| IP/hostname validation improvements | 11 | 2025-11-29 |
| Toast redesign (floating, top-right) | 6 | 2025-11-27 |
| Wizard header component extraction | 11 | 2025-11-27 |
| Printer database v2.0 (50+ printers) | 11 | 2025-11-22 |
| Class-based panel refactoring (PanelBase) | 14 | 2025-11-17 |
| Print Status → real Moonraker API | 8 | 2025-11-18 |
| Home Panel → live temps/LEDs | 8 | 2025-11-18 |
| Motion sub-screen complete | 5 | 2025-11-15 |
| Temperature sub-screens complete | 5 | 2025-11-15 |
| Extrusion sub-screen complete | 5 | 2025-11-15 |
| ModalBase RAII pattern | 6 | 2025-11-17 |
| Slide animations for overlays | 7 | 2025-11-18 |
| Clang-format pre-commit hooks | 10 | 2025-11-17 |

---

## Notes

- **Reactive Pattern:** All UI updates should use Subject-Observer pattern
- **XML First:** Prefer XML layout over C++ when possible
- **Clean Separation:** Keep business logic in C++, layout in XML
- **Documentation:** Update guides as patterns emerge
- **UI Patterns:** Document reusable patterns in LVGL9_XML_GUIDE.md (e.g., vertical accent bars, dynamic card components)
- **Responsive Design:** Use constants in globals.xml for easy layout adjustments (4-col vs 3-col grids)
- **Testing:** Test each phase before moving to next
