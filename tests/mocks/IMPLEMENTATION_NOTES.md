# Mock Implementation Notes

## Overview

Comprehensive mocking infrastructure for wizard UI integration testing, enabling tests without real WebSocket connections or LVGL rendering.

## Files Created

### Core Mocks

1. **include/moonraker_client_mock.h** / **src/mock/moonraker_client_mock.cpp**
   - Full mock client inheriting from `MoonrakerClient`
   - Supports `PrinterType` for different printer configs (VORON_24, K1, AD5M, etc.)
   - Print simulation, thermal model, gcode parsing
   - Used by `MoonrakerAPIMock` for integration testing

2. **tests/mocks/lvgl_mock.h** / **.cpp** (312 lines)
   - Minimal LVGL implementation for UI testing
   - Mocks 13 essential LVGL functions (widget creation, textareas, subjects, events, timers)
   - Namespace `LVGLMock` with test control API
   - On-demand widget creation for simplified test setup

3. **tests/mocks/ui_keyboard_mock.cpp** (76 lines)
   - No-op implementation of keyboard functions
   - All functions log calls but perform no actions
   - Prevents linking errors when wizard code uses keyboard

### Documentation

4. **tests/mocks/README.md** (215 lines)
   - Comprehensive usage guide
   - Example test code
   - Design philosophy and limitations
   - Future enhancement ideas

5. **tests/mocks/IMPLEMENTATION_NOTES.md** (this file)
   - Implementation details and decisions

### Example Tests

6. **tests/unit/test_mock_example.cpp** (290 lines)
   - 3 test cases with 44 assertions demonstrating mock usage
   - MoonrakerClientMock: connection tracking, RPC methods, reset
   - LVGLMock: textareas, buttons, subjects, timers, time advancement
   - Combined mock usage: simulated connection test flow

## Build System Integration

### Makefile Changes

Added separate test binaries to avoid symbol conflicts:

```makefile
# Unit tests (use real LVGL)
TEST_BIN := build/bin/helix-tests
TEST_SRCS := $(filter-out test_mock_example.cpp, ...)

# Integration tests (use mocks instead of real LVGL)
TEST_INTEGRATION_BIN := build/bin/run_integration_tests
TEST_INTEGRATION_SRCS := tests/unit/test_mock_example.cpp

# Mock object compilation
MOCK_OBJS := build/obj/tests/mocks/*.o
```

### New Make Targets

- `make test-integration` - Run integration tests with mocks
- `make help` - Updated with new target

### Test Results

**Unit tests (real LVGL):**
- 60 test cases
- 319 assertions
- All passing ✓

**Integration tests (mocks):**
- 3 test cases
- 44 assertions
- All passing ✓

## Key Design Decisions

### 1. Separate Test Binaries

**Problem:** Linking both real LVGL and mock LVGL causes duplicate symbol errors.

**Solution:**
- `helix-tests` - Unit tests with real LVGL (temp_graph, navigation, config)
- `run_integration_tests` - Integration tests with mocks (wizard UI flow)

**Alternative Considered:** Weak symbols or function wrapping - rejected due to complexity.

### 2. On-Demand Widget Creation

**Problem:** Tests would need to manually create widgets before using them.

**Solution:** `lv_obj_find_by_name()` and `set_textarea_value()` create widgets if not found.

**Benefit:** Simpler test setup - `LVGLMock::set_textarea_value("ip_input", "192.168.1.100")` works immediately.

### 3. Opaque Pointer Pattern

**Problem:** `typedef void lv_subject_t` prevents creating instances (`void obj;` is invalid).

**Solution:** Use dummy allocations: `lv_subject_t* s = reinterpret_cast<lv_subject_t*>(new char[1]);`

**Cleanup:** Tests must `delete[]` after use to avoid leaks.

### 4. Minimal Mocking

**Problem:** Full LVGL emulation is too complex.

**Solution:** Only mock functions actually used by wizard connection screen:
- Widget: `lv_xml_create()`, `lv_obj_find_by_name()`, `lv_scr_act()`
- Textarea: `lv_textarea_get_text()`, `lv_textarea_set_text()`
- Subject: `lv_subject_init_string()`, `lv_subject_copy_string()`, `lv_xml_register_subject()`
- Event: `lv_obj_add_event_cb()`, `lv_event_send()`, `lv_xml_register_event_cb()`
- Timer: `lv_timer_create()`, `lv_timer_del()`, `lv_tick_get()`

**Future:** Add more functions as needed for other panel tests.

### 5. Namespace for Test API

**Problem:** Test control functions would pollute global namespace or conflict with real LVGL.

**Solution:** `LVGLMock` namespace for test utilities:
- `LVGLMock::init()` / `reset()`
- `LVGLMock::set_textarea_value()` / `get_textarea_value()`
- `LVGLMock::trigger_button_click()`
- `LVGLMock::advance_time()` / `process_timers()`

**Pattern:** Real LVGL API in C (`extern "C"`), test control API in C++ namespace.

### 6. Comprehensive Logging

**Pattern:** All mocks use spdlog with `[MockXXX]` prefixes:
- `[MockMR]` - MoonrakerClientMock
- `[MockLVGL]` - LVGLMock
- `[MockKeyboard]` - Keyboard mock

**Benefit:** Easy debugging - tests show exactly which mock functions are called.

## Limitations

### Current Scope

- **Not a full LVGL emulation** - Only implements functions for wizard testing
- **No layout calculation** - Widgets have no position/size
- **No rendering** - Visual appearance not tested
- **Simplified event system** - All callbacks fire immediately (no event queue)
- **No timer scheduling** - Timers only fire when `process_timers()` called manually
- **No widget hierarchy** - No parent/child relationships tracked

### Testing Coverage

**Covered:**
- Connection flow (button clicks, textarea input, status updates)
- Validation logic (IP address, port number)
- Callback triggering (connected, disconnected)
- Subject updates

**Not Covered:**
- Visual layout/appearance (use screenshot tests for this)
- Touch input/gestures (use real LVGL simulator for this)
- Animation/transitions (use real LVGL simulator for this)
- Memory leaks in LVGL (use Valgrind/ASan with real LVGL)

## Future Enhancements

### Short Term (Next Integration Tests)

1. **Add wizard_validation mocks** - Mock `is_valid_ip_or_hostname()`, `is_valid_port()`
2. **Add config mocks** - Mock `Config::get<>()`, `Config::set<>()`
3. **Timer scheduling** - Track timer periods and trigger at correct mock time
4. **Event filtering** - Respect `lv_event_code_t filter` in `lv_obj_add_event_cb()`

### Medium Term (More Panel Tests)

5. **Widget hierarchy** - Track parent/child relationships
6. **More widgets** - Mock buttons, labels, rollers, dropdowns as needed
7. **More MoonrakerClient** - Mock `discover_printer()`, response callbacks
8. **Test fixtures** - Pre-built printer states (idle, printing, error)

### Long Term (Advanced Testing)

9. **Screenshot comparison** - Capture mock widget state for visual regression
10. **Performance testing** - Measure callback latency, memory usage
11. **Concurrency testing** - Multi-threaded callback triggering
12. **Fuzzing** - Random input generation for robustness testing

## Usage Patterns

### Basic Mock Setup

```cpp
TEST_CASE("Feature test") {
    // Initialize mocks
    LVGLMock::init();
    MoonrakerClientMock mock_client;

    // Test code here

    // Cleanup
    LVGLMock::reset();
    mock_client.reset();
}
```

### Simulating UI Interaction

```cpp
// Set textarea values
LVGLMock::set_textarea_value("ip_input", "192.168.1.100");

// Trigger button click
LVGLMock::trigger_button_click("btn_test");

// Verify subject updated
REQUIRE(LVGLMock::get_subject_value("status") == "Success!");
```

### Simulating Connection

```cpp
bool connected = false;
mock_client.connect("ws://test:7125/websocket",
    [&]() { connected = true; },
    []() {});

// Verify connection attempted
REQUIRE(mock_client.get_last_connect_url() == "ws://test:7125/websocket");

// Simulate success
mock_client.trigger_connected();
REQUIRE(connected);
```

### Testing Timeouts

```cpp
// Create timer
lv_timer_t* timer = lv_timer_create(callback, 5000, nullptr);

// Advance time
LVGLMock::advance_time(6000);  // +6 seconds

// Check if timeout occurred
REQUIRE(lv_tick_get() == 6000);

// Manually trigger timers
LVGLMock::process_timers();
```

## Success Criteria Met

✅ **MoonrakerClient Mock**
- Stores connection callbacks
- Tracks URL and RPC methods
- Provides test control API (`trigger_connected`, `trigger_disconnected`, `reset`)
- Compiles without errors

✅ **LVGL Mock**
- Implements 13 essential LVGL functions
- Provides test control API in `LVGLMock` namespace
- On-demand widget creation
- Compiles without errors

✅ **Keyboard Mock**
- No-op implementation of all keyboard functions
- Compiles without errors

✅ **Documentation**
- README with usage examples
- Implementation notes
- Test cases demonstrating all features

✅ **Build Integration**
- Makefile updated with mock compilation rules
- Separate `test-integration` target
- All tests passing (60 unit + 3 integration)

✅ **Ready for Wizard UI Tests**
- Mocks cover all wizard connection screen requirements
- Example test shows complete connection flow
- Easy to extend for printer identification, WiFi, etc.

## Next Steps

1. **Create wizard connection integration test**
   - `tests/unit/test_wizard_connection.cpp`
   - Test complete connection flow from button click to discovery
   - Verify validation errors, timeout handling

2. **Add config/validation mocks**
   - Mock `Config` class for get/set operations
   - Mock `wizard_validation` functions

3. **Expand coverage**
   - Printer identification screen
   - WiFi configuration screen
   - Completion screen

4. **Refine mocks**
   - Add timer scheduling
   - Improve event filtering
   - Track widget hierarchy

## File Statistics

- **Total Lines Added:** ~1,500 lines
- **Mock Code:** ~570 lines (headers + implementation)
- **Documentation:** ~650 lines (README + notes)
- **Test Examples:** ~290 lines
- **Build System:** ~30 lines (Makefile changes)

## Dependencies

**Required:**
- Catch2 v3 (already integrated)
- spdlog (already integrated)
- nlohmann/json (from libhv)

**No New Dependencies Added!**
