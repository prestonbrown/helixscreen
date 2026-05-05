# Testing Patterns Reference

## Test Fixture Hierarchy

| Fixture | Base | Provides |
|---------|------|----------|
| `HelixTestFixture` | — | Drains UpdateQueue, resets SystemSettingsManager, clears modal stack |
| `LVGLTestFixture` | `HelixTestFixture` | Adds headless DRM display + test screen |
| `XMLTestFixture` | `LVGLTestFixture` | Per-instance PrinterState/MoonrakerClient/API, XML subject registration |

- `HelixTestFixture` → `tests/helix_test_fixture.h` — for unit tests that mutate singletons
- `LVGLTestFixture` → `tests/lvgl_test_fixture.h` — for tests that touch LVGL widgets
- `XMLTestFixture` → `tests/test_fixtures.h` — for tests exercising XML bindings

`HelixTestFixture` ctor/dtor calls `reset_all()` which drains the update queue. This
prevents queued callbacks from leaking between tests.

### XMLTestFixture Specifics

XML subjects register into LVGL's global scope. Each test's `init_subjects(true)` overwrites
prior entries with fresh pointers. The destructor tears the screen down BEFORE deinitializing
subjects to avoid dangling observer references.

## UITest Utilities

`tests/ui_test_utils.h` provides headless testing with virtual input:

```cpp
UITest::init(screen);          // After creating test screen
UITest::cleanup();             // In destructor, BEFORE deleting screen

UITest::click(widget);         // Click at center
UITest::click_at(x, y);       // Click at coordinates
UITest::type_text("hello");   // Type into focused textarea
UITest::wait_ms(500);          // Fixed delay (processes LVGL tasks every 5ms)
UITest::wait_until([&]() {     // Poll condition every 10ms, 5s timeout
    return state_changed;
}, 5000);
UITest::wait_for_visible(w, 3000);
UITest::find_by_name(parent, "name");  // Recursive search
UITest::is_visible(widget);
UITest::get_text(widget);
UITest::is_checked(widget);
```

## Test Cleanup Order (Critical for Stability)

Cleanup in **reverse creation order**. Always drain the queue before destroying widgets:

```cpp
~MyFixture() {
    UITest::cleanup();                    // First
    if (panel) lv_obj_delete(panel);      // Then widgets
    if (screen) lv_obj_delete(screen);
    if (display) lv_display_delete(display);
}
```

### Multiple Fixture Crash

Creating multiple LVGL UI instances in sequence causes segfaults if cleanup is incomplete.
Each test must fully clean up its LVGL objects, subjects, and observers before the next
test's fixture runs. `HelixTestFixture::reset_all()` handles the queue drain.

## Observer Testing Gotcha

`lv_subject_add_observer()` fires the callback **immediately** with the current value:

```cpp
lv_subject_add_observer(subject, callback, &count);
REQUIRE(count == 1);  // Fired immediately!

state.set_value(new_value);
REQUIRE(count == 2);  // Fired again on change
```

## Running Tests

```bash
make test-run           # Parallel, excludes [slow] and hidden
make test-serial        # Sequential (debugging)
make test-asan          # AddressSanitizer (UAF, leaks, overflows)
make test-tsan          # ThreadSanitizer (data races, deadlocks)
```

Always use `"~[.]"` when running by tag to exclude hidden tests that may hang:
```bash
./build/bin/helix-tests "[connection]" "~[.]"
```

## Relevant Tags

- `[state]` — PrinterState singleton, LVGL subjects, observers
- `[connection]` — WebSocket connection lifecycle, retry logic
- `[application]` — Application lifecycle, shutdown
- `[core]` — Critical tests (12 must-pass)
- `[slow]` — Tests >500ms, excluded from test-run
