# HelixScreen UI Prototype - Testing Framework

This directory contains the testing infrastructure for the LVGL 9 UI prototype.

## Directory Structure

```
tests/
├── catch_amalgamated.hpp / .cpp  # Catch2 v3.5.1 (vendored, amalgamated single-header build)
├── unit/               # In-process Catch2 tests — build widgets inside the test
│   │                   # binary (fast, fine-grained; can't reach app lifecycle,
│   │                   # navigation, or async population)
│   ├── test_main.cpp   # Test runner entry point
│   └── test_*.cpp      # Individual test files
├── ui/                 # Out-of-process pytest suite — drives a real running
│   │                   # helix-screen instance via `helix-screen ctl --json`,
│   │                   # covering app boot, navigation, and async population.
│   │                   # See docs/devel/UI_TESTING.md § "Out-of-Process Tests".
│   ├── conftest.py
│   ├── helix/app.py    # HelixApp — wraps each ctl call as a subprocess
│   ├── goldens/         # Committed reference screenshots
│   └── test_*.py
└── README.md           # This file (in-process Catch2 tests only — see tests/ui/
                        # above and docs/devel/UI_TESTING.md for the pytest suite)
```

## Test Framework: Catch2 v3.5.1

We use **Catch2 v3.5.1**, amalgamated (single translation unit) rather than the
CMake/submodule build, so `make test` stays a plain Makefile target.

- Include: `#include "../catch_amalgamated.hpp"` in test files (not `../framework/catch.hpp` —
  that path is from an earlier v2.x vendoring and no longer exists).
- `TEST_CASE`/`SECTION`/`REQUIRE` all work the same as v2.x for the patterns below; a few
  APIs changed between v2 and v3 (see Catch2's own migration notes if something doesn't
  compile as shown).

## Running Tests

### Quick Start

```bash
# Build and run all unit tests
make test

# Build tests without running
make build/bin/helix-tests

# Run tests manually
./build/bin/helix-tests

# Run specific test cases
./build/bin/helix-tests "[navigation]"

# List all available test cases
./build/bin/helix-tests --list-tests

# Show verbose output
./build/bin/helix-tests -s
```

### Clean Build

```bash
make clean
make test
```

## Writing Unit Tests

### Basic Test Structure

Create a new file in `tests/unit/test_<feature>.cpp`:

```cpp
#include "../catch_amalgamated.hpp"
#include "../../include/your_header.h"

TEST_CASE("Feature description", "[tag]") {
    SECTION("Specific scenario") {
        // Arrange
        int expected = 42;

        // Act
        int result = your_function();

        // Assert
        REQUIRE(result == expected);
    }
}
```

### Using Test Fixtures

For tests requiring setup/teardown:

```cpp
class MyTestFixture {
public:
    MyTestFixture() {
        // Setup code
        lv_init();
    }

    ~MyTestFixture() {
        // Teardown code (LVGL handles its own cleanup)
    }
};

TEST_CASE_METHOD(MyTestFixture, "Test with fixture", "[tag]") {
    // Test code with access to fixture members
}
```

### Testing LVGL Components

Navigation tests demonstrate LVGL testing patterns:

```cpp
class NavigationTestFixture {
public:
    NavigationTestFixture() {
        // Initialize LVGL
        lv_init();

        // Create headless display for testing
        lv_display_t* disp = lv_display_create(800, 480);
        static lv_color_t buf[800 * 10];
        lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);

        // Initialize component under test
        ui_nav_init();
    }
};

TEST_CASE_METHOD(NavigationTestFixture, "Panel switching", "[navigation]") {
    ui_nav_set_active(UI_PANEL_CONTROLS);
    REQUIRE(ui_nav_get_active() == UI_PANEL_CONTROLS);
}
```

**Key Points:**
- LVGL must be initialized with `lv_init()`
- Create a headless display for testing (no SDL window needed)
- Static buffers ensure they persist for display lifetime
- LVGL handles cleanup automatically

### Catch2 Assertions

```cpp
// Basic comparisons
REQUIRE(value == expected);
REQUIRE(value != unexpected);
REQUIRE(value < max);

// Boolean checks
REQUIRE(is_valid);
REQUIRE_FALSE(is_invalid);

// Floating point (with epsilon)
REQUIRE(value == Approx(3.14).epsilon(0.01));

// Exceptions
REQUIRE_THROWS(dangerous_function());
REQUIRE_NOTHROW(safe_function());

// String matching
REQUIRE_THAT(str, Catch::Contains("substring"));
REQUIRE_THAT(str, Catch::StartsWith("prefix"));
```

### Test Organization

**Tags** help organize and filter tests:

```cpp
TEST_CASE("Fast computation", "[unit][math][fast]") { }
TEST_CASE("Slow rendering", "[ui][rendering][slow]") { }

// Run only fast tests:
./build/bin/helix-tests "[fast]"

// Run all UI tests:
./build/bin/helix-tests "[ui]"

// Exclude slow tests:
./build/bin/helix-tests "~[slow]"
```

**Sections** group related assertions:

```cpp
TEST_CASE("Navigation system", "[navigation]") {
    SECTION("Default state") {
        REQUIRE(ui_nav_get_active() == UI_PANEL_HOME);
    }

    SECTION("Panel switching") {
        ui_nav_set_active(UI_PANEL_CONTROLS);
        REQUIRE(ui_nav_get_active() == UI_PANEL_CONTROLS);
    }
}
```

Each `SECTION` runs independently with a fresh fixture.

## Out-of-Process / Integration Tests (`tests/ui/`)

`tests/integration/test-navigation.sh` — the old "launch it, click icons, eyeball it"
manual script this section used to document — is gone. It's been replaced by
`tests/ui/`: a real pytest suite that boots the actual `helix-screen` binary and drives
it through `helix-screen ctl --json`, covering exactly what that manual checklist used
to ask a human to verify (navigation, panel switching, screenshot capture) plus golden
image regression, all scripted and re-runnable.

```bash
make -j
./.venv/bin/python -m pytest tests/ui/ -v
```

Full detail — fixtures, `HelixApp`, golden mechanics, environment variables — lives in
`docs/devel/UI_TESTING.md` § "Out-of-Process Tests", not duplicated here.

## Testing Best Practices

### What to Test

**Unit tests:**
- Pure functions (no LVGL dependencies)
- State management logic (navigation, data models)
- Subject-Observer bindings
- Utility functions

**Integration tests:**
- Full UI rendering from XML
- User interactions (clicks, touch events)
- Panel transitions and animations
- Visual regression (screenshot comparison)

### What NOT to Test

- LVGL library internals (already tested by LVGL)
- SDL2 driver functionality
- Operating system behavior

### Test-Driven Development

1. **Red** - Write failing test first:
   ```cpp
   TEST_CASE("New feature") {
       REQUIRE(new_feature() == expected_result);
   }
   ```

2. **Green** - Implement minimal code to pass:
   ```cpp
   int new_feature() {
       return expected_result;
   }
   ```

3. **Refactor** - Clean up with tests passing

### Continuous Testing

```bash
# Watch for changes and rebuild (using entr or similar)
ls src/*.cpp include/*.h tests/unit/*.cpp | entr -c make test

# Or manually during development
make && make test
```

## Common Testing Patterns

### Testing Subject Updates

```cpp
TEST_CASE("Subject reactivity", "[subjects]") {
    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    lv_subject_set_int(&subject, 42);
    REQUIRE(lv_subject_get_int(&subject) == 42);
}
```

### Testing Panel State

```cpp
TEST_CASE("Panel visibility", "[panels]") {
    lv_obj_t* panel = lv_obj_create(lv_screen_active());
    lv_obj_set_hidden(panel, true);

    REQUIRE(lv_obj_is_hidden(panel));

    lv_obj_set_hidden(panel, false);
    REQUIRE_FALSE(lv_obj_is_hidden(panel));
}
```

### Testing Event Handlers

```cpp
static bool event_triggered = false;

void test_event_cb(lv_event_t* e) {
    event_triggered = true;
}

TEST_CASE("Event handling", "[events]") {
    event_triggered = false;
    lv_obj_t* btn = lv_button_create(lv_screen_active());
    lv_obj_add_event_cb(btn, test_event_cb, LV_EVENT_CLICKED, NULL);

    // Simulate click
    lv_event_send(btn, LV_EVENT_CLICKED, NULL);

    REQUIRE(event_triggered);
}
```

## Debugging Failed Tests

### Verbose Output

```bash
# Show all test details
./build/bin/helix-tests -s

# Show successful assertions too
./build/bin/helix-tests -s --success
```

### Running Single Test

```bash
# By test name
./build/bin/helix-tests "Navigation initialization"

# By section name
./build/bin/helix-tests "Default active panel is HOME"

# By tag
./build/bin/helix-tests "[navigation]"
```

### Adding Debug Output

```cpp
TEST_CASE("Debug example", "[debug]") {
    int value = compute_value();

    // Use INFO for context (only shown on failure)
    INFO("Computed value: " << value);
    INFO("Expected range: 0-100");

    REQUIRE(value >= 0);
    REQUIRE(value <= 100);
}
```

## Extending the Test Suite

### Adding a New Test File

1. Create `tests/unit/test_<feature>.cpp`
2. Include Catch2: `#include "../catch_amalgamated.hpp"`
3. Include headers under test
4. Write test cases with tags
5. Run `make test` - Makefile auto-detects new files

### Adding Integration Tests

1. Create script in `tests/integration/`
2. Make executable: `chmod +x tests/integration/test-*.sh`
3. Document expected behavior in script comments
4. Update this README with usage instructions

### Updating Catch2

Catch2 v3 ships its amalgamated single-TU build as two files
(`catch_amalgamated.hpp` + `catch_amalgamated.cpp`) from the release's
`extras/` directory — not a single header the way v2.x was:

```bash
# Replace both files with a newer v3.x release's extras/catch_amalgamated.*
curl -L -o tests/catch_amalgamated.hpp \
  https://github.com/catchorg/Catch2/releases/download/v3.5.1/catch_amalgamated.hpp
curl -L -o tests/catch_amalgamated.cpp \
  https://github.com/catchorg/Catch2/releases/download/v3.5.1/catch_amalgamated.cpp

# Rebuild tests
make clean && make test
```

Swap `v3.5.1` for whichever release you're moving to. Check Catch2's migration notes
for API changes between minor versions before assuming existing tests still compile.

## Resources

- **Catch2 Documentation:** https://github.com/catchorg/Catch2/blob/devel/docs/Readme.md
- **LVGL Testing Guide:** https://docs.lvgl.io/master/others/testing.html
- **Test-Driven Development:** https://martinfowler.com/bliki/TestDrivenDevelopment.html

## Troubleshooting

### "undefined reference to lv_*" errors

Make sure test target includes LVGL objects:
```makefile
$(TEST_BIN): $(TEST_OBJS) $(LVGL_OBJS) $(OBJ_DIR)/ui_nav.o
```

### "catch.hpp: No such file or directory"

Ensure framework directory is in include path:
```makefile
$(CXX) $(CXXFLAGS) -I$(TEST_FRAMEWORK_DIR) $(INCLUDES) ...
```

### Tests pass but feature broken in UI

Unit tests may not catch integration issues. Add integration test with full XML rendering.

### Segfault in tests

Ensure LVGL is initialized before creating objects:
```cpp
lv_init();  // MUST be first
lv_display_create(800, 480);  // Create display
// Now create objects
```

---

**Happy Testing!** 🧪
