# Moonraker API Wiring Plan

**Last Updated:** 2025-11-25
**Current Focus:** Wiring UI Controls to Moonraker API

---

## Overview

This document tracks the work to connect all UI controls to the real Moonraker API. The mock client (`MoonrakerClientMock`) has been enhanced to simulate realistic printer behavior, enabling development without hardware.

---

## ✅ COMPLETED PHASES

### Phase 1: Temperature Controls & Mock Infrastructure

| Task | Description | Status |
|------|-------------|--------|
| 1.1 | Wire nozzle temperature controls to MoonrakerAPI | ✅ Complete |
| 1.2 | Wire bed temperature controls to MoonrakerAPI | ✅ Complete |
| 1.3a | Add temperature simulation to mock (heating curves) | ✅ Complete |
| 1.3b | Refactor temp panel to proper C++ class with RAII | ✅ Complete |
| 1.4 | Move mock data generation from main.cpp to MoonrakerClientMock | ✅ Complete |
| 1.4b | Add unit tests to verify mock behaves like real Moonraker | ✅ Complete |
| 1.4c | Code review tests, fix thread safety issues | ✅ Complete |
| 1.4d | Complete mock implementation (267→357 assertions) | ✅ Complete |
| 1.6a | Add motion simulation (G28 homing, G0/G1 movement) | ✅ Complete |
| 1.6b | Add print job simulation (start/pause/resume/cancel) | ✅ Complete |
| 1.6c | Fix mock test failures (dangling pointers, JSON format) | ✅ Complete |

**Mock Client Features Now Implemented:**
- Temperature simulation with realistic heating/cooling curves
- G-code parsing for temperature commands (M104/M109/M140/M190, SET_HEATER_TEMPERATURE)
- G-code parsing for motion (G28, G0/G1, G90/G91)
- G-code parsing for print jobs (SDCARD_PRINT_FILE, PAUSE, RESUME, CANCEL_PRINT, M112)
- Position tracking with absolute/relative modes
- Homing state tracking (homed_axes)
- Print state machine (standby→printing→paused→complete/cancelled)
- Progress auto-increment during printing
- Warning stubs for unimplemented G-codes (M106/M107, BED_MESH_*, etc.)

**Test Coverage:**
- 357 assertions across 23 test cases
- Tests validate mock JSON matches real Moonraker API at 192.168.1.67
- Thread-safe notification capture with mutex protection

---

## 🔄 REMAINING PHASES

### Phase 1.5: Temperature Preset Flow Fix

**Priority:** High
**Complexity:** Medium

**Current Issue:**
Temperature preset buttons directly send G-code on click. Should use "pending selection" pattern where user clicks preset, value highlights, then "Confirm" button sends to Moonraker.

**Tasks:**
- [ ] Add `pending_target` state to temp panel
- [ ] Preset buttons set `pending_target`, update UI highlight
- [ ] "Confirm" button sends SET_HEATER_TEMPERATURE with pending value
- [ ] "Cancel" button clears pending state
- [ ] Keyboard input also uses pending pattern

**Files to Modify:**
- `src/ui_temp_control_panel.cpp`
- `ui_xml/nozzle_temp_panel.xml` (if needed)
- `ui_xml/bed_temp_panel.xml` (if needed)

---

### Phase 2: Motion Controls

**Priority:** High
**Complexity:** Medium

#### 2.1: Wire Homing Buttons

**Tasks:**
- [ ] "Home All" button sends `G28` via `gcode_script()`
- [ ] "Home X" sends `G28 X`
- [ ] "Home Y" sends `G28 Y`
- [ ] "Home Z" sends `G28 Z`
- [ ] Verify homed_axes state updates in UI
- [ ] Handle errors (e.g., already homing, endstop not triggered)

**Files:**
- `src/ui_panel_motion.cpp`
- `ui_xml/motion_panel.xml`

#### 2.2: Wire Jog Controls

**Tasks:**
- [ ] X+/X- buttons send `G0 X{delta}` (or `G91; G0 X{delta}; G90`)
- [ ] Y+/Y- buttons send `G0 Y{delta}`
- [ ] Z+/Z- buttons send `G0 Z{delta}`
- [ ] Step size selector (0.1, 1, 10, 100mm)
- [ ] Speed/feedrate control
- [ ] Disable jog if axis not homed (check homed_axes)
- [ ] Add visual feedback during moves

**Files:**
- `src/ui_panel_motion.cpp`
- `ui_xml/motion_panel.xml`

---

### Phase 3: Print Job Controls

**Priority:** High
**Complexity:** Medium

#### 3.1: Verify Print Start Wiring

**Tasks:**
- [ ] Verify file selection sends `SDCARD_PRINT_FILE FILENAME={path}`
- [ ] Confirm print_stats state changes to "printing"
- [ ] Progress updates from virtual_sdcard
- [ ] Thumbnail loading during print selection

**Files:**
- `src/ui_panel_print_select.cpp`

#### 3.2: Wire Pause/Resume/Cancel

**Tasks:**
- [ ] "Pause" button sends `PAUSE` macro
- [ ] "Resume" button sends `RESUME` macro
- [ ] "Cancel" button sends `CANCEL_PRINT` with confirmation dialog
- [ ] Emergency stop (M112) with prominent warning
- [ ] Update button states based on print_stats.state
- [ ] Show estimated time remaining

**Files:**
- `src/ui_panel_print_progress.cpp` (or wherever print controls live)
- Possibly new `ui_panel_print_active.xml`

---

### Phase 4: Extrusion Controls

**Priority:** Medium
**Complexity:** Medium

#### 4.1: Wire Extrude/Retract Buttons

**Tasks:**
- [ ] **Safety check**: Verify hotend temp ≥ min_extrude_temp before allowing
- [ ] "Extrude" sends `G1 E{length} F{speed}`
- [ ] "Retract" sends `G1 E-{length} F{speed}`
- [ ] Length selector (1, 5, 10, 50mm)
- [ ] Speed selector (slow/medium/fast)
- [ ] Show warning if temp too low
- [ ] Relative mode handling (G91 before, G90 after)

**Files:**
- `src/ui_panel_controls_extrusion.cpp`
- `ui_xml/extrusion_panel.xml`

#### 4.2: Wire Filament Panel Temperature Commands

**Tasks:**
- [ ] Load filament: Preheat to material temp, wait, then extrude
- [ ] Unload filament: Heat, retract, cool
- [ ] Purge: Extrude set amount at set temp
- [ ] Material presets (PLA: 210°C, PETG: 240°C, ABS: 250°C, etc.)

**Files:**
- `src/ui_panel_filament.cpp`
- `ui_xml/filament_panel.xml`

---

### Phase 5: Testing & Cleanup

**Priority:** Medium
**Complexity:** Low

#### 5.1: Fix Disabled Robustness Tests

**Tasks:**
- [ ] Review disabled tests in `test_moonraker_client_robustness.cpp`
- [ ] Fix or remove tests that are no longer relevant
- [ ] Ensure all tests pass with both mock and real connections

#### 5.2: Remove Debug Artifacts

**Tasks:**
- [ ] Remove red outline from UI components (debug visual)
- [ ] Clean up any debug logging left enabled
- [ ] Review and remove commented-out code
- [ ] Final code review pass

---

## Mock Client Coverage Matrix

| Feature | G-code/Method | Mock Support | Tests |
|---------|---------------|--------------|-------|
| **Temperature** | | | |
| Set nozzle temp | M104/M109 | ✅ Simulated | ✅ |
| Set bed temp | M140/M190 | ✅ Simulated | ✅ |
| SET_HEATER_TEMPERATURE | G-code | ✅ Simulated | ✅ |
| **Motion** | | | |
| Home all | G28 | ✅ Simulated | ✅ |
| Home single axis | G28 X/Y/Z | ✅ Simulated | ✅ |
| Absolute move | G0/G1 | ✅ Simulated | ✅ |
| Relative mode | G91 | ✅ Simulated | ✅ |
| Absolute mode | G90 | ✅ Simulated | ✅ |
| **Print Jobs** | | | |
| Start print | SDCARD_PRINT_FILE | ✅ Simulated | ✅ |
| Pause | PAUSE | ✅ Simulated | ✅ |
| Resume | RESUME | ✅ Simulated | ✅ |
| Cancel | CANCEL_PRINT | ✅ Simulated | ✅ |
| Emergency stop | M112 | ✅ Simulated | ✅ |
| Progress | auto-increment | ✅ Simulated | ✅ |
| **Not Implemented (Stubs)** | | | |
| Fan control | M106/M107 | ⚠️ Warning stub | - |
| Fan speed | SET_FAN_SPEED | ⚠️ Warning stub | - |
| Extrusion | G1 E{n} | ⚠️ Warning stub | - |
| Bed mesh | BED_MESH_* | ⚠️ Warning stub | - |
| Calibration | QUAD_GANTRY_LEVEL | ⚠️ Warning stub | - |
| Input shaping | SET_INPUT_SHAPER | ⚠️ Warning stub | - |
| Pressure advance | SET_PRESSURE_ADVANCE | ⚠️ Warning stub | - |
| LED control | SET_LED | ⚠️ Warning stub | - |

---

## Architecture Notes

### Temperature Panel (ui_temp_control_panel)

The temperature panel has been refactored to a proper RAII C++ class:

```cpp
class UITempControlPanel {
public:
    UITempControlPanel(lv_obj_t* parent, const UIHeaterConfig& config);
    ~UITempControlPanel();

    void set_current_temp(double temp);
    void set_target_temp(double target);
    void set_graph_data(const std::vector<double>& temps);

private:
    lv_obj_t* panel_;
    lv_obj_t* temp_label_;
    lv_obj_t* target_label_;
    lv_obj_t* graph_;
    // ... observers for reactive updates
};
```

### PrinterState Callbacks

`PrinterState` now supports callbacks for non-blocking UI updates:

```cpp
printer_state.set_state_change_callback([](const std::string& key) {
    // Called when any state changes (e.g., "extruder.temperature")
    // Safe to call LVGL functions if on main thread
});
```

### Mock Client Usage

```cpp
// In main.cpp for development:
auto client = std::make_shared<MoonrakerClientMock>(
    MoonrakerClientMock::PrinterType::VORON_24);

// Connect triggers initial state dispatch
client->connect("ws://mock/websocket",
    []() { /* on_connected */ },
    []() { /* on_disconnected */ });

// Simulation runs in background thread
// Temperature changes, print progress updates automatically

// G-code commands work:
client->gcode_script("G28");           // Homes axes
client->gcode_script("M104 S200");     // Sets nozzle temp
client->gcode_script("SDCARD_PRINT_FILE FILENAME=test.gcode");
```

---

## Testing Strategy

### Unit Tests

Run mock behavior tests:
```bash
./build/bin/run_tests "[mock]"
```

All 357 assertions should pass.

### Integration Testing

1. Run UI with mock client (`--mock` flag)
2. Click temperature preset → verify temp changes over time
3. Click "Home All" → verify homed_axes updates
4. Start print → verify progress increments
5. Pause/Resume/Cancel → verify state transitions

### Real Hardware Testing

1. Connect to real printer at 192.168.1.67 (or configured IP)
2. All operations should work identically to mock
3. Verify no timing-related issues

---

## Next Steps for Developer

1. **Start with Phase 1.5** (preset button flow) - small, well-defined scope
2. **Then Phase 2.1** (homing buttons) - uses existing mock infrastructure
3. **Build up to jog controls** - more complex but follows same patterns
4. **Print controls** likely already partially wired - verify and complete
5. **Extrusion controls** need temperature safety checks

The mock client is comprehensive - use it for development. Real hardware testing at the end of each phase.
