# UI Testing Guide

## Overview

HelixScreen has two UI test layers, covering different things:

- **In-process (this doc, below):** headless LVGL testing with virtual input devices,
  building widgets inside the test binary. Fast, fine-grained, but structurally can't
  reach app lifecycle, navigation, or async population.
- **Out-of-process (`tests/ui/`):** pytest drives a real running `helix-screen` instance
  through `helix-screen ctl`. See "Out-of-Process Tests" below.

**Test Framework:** Catch2 v3.5.1
**Test Utilities:** `tests/ui_test_utils.h/cpp`
**Test Location:** `tests/unit/test_*.cpp`

> Driving the **real binary** headless — bringing up an actual panel and
> screenshotting it with no display server — is a separate path: run
> `helix-screen` under `SDL_VIDEODRIVER=dummy` and control it with
> `helix-screen ctl`. See `HELIXCTL.md` § "Running headless". Use that when a
> test needs the full application lifecycle rather than a fixture-built widget
> tree.

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

## Fixtures & Test Cases

Fixtures and Catch2 test structure are suite-wide concerns and live in
[TESTING.md](TESTING.md) § "Test Fixtures" / "Catch2 v3 Basics" — derive from
`LVGLTestFixture` / `XMLTestFixture` (never hand-roll `lv_init()` +
`lv_display_create()`; see "Known Limitations & Workarounds" § 1 below), and
follow the tag taxonomy in TESTING.md § "Test Tag System" for organizing and
selecting UI tests (always pair a tag run with `"~[.]"` to skip hidden tests
that may hang). The complete test-file template at the end of this doc shows
both applied to a UI test, including the once-per-run XML component
registration.

## Out-of-Process Tests (`tests/ui/`)

**Test Framework:** pytest
**Test Utilities:** `tests/ui/helix/app.py` (`HelixApp`)
**Test Location:** `tests/ui/test_*.py`

Catch2 tests build widgets inside the test binary — real widget code, but no real app
lifecycle. `tests/ui/` instead boots the actual `helix-screen` binary on a private
control socket and drives it through `helix-screen ctl --json` (see `HELIXCTL.md`),
covering things the in-process layer structurally cannot reach: app boot, panel/overlay
navigation, and async population.

`HelixApp` (`tests/ui/helix/app.py`) wraps each `ctl --json` call as a subprocess and
raises on failure:

- `HelixCtlError` — the server rejected the command (unknown widget, bad subject, ...).
  Carries `.message`, `.code`, and `.command` (the full argv, so a failure message names
  exactly what was run).
- `HelixAppError` — the app itself failed to boot or died mid-test. Carries the tail of
  its log.

```python
def test_navigate_to_controls(helix_app):
    helix_app.navigate("controls")
    assert helix_app.current()["panel"] == "controls"
```

Two fixtures in `tests/ui/conftest.py`:

- `helix_app` (session-scoped) — one instance shared by the whole run. Use this by
  default; a boot costs ~2s and most tests don't dirty global state.
- `fresh_helix_app` (function-scoped) — a private instance for a test that does dirty
  global state (e.g. changes a persistent setting).

Run with the repo's venv, not bare `python3` (it lacks pytest):

```bash
make -j                                        # build the binary tests/ui/ drives
make test-ui-pytest                            # full suite (incl. goldens), via .venv
# or directly:
./.venv/bin/python -m pytest tests/ui/ -v
./.venv/bin/python -m pytest tests/ui/ --accept-goldens   # after reviewing a diff
```

`make test-ui-pytest` (not `make test-ui` — that name is already the in-process
Catch2 `[navigation],[theme],[wizard]` convenience target, see `mk/tests.mk`) fails
with a clear message pointing at `make venv-setup` or `make -j` if the venv or the
binary is missing, rather than an opaque pytest error.

`HelixApp` boots the same way `scripts/screenshot.sh` does — `--test --skip-wizard
--skip-splash --remote --remote-socket <private>`. **By default it runs headless**
(`SDL_VIDEODRIVER=dummy`) — verified to render identically for navigate/screenshot
purposes, and necessary since a suite run boots many instances and a visible window
would steal focus on every one. Exporting `SDL_VIDEODRIVER` yourself (e.g. `=wayland`
to watch a run) switches the renderer away from the `dummy` driver every golden was
captured under — a plausible way to turn the golden suite red for reasons that have
nothing to do with the UI change under test. Unset it before running goldens for real.

Two environment variables most tests never need to touch:

- `HELIX_UI_BINARY` — overrides the `helix-screen` binary path (default:
  `build/bin/helix-screen` relative to the repo root). Needed by a test that copies
  `conftest.py` elsewhere (see `test_diagnostics.py`'s pytester sub-run), whose
  `__file__`-relative path resolution no longer finds the real binary once copied.
- `HELIX_UI_ARTIFACTS` — overrides where failure diagnostics land (default:
  `ui-artifacts/`, relative to wherever pytest is invoked from).

Failures write a screenshot, the app's log tail, and a screen-state dump to
`ui-artifacts/<test-name>/` (or `$HELIX_UI_ARTIFACTS/<test-name>/`).

Golden images are **local-only** for now. They are sensitive to renderer and font
rasterization, so a golden captured on a desktop will not match a CI runner; see the
design spec's Risks section.

**Treat the golden corpus as pinned to one environment — Linux, with the toolchain
the goldens were captured under.** The rest of `tests/ui/` (navigation, text,
geometry, reset, wait_idle, screenshot mechanics) is environment-agnostic and runs
anywhere; only the `test_screen_matches_golden[*]` cases carry this constraint.
Measured on macOS: `bed-mesh` differs by a few dozen pixels of text antialiasing,
`macros` by ~8200 in the soft-edged band where the overlay's drop shadow falls —
both identical with an unmodified binary, so they are rasterization, not
regressions. Do not `--accept-goldens` to make those green; it pins the corpus to
whichever machine last ran it and turns the intended environment red instead.

### Golden corpus scope (`tests/ui/test_screens.py`)

The screen list is *sourced from*, not hand-copied from,
`scripts/screenshot-recipes.sh`: the test shells out to
`bash -c 'source scripts/screenshot-recipes.sh; ...'` and walks the table through
that script's own two accessors — `screenshot_recipe_tokens` to enumerate and
`screenshot_recipe_for` to resolve — so a recipe added or renamed there shows up
here without a second edit to keep in sync.

Going through the accessors rather than reading the data variable is deliberate:
this used to expand `${!SCREENSHOT_RECIPE[@]}` directly, which broke the moment
that array did. (It was an associative array; macOS ships bash 3.2, which
supports neither `declare -A` nor `declare -g` and *still exits 0* after
refusing them, so the table read as empty on macOS and the failure surfaced as a
`KeyError` at module scope. `_load_recipes()` now raises with bash's stderr if
the table comes back empty.)

Only 8 of the ~38 known recipe tokens are golden'd so far — deliberately, because
`freeze()` cannot pin down everything a screen might show:

- **Live mock telemetry** (`home`, `controls`, `filament`, `fan`, and any screen that
  leaves the Controls temperature card visible) drifts via the mock backend's
  `simulation_thread_` (`moonraker_client_mock.cpp`) — a raw background thread, not
  an LVGL timer, so it's invisible to both `freeze()` and `wait_idle()`.
- **Wall-clock content** (`console`'s gcode log timestamps, `filament`'s usage-chart
  x-axis) is never the same twice by construction.
- **Free-running spinners** (`camera`'s "Connecting Camera..." indicator) animate via
  `lv_anim` independent of `settings_animations_enabled`, so `freeze()` catches an
  arbitrary arc position — the same category of issue the design spec calls out for
  the print-select loading spinner.
- **Modal backdrops** (`preflight-check`) can inherit jitter faintly through the dim
  scrim over a jittery panel underneath.

Each of these was confirmed empirically (byte-identical captures compared across
independent app boots, not just within one `capture(stable=True)` call) before being
excluded — see the task-10 report for the evidence. Adding one of them later needs
either a mock-side way to pin the drifting value, or accepting a masked/cropped
comparison region; don't just re-add the token and hope.

Every golden'd screen also depends on `settings_animations_enabled` being off, or
`NavigationManager`'s overlay slide+fade can still be mid-flight when `freeze()`
runs, locking in a half-transitioned frame. This used to be a real bug: each
`HelixApp` boots with its own private `HELIX_CONFIG_DIR` (to avoid lock-file
collisions between instances), which bypassed `config/settings-test.json` entirely
and fell back to the platform-capability default instead (`true` on native/desktop —
confirmed via boot log: "animations=true"). `HelixApp.start()` (`helix/app.py`) now
seeds each private config dir with `config/settings-test.json` before boot, so every
instance gets the intended test defaults *and* lock isolation — no per-test
workaround needed. If a future screen animates unexpectedly, check whether that
seeding step is still wired up before adding a local fixture to paper over it again.

### CI coverage: what runs, what doesn't, and why

`.github/workflows/build.yml`'s `test-ubuntu` job runs the out-of-process suite
**with the 8 golden-image tests excluded**
(`pytest tests/ui/ -v --ignore=tests/ui/test_screens.py`) after the existing
in-process Catch2 run, using `actions/setup-python` + `pip install pytest` +
`requirements.txt` — not `make test-ui-pytest`, since that target's `.venv` check
exists for a local dev who forgot `make venv-setup`, which doesn't apply to a
runner provisioned by `actions/setup-python`.

The 38 excluded-golden tests assert on behavior (navigation, `wait_idle`, text
reading, capture mechanics, screen state, reset semantics) and are portable across
machines. Verified they have zero coupling to the golden files themselves: they
pass with `tests/ui/goldens/` removed entirely, not just with `test_screens.py`
skipped. The golden tests compare pixels, and cross-machine font rasterization is
exactly the golden-portability risk called out above and in the design spec's
Risks section — they stay a **local-only** gate until someone deliberately captures
them inside a fixed container with pinned fonts. If a future contributor sees 8
tests missing from a CI run and "fixes" it by dropping `--ignore`, that trades a
green build for pixel diffs unrelated to whatever change actually broke it — don't.

**Relationship to `scripts/smoke-headless.sh`** (also run in `compile-ubuntu`):
mostly, but not entirely, subsumed. The out-of-process suite exceeds the smoke
test's boot/navigate/screenshot checks in every dimension (many more panels, golden
comparisons, `wait_idle`, text reading, widget geometry) — but the smoke test does
one thing the suite's teardown doesn't replicate: it explicitly inspects the exit
status after a clean shutdown request and fails on `139`/`134` (SIGSEGV/SIGABRT) or
a crash signature in the log. `HelixApp.stop()` waits for the process to exit but
never checks *how* it exited, so a segfault during shutdown cleanup would currently
go unnoticed by the pytest teardown path. Not acted on here — flagging it as a real,
narrow gap rather than a reason to drop the smoke test.

## Known Limitations & Workarounds

### 1. Use the Base Fixtures for Cross-Test Isolation

**Problem:** Hand-rolling `lv_init()`/`lv_display_create()`/`lv_display_delete()` in a fixture bypasses the drain-and-reset that keeps LVGL state clean between tests, and multiple such instances in sequence can crash on stale observers or subjects.

**Fix:** Derive from the mandated base fixtures instead of managing LVGL yourself:
- `HelixTestFixture` (`tests/helix_test_fixture.h`) — ctor + dtor call `reset_all()` (drains `UpdateQueue`, clears `ModalStack`, resets language).
- `LVGLTestFixture` — inherits `HelixTestFixture` and manages the LVGL display/indev lifecycle.
- `XMLTestFixture` — owns per-instance `PrinterState` / `MoonrakerClient` / `MoonrakerAPI` and refreshes subjects via `init_subjects(true)`.

These handle the cleanup that a bespoke fixture forgets; a bespoke fixture is the usual cause of a "first test passes, second segfaults" pattern.

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
lv_obj_send_event(widget, LV_EVENT_VALUE_CHANGED, nullptr);
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
./build/bin/helix-tests --success

# Show all output
./build/bin/helix-tests -s
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
- **Example Test Files:** `tests/unit/test_wizard_connection_ui.cpp`, `tests/unit/test_ui_panel_bindings.cpp`
- **Catch2 Documentation:** https://github.com/catchorg/Catch2
- **LVGL Testing Guide:** `LVGL9_XML_GUIDE.md`
