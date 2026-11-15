# Test Mocks

This directory contains mock implementations for wizard UI integration testing.

## Overview

The mocks enable testing wizard connection flow without:
- Real WebSocket connections to Moonraker
- Actual LVGL rendering
- Physical display hardware

## Files

### MoonrakerClient Mock

**`moonraker_client_mock.h`** / **`moonraker_client_mock.cpp`**

Simulates `MoonrakerClient` WebSocket connection behavior.

**Key Features:**
- Stores connection callbacks for manual triggering
- Tracks last connection URL for verification
- Records all RPC methods called
- Provides test control API

**Test Control Methods:**
```cpp
MoonrakerClientMock mock;

// Simulate connection
mock.connect("ws://localhost:7125/websocket", on_connected, on_disconnected);

// Trigger success
mock.trigger_connected();  // Calls on_connected callback

// Trigger failure
mock.trigger_disconnected();  // Calls on_disconnected callback

// Verify URL
REQUIRE(mock.get_last_connect_url() == "ws://localhost:7125/websocket");

// Reset state
mock.reset();
```

### LVGL Mock

**`lvgl_mock.h`** / **`lvgl_mock.cpp`**

Minimal LVGL implementation for wizard UI testing.

**Mocked Functions:**
- Widget creation: `lv_xml_create()`, `lv_obj_find_by_name()`
- Textareas: `lv_textarea_get_text()`, `lv_textarea_set_text()`
- Subjects: `lv_subject_init_string()`, `lv_subject_set_string()`
- Events: `lv_obj_add_event_cb()`, `lv_event_send()`
- Timers: `lv_timer_create()`, `lv_timer_delete()`, `lv_tick_get()`

**Test Control API:**
```cpp
#include "lvgl_mock.h"

// Initialize mock
LVGLMock::init();

// Set textarea value
LVGLMock::set_textarea_value("ip_input", "192.168.1.100");

// Get textarea value
std::string ip = LVGLMock::get_textarea_value("ip_input");

// Get subject value
std::string status = LVGLMock::get_subject_value("connection_status");

// Trigger button click
LVGLMock::trigger_button_click("btn_test_connection");

// Advance time (for timeout testing)
LVGLMock::advance_time(5000);  // +5 seconds
LVGLMock::process_timers();

// Reset all state
LVGLMock::reset();
```

### Keyboard Mock

**`ui_keyboard_mock.cpp`**

No-op implementation of `ui_keyboard.h` functions.

All functions are stubs that log calls but perform no actions.

## Usage in Tests

### Example: Wizard Connection Test

```cpp
#include "catch_amalgamated.hpp"
#include "lvgl_mock.h"
#include "moonraker_client_mock.h"
#include "ui_wizard.h"

TEST_CASE("Wizard connection flow", "[wizard][integration]") {
    // Setup
    LVGLMock::init();
    MoonrakerClientMock mock_client;

    ui_wizard_init(&mock_client, config_instance);

    SECTION("Successful connection") {
        // Arrange
        LVGLMock::set_textarea_value("ip_input", "192.168.1.100");
        LVGLMock::set_textarea_value("port_input", "7125");

        // Act - trigger button click
        LVGLMock::trigger_button_click("btn_test_connection");

        // Assert - verify connection attempted
        REQUIRE(mock_client.get_last_connect_url() == "ws://192.168.1.100:7125/websocket");

        // Simulate success
        mock_client.trigger_connected();

        // Verify status updated
        REQUIRE(LVGLMock::get_subject_value("connection_status") == "Connected!");
    }

    // Cleanup
    LVGLMock::reset();
    mock_client.reset();
}
```

## Design Philosophy

### Minimal Mocking
Only mock what wizard connection screen needs. Don't try to implement full LVGL.

### On-Demand Creation
`lv_obj_find_by_name()` creates widgets if not found - simplifies test setup.

### Clear Logging
All mocks use spdlog with `[MockXXX]` prefixes for debugging.

### Stateless
All mocks can be reset to clean state with `reset()` methods.

## Integration with Build System

Mocks are compiled into test binary via `Makefile`:

```makefile
MOCK_OBJS := $(OBJ_DIR)/tests/mocks/moonraker_client_mock.o \
             $(OBJ_DIR)/tests/mocks/lvgl_mock.o \
             $(OBJ_DIR)/tests/mocks/ui_keyboard_mock.o

$(TEST_BIN): $(TEST_MAIN_OBJ) $(CATCH2_OBJ) $(TEST_OBJS) $(MOCK_OBJS) ...
```

## Limitations

1. **Not a full LVGL emulation** - Only implements functions used by wizard
2. **No layout calculation** - Widgets have no position/size
3. **No rendering** - Visual appearance not tested
4. **Simplified event system** - All callbacks fire immediately
5. **No timer scheduling** - Timers only fire when `process_timers()` called

For comprehensive UI testing, use screenshot-based visual regression tests with real LVGL.

## Future Enhancements

- Add more LVGL functions as needed for other panel tests
- Implement timer scheduling for realistic timeout testing
- Add widget hierarchy (parent/child relationships)
- Mock more MoonrakerClient methods (discover_printer, send_jsonrpc callbacks)
