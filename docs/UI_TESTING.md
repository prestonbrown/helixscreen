# UI Testing Guide

## Overview

HelixScreen uses headless LVGL testing with virtual input devices to test UI components without requiring a display. This allows automated testing of widget hierarchies, user interactions, and state changes.

**Test Framework:** Catch2 v3.5.1
**Test Utilities:** `tests/ui_test_utils.h/cpp`
**Test Location:** `tests/unit/test_*.cpp`

## Architecture

```
┌─────────────────────────────────────────┐
│  Catch2 Test Framework                  │
│  ┌───────────────────────────────────┐  │
│  │ Test Fixture (creates LVGL env)  │  │
│  │  ┌─────────────────────────────┐ │  │
│  │  │ UITest Utilities            │ │  │
│  │  │  - click()                  │ │  │
│  │  │  - type_text()              │ │  │
│  │  │  - wait_until()             │ │  │
│  │  │  - find_by_name()           │ │  │
│  │  └─────────────────────────────┘ │  │
│  │  ┌─────────────────────────────┐ │  │
│  │  │ Virtual Input Device        │ │  │
│  │  │  (simulates touch/click)    │ │  │
│  │  └─────────────────────────────┘ │  │
│  │  ┌─────────────────────────────┐ │  │
│  │  │ Headless LVGL Display       │ │  │
│  │  │  (800x480 virtual screen)   │ │  │
│  │  └─────────────────────────────┘ │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

## UITest Utilities Reference

### Initialization

```cpp
#include "../ui_test_utils.h"

// In test fixture constructor (after LVGL init)
lv_obj_t* screen = lv_obj_create(lv_screen_active());
UITest::init(screen);

// In test fixture destructor
UITest::cleanup();
```

### Widget Interaction

#### Click Simulation
```cpp
// Click widget at its center
lv_obj_t* button = UITest::find_by_name(parent, "my_button");
UITest::click(button);

// Click at specific coordinates
UITest::click_at(400, 240);  // Center of 800x480 screen
```

#### Text Input
```cpp
// Type into focused textarea
UITest::type_text("Hello World");

// Focus and type
lv_obj_t* textarea = UITest::find_by_name(parent, "input_field");
UITest::type_text(textarea, "password123");

// Special keys
UITest::send_key(LV_KEY_ENTER);
UITest::send_key(LV_KEY_BACKSPACE);
```

### Waiting & Timing

```cpp
// Fixed delay (processes LVGL tasks every 5ms)
UITest::wait_ms(500);

// Wait for condition (polls every 10ms)
bool success = UITest::wait_until([&]() {
    return some_state_changed;
}, 5000);  // 5 second timeout

// Wait for visibility changes
UITest::wait_for_visible(widget, 3000);
UITest::wait_for_hidden(widget, 3000);

// Wait for async operations (timers, animations)
UITest::wait_for_timers(10000);
```

### Widget Queries

```cpp
// Find widgets by name (recursive search)
lv_obj_t* widget = UITest::find_by_name(parent, "widget_name");

// Check visibility
bool visible = UITest::is_visible(widget);

// Get text content (labels, textareas)
std::string text = UITest::get_text(widget);

// Check checked/selected state (switches, checkboxes)
bool checked = UITest::is_checked(widget);

// Count dynamic children (e.g., list items)
int count = UITest::count_children_with_marker(parent, "network_item");
```

## Writing Test Fixtures

### Basic Test Fixture Pattern

```cpp
class MyUIFixture {
public:
    MyUIFixture() {
        // 1. Initialize LVGL (once per process)
        static bool lvgl_initialized = false;
        if (!lvgl_initialized) {
            lv_init();
            lvgl_initialized = true;
        }

        // 2. Create headless display
        static lv_color_t buf[800 * 10];  // 10-line buffer
        display = lv_display_create(800, 480);
        lv_display_set_buffers(display, buf, nullptr, sizeof(buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, [](lv_display_t* disp,
                                             const lv_area_t* area,
                                             uint8_t* px_map) {
            lv_display_flush_ready(disp);  // Dummy flush
        });

        // 3. Create test screen
        screen = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen, 800, 480);

        // 4. Register XML components (once, after LVGL init)
        ensure_components_registered();

        // 5. Initialize subjects
        ui_my_component_init_subjects();

        // 6. Create UI
        my_panel = lv_xml_create(screen, "my_panel", nullptr);

        // 7. Initialize UITest system
        UITest::init(screen);
    }

    ~MyUIFixture() {
        // Clean up in reverse order
        UITest::cleanup();

        if (my_panel) {
            lv_obj_delete(my_panel);
            my_panel = nullptr;
        }

        if (screen) {
            lv_obj_delete(screen);
            screen = nullptr;
        }

        if (display) {
            lv_display_delete(display);
            display = nullptr;
        }
    }

    lv_obj_t* screen = nullptr;
    lv_display_t* display = nullptr;
    lv_obj_t* my_panel = nullptr;
};
```

### Global Component Registration

XML components should be registered **once per test run, after LVGL init**:

```cpp
// At file scope
static bool components_registered = false;

static void ensure_components_registered() {
    if (!components_registered) {
        lv_xml_component_register_from_file("A:ui_xml/globals.xml");
        lv_xml_component_register_from_file("A:ui_xml/my_component.xml");
        ui_custom_widget_register();
        components_registered = true;
    }
}
```

**❌ Don't** register in static initializers (runs before LVGL init)
**❌ Don't** register in each fixture constructor (causes errors)
**✅ Do** register on-demand after LVGL is initialized

## Writing Test Cases

### Basic Test Structure

```cpp
TEST_CASE_METHOD(MyUIFixture, "Component: Test description", "[tag][component]") {
    // Arrange - find widgets
    lv_obj_t* button = UITest::find_by_name(screen, "my_button");
    REQUIRE(button != nullptr);

    // Act - simulate interaction
    UITest::click(button);
    UITest::wait_ms(100);

    // Assert - verify state
    REQUIRE(some_state_changed);
}
```

### Widget Hierarchy Validation

```cpp
TEST_CASE_METHOD(MyUIFixture, "Modal: All widgets exist", "[modal]") {
    lv_obj_t* modal = UITest::find_by_name(screen, "my_modal");
    REQUIRE(modal != nullptr);
    REQUIRE_FALSE(UITest::is_visible(modal));  // Hidden initially

    // Verify child widgets
    lv_obj_t* title = UITest::find_by_name(modal, "title");
    REQUIRE(title != nullptr);
    REQUIRE(UITest::get_text(title) == "Expected Title");

    lv_obj_t* input = UITest::find_by_name(modal, "input_field");
    REQUIRE(input != nullptr);

    lv_obj_t* ok_btn = UITest::find_by_name(modal, "ok_button");
    REQUIRE(ok_btn != nullptr);
}
```

### State Change Testing

```cpp
TEST_CASE_METHOD(MyUIFixture, "Toggle: Changes state", "[toggle]") {
    lv_obj_t* toggle = UITest::find_by_name(screen, "my_toggle");
    REQUIRE(toggle != nullptr);

    // Initial state
    REQUIRE_FALSE(UITest::is_checked(toggle));

    // Simulate click
    UITest::click(toggle);
    UITest::wait_ms(50);

    // Verify state change
    REQUIRE(UITest::is_checked(toggle));
}
```

### Async Operation Testing

```cpp
TEST_CASE_METHOD(MyUIFixture, "Network: Scan completes", "[network]") {
    // Start async scan
    start_network_scan();

    // Wait for completion
    bool completed = UITest::wait_until([&]() {
        return scan_status == SCAN_COMPLETE;
    }, 5000);

    REQUIRE(completed);

    // Verify results
    int network_count = UITest::count_children_with_marker(
        network_list, "network_item"
    );
    REQUIRE(network_count > 0);
}
```

## Test Tags

Use tags to organize and run specific test groups:

```cpp
// Single tag
TEST_CASE_METHOD(Fixture, "Test", "[wizard]") { ... }

// Multiple tags
TEST_CASE_METHOD(Fixture, "Test", "[wizard][wifi][ui]") { ... }

// Disable test
TEST_CASE_METHOD(Fixture, "Test", "[wizard][.disabled]") { ... }

// Platform-specific
#ifdef __APPLE__
TEST_CASE_METHOD(Fixture, "Test", "[macos]") { ... }
#endif
```

### Running Tests by Tag

```bash
# Run all tests
./build/bin/run_tests

# Run tests with specific tags
./build/bin/run_tests "[wizard]"
./build/bin/run_tests "[wizard][wifi]"

# Exclude disabled tests
./build/bin/run_tests "~[.disabled]"
./build/bin/run_tests "[wizard]~[.disabled]"

# Run specific test by name
./build/bin/run_tests "Wizard WiFi: Password modal"

# List available tests
./build/bin/run_tests --list-tests
./build/bin/run_tests "[wizard]" --list-tests
```

## Known Limitations & Workarounds

### 1. Multiple Fixture Instances Cause Segfaults

**Problem:** Creating multiple LVGL UI instances in sequence causes crashes.

**Symptoms:**
- First test passes
- Second test segfaults during fixture construction
- Error: "Segmentation violation signal"

**Root Cause:** Incomplete LVGL object hierarchy cleanup between tests.

**Workaround:**
```cpp
// Disable problematic tests
TEST_CASE_METHOD(Fixture, "Test 2", "[.disabled]") { ... }

// Run tests individually
./build/bin/run_tests "Test 1"
./build/bin/run_tests "Test 2"
```

**Proper Fix (TODO):**
1. Ensure all LVGL objects deleted before display deletion
2. Verify subject cleanup in `init_subjects()` functions
3. Add explicit `lv_obj_clean()` calls in fixture destructor

### 2. Virtual Input Events Don't Trigger ui_switch

**Problem:** `UITest::click()` doesn't trigger `LV_EVENT_VALUE_CHANGED` on custom widgets.

**Affected Widgets:**
- `ui_switch` (custom toggle widget)
- Possibly other custom components

**Workaround:**
```cpp
// Option 1: Test via C++ API instead of UI simulation
WiFiManager::set_enabled(true);
REQUIRE(WiFiManager::is_enabled());

// Option 2: Test subjects directly
lv_subject_set_int(&my_subject, 42);
REQUIRE(lv_subject_get_int(&my_subject) == 42);

// Option 3: Manually trigger event
lv_event_send(widget, LV_EVENT_VALUE_CHANGED, nullptr);
```

**Proper Fix (TODO):** Investigate why custom widgets don't receive indev events in test environment.

### 3. Font and Constant Warnings

**Problem:** LVGL XML warnings about missing fonts/constants in test output.

**Symptoms:**
```
[Warn] lv_xml_get_font: No font was found with name "montserrat_16"
[Warn] lv_xml_get_const: No constant was found with name "bg_secondary"
```

**Impact:** None (uses default fonts, tests still pass)

**Workaround:** Ignore warnings or load actual font/constant definitions.

## Best Practices

### DO ✅

1. **Test widget structure first** - Verify all components exist before testing interactions
2. **Use descriptive test names** - "Component: What it tests" format
3. **Tag tests appropriately** - Makes selective testing easier
4. **Wait for async operations** - Use `wait_until()` instead of fixed delays
5. **Test from user perspective** - Simulate real interactions
6. **Document test limitations** - Mark disabled tests with reasons
7. **One assertion per test** - Or group related assertions logically

### DON'T ❌

1. **Don't rely on timing** - Use condition waits instead of `wait_ms()`
2. **Don't test implementation details** - Test behavior, not internals
3. **Don't create complex fixtures** - Keep setup simple and focused
4. **Don't skip cleanup** - Always delete objects in reverse creation order
5. **Don't use static initializers** - Register components after LVGL init
6. **Don't ignore segfaults** - Investigate fixture cleanup issues
7. **Don't commit disabled tests without documentation** - Explain why disabled

## Examples

### Complete Test File Template

```cpp
#include "../catch_amalgamated.hpp"
#include "../ui_test_utils.h"
#include "../../include/ui_my_component.h"
#include "../../lvgl/lvgl.h"

// Global component registration (once)
static bool components_registered = false;

static void ensure_components_registered() {
    if (!components_registered) {
        lv_xml_component_register_from_file("A:ui_xml/globals.xml");
        lv_xml_component_register_from_file("A:ui_xml/my_component.xml");
        components_registered = true;
    }
}

// Test fixture
class MyComponentFixture {
public:
    MyComponentFixture() {
        static bool lvgl_initialized = false;
        if (!lvgl_initialized) {
            lv_init();
            lvgl_initialized = true;
        }

        static lv_color_t buf[800 * 10];
        display = lv_display_create(800, 480);
        lv_display_set_buffers(display, buf, nullptr, sizeof(buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, [](lv_display_t* disp,
                                             const lv_area_t* area,
                                             uint8_t* px_map) {
            lv_display_flush_ready(disp);
        });

        screen = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen, 800, 480);

        ensure_components_registered();
        ui_my_component_init_subjects();

        component = lv_xml_create(screen, "my_component", nullptr);
        UITest::init(screen);
    }

    ~MyComponentFixture() {
        UITest::cleanup();
        if (component) lv_obj_delete(component);
        if (screen) lv_obj_delete(screen);
        if (display) lv_display_delete(display);
    }

    lv_obj_t* screen = nullptr;
    lv_display_t* display = nullptr;
    lv_obj_t* component = nullptr;
};

// Tests
TEST_CASE_METHOD(MyComponentFixture, "Component: Widgets exist", "[component]") {
    REQUIRE(component != nullptr);

    lv_obj_t* child = UITest::find_by_name(component, "child_widget");
    REQUIRE(child != nullptr);
}

TEST_CASE_METHOD(MyComponentFixture, "Component: Button click", "[component]") {
    lv_obj_t* button = UITest::find_by_name(component, "my_button");
    REQUIRE(button != nullptr);

    UITest::click(button);
    UITest::wait_ms(100);

    // Verify expected state change
    REQUIRE(something_happened);
}
```

## Debugging Tests

### Enable Verbose Output

```bash
# Show successful assertions
./build/bin/run_tests --success

# Show all output
./build/bin/run_tests -s
```

### Use spdlog for Debugging

```cpp
#include <spdlog/spdlog.h>

TEST_CASE_METHOD(Fixture, "Debug test", "[debug]") {
    spdlog::info("[Test] Starting test");

    lv_obj_t* widget = UITest::find_by_name(screen, "widget");
    spdlog::debug("[Test] Found widget: {}", (void*)widget);

    UITest::click(widget);
    spdlog::info("[Test] Clicked widget");

    REQUIRE(widget != nullptr);
}
```

### Check Widget Hierarchy

```cpp
// Print all child widget names
void print_children(lv_obj_t* parent, int depth = 0) {
    int32_t count = lv_obj_get_child_count(parent);
    for (int32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, i);
        // Note: Not all widgets have names
        spdlog::info("{}{}: child {}", std::string(depth * 2, ' '),
                     depth, i);
    }
}
```

## References

- **Test Utilities Implementation:** `tests/ui_test_utils.h/cpp`
- **Example Test File:** `tests/unit/test_wizard_wifi_ui.cpp`
- **Catch2 Documentation:** https://github.com/catchorg/Catch2
- **LVGL Testing Guide:** `docs/LVGL9_XML_GUIDE.md`
- **Current Test Status:** `HANDOFF.md` (Known Issues section)
