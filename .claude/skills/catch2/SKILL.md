---
name: catch2
description: >
  Catch2 v3.5 test framework knowledge for HelixScreen C++ development.
  Use when writing unit tests, UI tests, test fixtures, matchers, generators,
  or any Catch2-related code. Includes HelixScreen-specific patterns:
  amalgamated distribution, parallel sharding, tag system, UI test utilities,
  mock patterns, and test infrastructure. Trigger on TEST_CASE, SECTION,
  REQUIRE, or any test-related code in the HelixScreen project.
---

# Catch2 for HelixScreen

HelixScreen uses **Catch2 v3.5.1** (amalgamated distribution) with ~2,050 TEST_CASEs across 203 test files.

## Build & Run

```bash
make test              # Build tests (does not run)
make test-run          # Parallel, excludes [slow] and hidden (~27s)
make test-fast         # Same as test-run
make test-all          # Parallel, includes [slow]
make test-serial       # Sequential for debugging
make test-verbose      # Sequential with timing

# Specific tags
./build/bin/helix-tests "[connection]" "~[.]"   # Always use ~[.] to exclude hidden
make test-ui            # [ui] tag
make test-core          # [core] tag (must pass)

# Sanitizers
make test-asan          # AddressSanitizer
make test-tsan          # ThreadSanitizer
make test-asan-one TEST="[tag]"
```

## Tag System

### Importance Tags
| Tag | Purpose |
|-----|---------|
| `[core]` | Critical — if these fail, app is broken |
| `[slow]` | Network/timing tests, excluded from `test-run` |
| `[eventloop]` | Uses `hv::EventLoop`, very slow (always with `[slow]`) |

### Feature Tags
`[ui]` `[gcode]` `[ams]` `[print]` `[state]` `[filament]` `[application]` `[config]` `[printer]` `[connection]` `[api]` `[network]` `[calibration]` `[wizard]` `[history]` `[assets]` `[predictor]`

### Hidden Tags (excluded by `~[.]`)
`[.pending]` `[.integration]` `[.slow]` `[.disabled]` `[xml_required]` `[ui_integration]` `[.stress]`

## Test File Conventions

- Location: `tests/unit/test_*.cpp`
- Naming: `test_<feature>_<aspect>.cpp` (e.g., `test_moonraker_client_subscription_cancel.cpp`)
- Tags: Always tag with feature + importance
- Binary: `build/bin/helix-tests`

## HelixScreen Test Patterns

### Tagging a New Test
```cpp
TEST_CASE("My feature works", "[state][core]") {
    // Will run with make test-core, make test-run
}
```

### Slow Test
```cpp
TEST_CASE("WebSocket reconnect", "[connection][slow][eventloop]") {
    // Uses hv::EventLoop → must be [slow]
}
```

### Parallel Sharding
Tests run in parallel by default using Catch2's `--shard-count`/`--shard-index`:
- Each shard gets its own LVGL instance
- 4 cores: ~3.5x speedup, 8 cores: ~6x, 14 cores: ~9x

## UI Test Utilities

Headless LVGL testing via `tests/ui_test_utils.h`:

```cpp
#include "../ui_test_utils.h"

// Fixture setup
lv_obj_t* screen = lv_obj_create(lv_screen_active());
UITest::init(screen);        // Creates virtual input + headless display (800x480)

// Interaction
UITest::click(widget);                    // Click widget center
UITest::click_at(400, 240);               // Click coordinates
UITest::type_text("Hello");               // Type into focused textarea
UITest::send_key(LV_KEY_ENTER);           // Special key

// Find widgets
lv_obj_t* btn = UITest::find_by_name(parent, "my_button");

// Wait for condition
UITest::wait_until([&]{ return condition; }, 1000);  // timeout ms

// Cleanup
UITest::cleanup();
```

## When to Tag [slow]
- Creates `hv::EventLoop` → also add `[eventloop]`
- Uses `std::this_thread::sleep_for()`
- Uses fixtures with network clients
- Takes >500ms

## Reference Files

| Topic | File |
|-------|------|
| Test cases & sections | `references/test-cases-and-sections.md` |
| Assertions (REQUIRE, CHECK) | `references/assertions.md` |
| Matchers | `references/matchers.md` |
| Generators | `references/generators.md` |
| Test fixtures | `references/test-fixtures.md` |
| Logging (INFO, WARN) | `references/logging.md` |
| Skip/pass/fail | `references/skipping-passing-failing.md` |
| Command line options | `references/command-line.md` |
| Configuration | `references/configuration.md` |
| Event listeners | `references/event-listeners.md` |
| Other macros | `references/other-macros.md` |
| Reporters | `references/reporters.md` |
| Benchmarks | `references/benchmarks.md` |
| Floating point comparison | `references/comparing-floating-point-numbers.md` |
| Filtering | `references/filtering-execution-path.md` |
| CMake integration | `references/cmake-integration.md` |
| Custom main | `references/own-main.md` |
