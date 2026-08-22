# Testing Infrastructure

**Status:** Active
**Last Updated:** 2026-08-08

---

## Quick Start

```bash
make test              # Build tests (does not run)
make test-run          # Run unit tests in parallel (~4-8x faster)
make test-fast         # Skip [slow] tests
make test-serial       # Sequential (for debugging)
make test-all          # Everything including [slow]

# Run specific tests
./build/bin/helix-tests "[connection]" "~[.]"
```

**⚠️ Always use `"~[.]"` when running by tag** to exclude hidden tests that may hang.

---

## Test Tag System

Tests are tagged by **feature/importance**, not layer/speed. This enables running all tests for a feature during development and identifying critical tests.

### Importance Tags

| Tag | Count | Purpose |
|-----|-------|---------|
| `[core]` | ~12 | Critical tests - if these fail, the app is fundamentally broken |
| `[slow]` | ~36 | Tests with network/timing - excluded from `test-run` |
| `[eventloop]` | ~2 | Uses `hv::EventLoop` - very slow, always paired with `[slow]` |

*Counts are TEST_CASE definitions; each can have multiple SECTIONs expanding the actual test paths.*

### Feature Tags

| Tag | Count | Purpose |
|-----|-------|---------|
| `[ui]` | ~162 | Theme, icons, widgets, panels |
| `[gcode]` | ~118 | G-code parsing, streaming, geometry |
| `[ams]` | ~117 | AMS/MMU backends |
| `[print]` | ~72 | Print workflow: start, pause, cancel, progress |
| `[state]` | ~57 | PrinterState singleton, LVGL subjects, observers - drive state via `tests/test_helpers/print_state_test_drivers.h` (see "Test Fixtures") |
| `[filament]` | ~53 | Spoolman, filament sensors |
| `[application]` | ~51 | Application lifecycle |
| `[config]` | ~50 | Configuration loading, validation |
| `[printer]` | ~32 | Printer detection, capabilities, hardware |
| `[assets]` | ~28 | Thumbnail extraction |
| `[wizard]` | ~27 | Setup wizard flow |
| `[history]` | ~27 | Print/notification history |
| `[network]` | ~26 | WiFi, Ethernet management |
| `[api]` | ~25 | Moonraker API infrastructure |
| `[connection]` | ~23 | WebSocket connection lifecycle, retry logic |
| `[calibration]` | ~17 | Bed mesh, input shaper, QGL, Z-tilt |
| `[predictor]` | ~15 | Pre-print time estimation |

### Sub-Tags

| Tag | Parent | Purpose |
|-----|--------|---------|
| `[afc]` | `[ams]` | AFC (Armored Filament Changer) backend |
| `[valgace]` | `[ams]` | Valgace AMS backend |
| `[ui_theme]` | `[ui]` | Theme colors, fonts |
| `[ui_icon]` | `[ui]` | Icon rendering |
| `[navigation]` | `[ui]` | Panel switching |

### Hidden Tags (Excluded by Default)

- `[.pending]` - Test not yet implemented
- `[.integration]` - Requires full environment
- `[.slow]` - Long-running (deprecated, use `[slow]`)
- `[.disabled]` - Temporarily disabled

Run `./build/bin/helix-tests "[.]" --list-tests` to see all hidden tests.

---

## Core Tests (~12 Must Pass)

These validate fundamental functionality:

**PrinterState** (`test_printer_state.cpp`): Singleton instance, persistence, subject addresses, observer notifications

**Navigation** (`test_navigation.cpp`): Initialization, panel switching, invalid panel handling, all panels accessible

**Config** (`test_config.cpp`): get() for string/int values, missing key handling, defaults

**Print Start** (`test_print_start_collector.cpp`): PRINT_START marker, completion marker, homing/heating phase detection

**UI** (`test_temp_graph.cpp`, `test_temp_graph_controller.cpp`, `test_temp_graph_overlay.cpp`, `test_temp_graph_scaling.cpp`, `test_panel_widget_temp_graph.cpp`): Graph create/destroy

---

## Make Targets

### By Speed/Scope

| Target | Behavior |
|--------|----------|
| `make test-run` | Parallel, excludes `[slow]` and hidden |
| `make test-fast` | Same as test-run |
| `make test-all` | Parallel, includes `[slow]` |
| `make test-slow` | Only `[slow]` tagged tests |
| `make test-eventloop` | Only `[eventloop]` tests (5-10 min) |
| `make test-serial` | Sequential for debugging |
| `make test-verbose` | Sequential with timing |

### By Feature

| Target | Tags |
|--------|------|
| `make test-core` | `[core]` |
| `make test-connection` | `[connection]` |
| `make test-state` | `[state]` |
| `make test-print` | `[print]` |
| `make test-gcode` | `[gcode]` |
| `make test-moonraker` | `[api]` |
| `make test-ui` | `[ui]` |
| `make test-network` | `[network]` |
| `make test-ams` | `[ams]` |
| `make test-calibration` | `[calibration]` |
| `make test-filament` | `[filament]` |
| `make test-security` | `[security]` |

### Sanitizers

| Target | Purpose |
|--------|---------|
| `make test-asan` | AddressSanitizer (memory leaks, use-after-free, overflows) |
| `make test-tsan` | ThreadSanitizer (data races, deadlocks) |
| `make test-asan-one TEST="[tag]"` | Run specific test with ASAN |
| `make test-tsan-one TEST="[tag]"` | Run specific test with TSAN |

Sanitizers add ~2-5x overhead. Use for debugging, not regular runs.

Not a Catch2 target: `make test-xml` builds and runs the separate helix-xml engine
suite (CMake + Unity). See [helix-xml Engine Tests](#helix-xml-engine-tests-separate-suite).

---

## Parallel Execution

Tests run in parallel by default using Catch2's sharding. Each shard runs in a separate process with its own LVGL instance.

```bash
# What make test-run does internally:
for i in $(seq 0 $((NPROCS-1))); do
    ./build/bin/helix-tests "~[.] ~[slow]" --shard-count $NPROCS --shard-index $i &
done
wait
```

| Machine | Serial | Parallel | Speedup |
|---------|--------|----------|---------|
| 4 cores | ~100s | ~30s | ~3.5x |
| 8 cores | ~100s | ~18s | ~6x |
| 14 cores | ~100s | ~12s | ~9x |

Use `make test-serial` when debugging failures or reading output.

### When a shard fails, crashes, or times out

The harness diagnoses it for you instead of leaving you to re-run by hand. For
each suspect shard it prints:

```
── shard diagnostics ──
logs preserved: /tmp/helix-shards-TINI6P

shard 95
  ran 194 test case(s) → /tmp/helix-shards-TINI6P/95.tests
  failing assertion(s): tests/unit/test_foo.cpp:27
  reproduce: build/bin/helix-tests "~[.] ~[slow]" --shard-count 96 --shard-index 95
  re-running alone…
  → REPRODUCED alone (exit 1): a real fault, not a flake
```

- **Logs are kept** (`$SHARD_ARTIFACT_ROOT`, default `/tmp`) whenever anything
  goes wrong, and deleted only on a fully clean run. `<n>.log` is the shard's
  output, `<n>.tests` the test cases it ran, `<n>.retry.log` the isolation re-run.
  Set `SHARD_ARTIFACT_ROOT=$(PWD)/build` in CI to collect them as artifacts.
- **Each suspect shard is re-run alone.** Green in isolation but red under the
  full parallel run means a load/timing flake, not a fault in the diff under
  review. Red both times is real.
- **A shard that dies with no `FAILED` marker** crashed *after* its assertions
  passed — a teardown or static-destructor fault. It is reported as a warning
  and does not fail the run, but the log survives so it can be investigated.

> **Shard numbers are not stable.** Catch2 distributes test cases across shards
> by position, so adding or removing *any* test reshuffles every shard's
> contents. A failure moving from shard 51 to shard 85 between runs is not
> evidence that your change caused it — the isolation re-run is.

---

## Excluded Tests Breakdown

The default `make test-run` uses filter `~[.] ~[slow]` to exclude tests that would slow down fast iteration. Here's what's excluded:

### Test Count Summary

| Category | Count | Notes |
|----------|------:|-------|
| **Test files** | 627 | All in `tests/unit/` |
| **TEST_CASE macros** | thousands | Individual test definitions |
| **SECTION blocks** | thousands | Subsections within test cases |
| **Slow tests** `[slow]` | ~200 | Excluded from `test-run` |
| **Hidden tests** `[.]` | dozens | Require explicit invocation |

*Counts drift as the suite grows — regenerate with `grep -rc` if you need exact figures.*

*Note: Some overlap exists between [slow] and [.]*

### Hidden Tests `[.]` (90 tests)

Hidden tests never run automatically. They require explicit invocation, and the
`ui_xml/`-dependent ones must be run **from the repo root**. Full inventory and
per-file coverage notes: `HIDDEN_TESTS_TRACKER.md`.

| Category | Count | Purpose |
|----------|------:|---------|
| `[.xml_required]` | 41 | Panel subject-binding tests needing XML components |
| `[.ui_integration]` | 17 | Real widget tree built from `ui_xml/` |
| `[.disabled]` | 11 | Known broken (macOS WiFi Location permission) |
| `[.]` (generic) | 9 | Destructive global state, event-loop concurrency |
| `[.skip]` | 7 | Superseded `ams_slot` binding tests |
| `[.slow]` / `[.benchmark]` / `[.memprobe]` / `[.integration]` | 5 | Stress, timing, memory probe |

### Slow Tests `[slow]` (~185 tests)

Slow tests are excluded from `test-run` but can be run with `make test-slow`.

| File | Count | Why Slow |
|------|------:|----------|
| `test_print_history_api.cpp` | 18 | History database operations |
| `test_moonraker_client_subscription_cancel.cpp` | 17 | WebSocket event loops |
| `test_moonraker_client_security.cpp` | 14 | Security test fixtures |
| `test_moonraker_client_robustness.cpp` | 14 | Concurrent access tests |
| `test_notification_history.cpp` | 13 | History/persistence |
| `test_moonraker_mock_behavior.cpp` | 12 | Mock client simulation |
| `test_gcode_streaming_controller.cpp` | 12 | Layer processing loops |
| `test_moonraker_events.cpp` | 11 | Event dispatch timing |
| `test_printer_hardware.cpp` | 10 | Hardware detection |
| `test_spoolman.cpp` | 9 | Spoolman API calls |
| Other (16 files) | ~55 | Various timing/network tests |

**When to add `[slow]`:**
- Test creates `hv::EventLoop` (network operations) - also add `[eventloop]`
- Test uses `std::this_thread::sleep_for()` for timing
- Test uses fixtures with network clients (e.g., `MoonrakerClientSecurityFixture`)
- Test takes >500ms to complete

**When to add `[eventloop]`:**
- Test creates `hv::EventLoop` for WebSocket operations
- Test requires real network connection/disconnection cycles
- ALWAYS add `[slow]` alongside `[eventloop]` - eventloop tests are inherently slow

### Disabled Tests (#if 0)

These tests are completely disabled due to known issues:

| File | Line | Reason |
|------|------|--------|
| `test_moonraker_client_robustness.cpp` | 555 | `send_jsonrpc` returns -1 instead of 0 when disconnected |
| `test_moonraker_client_security.cpp` | 690 | Segmentation fault (object lifetime issues) |

### Running Excluded Tests

```bash
# Run slow tests only
make test-slow

# Run all tests (slow + fast, but not hidden)
make test-all

# Run specific hidden tests
./build/bin/helix-tests "[.][application][integration]"

# List all hidden tests
./build/bin/helix-tests "[.]" --list-tests

# List all slow tests
./build/bin/helix-tests "[slow]" --list-tests
```

---

## Test Timing Categories

Tests fall into three timing categories based on their execution characteristics. Understanding these helps plan CI/CD pipelines and local development workflows.

### Fast Tests (~2,000+ test cases, ~27s parallel)

The majority of tests complete quickly and are suitable for rapid iteration during development.

```bash
make test-run    # Default: runs fast tests in parallel shards
```

**Characteristics:**
- No network operations or event loops
- Pure logic, parsing, state management
- Typical test: <100ms

### Slow Non-EventLoop Tests (~52 tests)

Tests marked `[slow]` that do NOT use `hv::EventLoop`. These are slow due to deliberate delays, database operations, or simulation work.

```bash
make test-slow   # Run only [slow] tagged tests
```

**Why slow:**
- `std::this_thread::sleep_for()` for timing tests
- Database/history operations (SQLite)
- Mock print simulation with phase transitions

| File | Count | Reason |
|------|------:|--------|
| `test_print_history_api.cpp` | 18 | SQLite operations |
| `test_notification_history.cpp` | 13 | History persistence |
| `test_moonraker_mock_behavior.cpp` | 12 | Mock simulation delays |
| `test_gcode_streaming_controller.cpp` | 12 | Layer processing |

### EventLoop Tests (~54 tests, 5-10 min total)

Tests using `hv::EventLoop` for real network operations. These are the slowest tests and are tagged with BOTH `[eventloop]` AND `[slow]`.

```bash
# Run eventloop tests specifically
./build/bin/helix-tests "[eventloop]" "~[.]"

# These are already excluded by make test-run (via ~[slow])
```

**Why very slow:**
- Real WebSocket connection/disconnection cycles
- Network timeout waiting (1-5 seconds per test)
- Event loop startup/shutdown overhead
- Thread synchronization

| File | Count | Tests |
|------|------:|-------|
| `test_moonraker_client_subscription_cancel.cpp` | 17 | Subscription lifecycle |
| `test_moonraker_client_robustness.cpp` | 14 | Edge cases, concurrent access |
| `test_moonraker_client_security.cpp` | 14 | Security validation |
| `test_print_preparation_manager.cpp` | 6 | Print preparation retry |
| `test_moonraker_api_security.cpp` | 2 | API lifecycle |
| `test_moonraker_connection_retry.cpp` | 1 | Connection retry logic |

**Important:** All `[eventloop]` tests MUST also be tagged `[slow]` to ensure they are excluded from `make test-run`.

### Test Execution Matrix

| Command | Fast | Slow (non-eventloop) | EventLoop | Hidden |
|---------|:----:|:--------------------:|:---------:|:------:|
| `make test-run` | Yes | No | No | No |
| `make test-fast` | Yes | No | No | No |
| `make test-slow` | No | Yes | Yes | No |
| `make test-all` | Yes | Yes | Yes | No |
| `[eventloop]` | No | No | Yes | No |

---

## Test Organization

```
tests/
├── catch_amalgamated.hpp/.cpp  # Catch2 v3 amalgamated
├── test_main.cpp               # Test runner entry
├── ui_test_utils.h/.cpp        # UI testing utilities
├── unit/                       # Unit tests (real LVGL)
│   ├── test_config.cpp
│   ├── test_gcode_parser.cpp
│   └── ...
├── integration/                # Integration tests (mocks)
│   └── test_mock_example.cpp
└── mocks/                      # Mock implementations
    ├── mock_websocket_server.{h,cpp}
    ├── mock_mdns_discovery.h
    └── mock_printer_state.h

experimental/src/              # Standalone test binaries
```

---

## helix-xml Engine Tests (separate suite)

Engine-level XML coverage does **not** live in `tests/`. `lib/helix-xml/` is our own MIT
fork of the XML engine and its own repo
([prestonbrown/helix-xml](https://github.com/prestonbrown/helix-xml)), so it carries a
standalone CMake + Unity suite under `lib/helix-xml/tests/`. `make test` never builds it -
`helix-tests` only reaches the engine through the app, so a submodule pointer bump that
regresses the parser is invisible to every Catch2 gate here.

The suite builds the engine against a **pinned upstream LVGL v9.5.0** pulled by CMake
`FetchContent`, not against our patched `lib/lvgl`. That is the point: the engine is a
library, and its tests must not depend on the consuming application.

### Running it

From this repo:

```bash
make test-xml                                        # configure + build + ctest
make test-xml HELIX_XML_CTEST_ARGS='-R test_expr'    # one executable
```

The build tree is `build/helix-xml-tests/`, outside the submodule. The **first** configure
clones LVGL (needs network, several minutes); every run after that is a no-op configure plus
a couple of seconds of ctest. `make test-xml` reports the real Unity case count rather than
ctest's executable count.

From a bare clone of the submodule, with no HelixScreen around it:

```bash
cmake -S tests -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Offline, point it at an LVGL checkout you already have:

```bash
cmake -S tests -B build -DLVGL_DIR=/path/to/lvgl
```

`LVGL_DIR` must be a **pristine** upstream checkout. Aiming it at HelixScreen's `lib/lvgl`
fails at configure time with an explanation: our `patches/*.patch` inject calls to app-side
symbols (`helix_crash_note_*`) that a standalone build cannot link. Do not unpatch the
submodule to work around it - the application needs those patches.

### Gates

`scripts/quality-checks.sh` runs the suite, but only when staged changes touch
`lib/helix-xml/` - in practice the pointer bump itself, which is exactly the change that
alters what the suite tests. It **never configures**: if build/helix-xml-tests/CMakeCache.txt
is missing it skips with an instruction to run `make test-xml` once by hand, so a multi-minute
LVGL fetch never fires from a commit hook. Once the build tree exists, a failing suite is a
hard failure like any other test gate.

The submodule has its own CI too (`.github/workflows/ci.yml` **inside** `lib/helix-xml/`, not
this repo's): a gcc + clang matrix, an ASAN/UBSAN job, and a conf-guards job that
compile-checks `LV_USE_XML=0`, `LV_USE_TRANSLATION=0` and `LV_USE_OBJ_NAME=0` to prove the
library's `#if` guards hold.

### What goes where

| Here, in Catch2 | There, in Unity |
|-----------------|-----------------|
| HelixScreen tooling built *around* XML: the hot reloader, the attribute validator, the card hit-test sweep | Parser, loader, component and widget registries, `<if>`/`<else>`, `<repeat>`, `subject_expr`, translation, styles, malformed input |
| Widget contracts our XML depends on: semantic-widget text forwarding, borrowed-subject snapshot/restore | Anything that would still be true with HelixScreen deleted |

Seventeen engine-level `tests/unit/test_xml_*.cpp` files migrated into the submodule and were
deleted here. What stayed: `test_xml_hot_reloader.cpp`, `test_xml_attribute_validator.cpp` and
`test_xml_card_hittest_sweep.cpp` (HelixScreen tooling, not the engine), plus two preserved
cases extracted into `test_ui_text_inline_content.cpp` (semantic-widget text forwarding) and
`test_xml_hot_reload_borrowed_subjects.cpp` (`snapshot_borrowed_subjects` /
`restore_borrowed_subjects`).

### Adding a test to the submodule suite

Drop a `.c` file in `lib/helix-xml/tests/cases/`. A CMake glob picks it up and it becomes its
own executable and its own ctest entry - no `CMakeLists.txt` edit (21 executables today).
Fixtures go in `tests/assets/`, reached through the `HELIX_TEST_ASSET_DIR` define rather than
the shell's cwd. Shared setup lives in `tests/helpers/` (`helix_test_env.h`,
`helix_test_pump.h`, `helix_log_capture.h`) and assertions in `helpers/xml_assert.h`. Test
code is compiled `-Wall -Wextra`; only the vendored engine and Unity sources get `-w`.

**Assertions are structural only.** There is no `ASSERT_WIDTH`, `ASSERT_POS` or
`ASSERT_TEXT_WIDTH` in `xml_assert.h`, and none may be added: `tests/lv_conf.h` picks a color
depth, default font, widget set and theme that no real device runs, so a geometry assertion
would encode the test config instead of the engine's behavior. Assert on tree shape, names,
child counts, label text, flags, states, and style properties the XML under test declared. To
prove a layout behavior, assert the property the XML set (`lv_obj_get_style_flex_flow`),
never the pixels that came out of it. There is no screenshot or pixel comparison anywhere in
the suite.

Remember the inverted submodule workflow: edit in place, commit and push **inside**
`lib/helix-xml/`, then commit the bumped pointer here. Never write a `patches/*.patch` for it.
See `HELIX_XML_FORK.md`.

---

## Writing Tests

### Test Fixtures

`HelixTestFixture` (`tests/helix_test_fixture.h`) is the base for every test fixture. Its ctor and dtor call `reset_all()` which drains the update queue, resets `SystemSettingsManager` language, and clears the modal stack. Use `TEST_CASE_METHOD(HelixTestFixture, ...)` for plain unit tests that mutate process-wide singletons so mutations don't leak to the next test.

`LVGLTestFixture` (`tests/lvgl_test_fixture.h`) inherits `HelixTestFixture` and adds a headless DRM display + test screen. Use it for tests that touch LVGL widgets.

`XMLTestFixture` (`tests/test_fixtures.h`) inherits `LVGLTestFixture` and owns per-instance `PrinterState`, `MoonrakerClient`, and `MoonrakerAPI` — no shared static state between tests. Reach for it whenever you need to exercise XML bindings. XML subjects register into LVGL's global scope; each test's `init_subjects(true)` overwrites prior entries with fresh pointers, and the destructor tears the screen down before deinitializing subjects to avoid dangling observer references.

**Driving print state in a test:** use `tests/test_helpers/print_state_test_drivers.h`, never a hand-written `print_state_enum` value. Consumers gate on the derived `print_lifecycle` subject, which is republished only inside `PrinterState::update_from_status()`; writing the raw enum subject by hand leaves the lifecycle stale, lifecycle consumers never re-gate, and the assertion fails as though the production guard were missing. `set_wire_state()` drives the real input path; `lifecycle_from_bools()` adapts suites that feed bool pairs. Hand-written enum writes are what broke ~90 assertions when the lifecycle migration landed.

### Catch2 v3 Basics

```cpp
#include "your_module.h"
#include "../catch_amalgamated.hpp"

using Catch::Approx;

TEST_CASE("Component - Feature", "[component][feature]") {
    SECTION("Scenario one") {
        REQUIRE(result == expected);
    }
    SECTION("Scenario two") {
        REQUIRE(value == Approx(3.14).epsilon(0.01));
    }
}
```

**Assertions:** `REQUIRE()` (stops on failure), `CHECK()` (continues), `REQUIRE_FALSE()`

**Skipping:** `if (!condition) { SKIP("Reason"); }`

**Logging:** `INFO("Parsed " << count << " items");`

### Adding New Tests

1. Create file in `tests/unit/test_<module>.cpp`
2. **Always add a feature tag** - What functional area?
3. **Add `[core]` if critical** - Would the app break without this?
4. **Add `[slow]` if >500ms** - Keeps fast iteration fast

```cpp
// Good: Feature + importance
TEST_CASE("PrinterState observer cleanup", "[core][state]")

// Good: Feature + speed
TEST_CASE("Connection retry 5s timeout", "[connection][slow]")

// Bad: No feature context
TEST_CASE("Some test", "[unit]")
```

The Makefile auto-discovers test files in `tests/unit/` and `tests/integration/`.

---

## Mocking Infrastructure

### MoonrakerClientMock

```cpp
#include "moonraker_client_mock.h"

MoonrakerClientMock client;
client.connect(url, on_connected, on_disconnected);
client.trigger_connected();   // Fire callback
client.get_rpc_methods();     // Verify calls made
client.reset();               // Reset for next test
```

### Available Mocks

- **MoonrakerClientMock:** WebSocket simulation (`include/moonraker_client_mock.h`)
- **mock_websocket_server** (`tests/mocks/mock_websocket_server.{h,cpp}`): WebSocket server stub
- **mock_mdns_discovery** / **mock_printer_state** (`tests/mocks/`): discovery and printer-state stubs

### Mock Drift Protection

Six mock boundaries are guarded at build time by `[compile][drift]` tests in `tests/unit/test_interface_drift_*.cpp`. Each test `static_assert`s that the mock derives from the corresponding interface and is not abstract — so adding a pure virtual to an interface without updating the mock (directly or via the concrete class it inherits from) fails the build.

Covered: `AmsBackend`, `EthernetBackend`, `UsbBackend`, `WifiBackend` (already pure-virtual interfaces), plus `IMoonrakerAPI` and `helix::IMoonrakerClient` (narrow interfaces added Apr 2026 — see `include/i_moonraker_api.h`, `include/i_moonraker_client.h`). The Moonraker mocks still inherit the concrete classes; the interfaces enforce drift protection without requiring a mock rewrite.

---

## UI Testing Utilities

The real API is the `UITest::` namespace in `tests/ui_test_utils.h`:

```cpp
#include "ui_test_utils.h"

UITest::init(screen);                      // Set up the test indev on a screen
lv_obj_t* w = UITest::find_by_name(root, "my_button");
UITest::click(w);                          // Simulate a click/touch on a widget
UITest::click_at(x, y);                    // Or at explicit coordinates
UITest::type_text(textarea, "hello");
UITest::wait_until([]{ return done; });    // Pump timers until a condition
UITest::cleanup();
```

See UI_TESTING.md for the full utility list.

---

## Gotchas

### LVGL Observer Auto-Notification

`lv_subject_add_observer()` immediately fires the callback with current value:

```cpp
lv_subject_add_observer(subject, callback, &count);
REQUIRE(count == 1);  // Fired immediately!

state.set_value(new_value);
REQUIRE(count == 2);  // Fired again on change
```

### Hidden Tests Hang

Always use `"~[.]"` when running by tag:

```bash
# ✅ Correct
./build/bin/helix-tests "[application]" "~[.]"

# ❌ May hang on hidden tests
./build/bin/helix-tests "[application]"
```

### Common Issues

| Issue | Solution |
|-------|----------|
| Catch2 header not found | Use `#include "../catch_amalgamated.hpp"` |
| Approx not found | Add `using Catch::Approx;` |
| Test won't link | Check .o files in Makefile test link command |
| LVGL undefined in integration | Use mocks, not real LVGL |

---

## Debugging

```bash
# Run specific test case
./build/bin/helix-tests "Test case name"

# List all tests matching tag
./build/bin/helix-tests --list-tests "[connection]"

# Verbose output
./build/bin/helix-tests -s -v high

# In debugger
lldb build/bin/helix-tests
(lldb) run "[gcode]"
```

---

## Related Documentation

- **[ARCHITECTURE.md](ARCHITECTURE.md):** Thread safety patterns
- **[BUILD_SYSTEM.md](BUILD_SYSTEM.md):** Build configuration
- **[HELIX_XML_FORK.md](HELIX_XML_FORK.md):** Why the XML engine is its own repo, with its own tests and CI
- **[DEVELOPMENT.md#contributing](DEVELOPMENT.md#contributing):** Code standards
