# tests/CLAUDE.md — Writing HelixScreen Tests

## Layout

| Path | Contents |
|------|----------|
| `tests/unit/` | Catch2 unit tests. **Auto-globbed** (`Makefile:904`) — drop a `.cpp` in and it builds |
| `tests/unit/application/` | App-lifecycle tests (separate glob, own include path) |
| `tests/mocks/` | Mock implementations |
| `tests/shell/` | `bats` tests — installer, lint gates, packaging |
| `tests/ui/` | Out-of-process pytest driving the real binary via `helix-screen ctl` |
| `tests/test_helpers/` | `*TestAccess` friend classes for reaching private members |

```bash
make test                      # build tests only (does NOT run them)
make test-run                  # build AND run in parallel
./build/bin/helix-tests "[tag]"  # run one tag
```

`make -j` builds **only** the app binary. Run `make test` before `./build/bin/helix-tests`
or you are testing a stale binary.

---

## Fixtures — inherit, don't hand-roll

```
HelixTestFixture          drains UpdateQueue, resets language, clears ModalStack (ctor + dtor)
└── LVGLTestFixture       + LVGL init, headless display, UpdateQueue init/shutdown
    └── LVGLUITestFixture + XML component registration
XMLTestFixture            per-instance PrinterState / MoonrakerClient / MoonrakerAPI
```

**Always derive from one of these.** A hand-rolled fixture that calls `lv_init_safe()` and
`lv_display_create()` looks sufficient and is not — it silently skips the UpdateQueue
lifecycle, and anything that defers work through `ui_queue_update()` **never runs**. There is
no error; the code under test simply does nothing and the test fails on a value that looks
like a logic bug.

That is the trap: `NavigationManager::push_overlay()` queues its entire body through
`UpdateQueue`. With an uninitialised queue, `push_overlay()` + `drain()` is a no-op pair, so a
width/lifecycle assertion fails as if the production logic were wrong.

```cpp
class MyFixture : public LVGLTestFixture {   // ← not a bare class
  public:
    MyFixture()  { /* your setup */ }
    ~MyFixture() override { helix::ui::UpdateQueue::instance().drain(); }
};
```

### Deferred work needs an explicit drain

Anything routed through `ui_queue_update()` / `tok.defer()` runs on the next queue tick, not
at call time. Drain before asserting:

```cpp
nav.push_overlay(widget);
helix::ui::UpdateQueue::instance().drain();
```

`process_lvgl(ms)` (on `LVGLTestFixture`) additionally pumps timers and animations.

### `process_lvgl()` moves *virtual* time — it is not a wall-clock wait

The test binary builds its display with a bare `lv_display_create()`. No driver, so nothing
ever calls `lv_tick_set_cb()` and `lv_tick_get()` returns exactly what `lv_tick_inc()` has
been fed. `process_lvgl()` is the thing feeding it, and it barely sleeps: 1ms of real time per
5ms step, and **zero** below `ms <= 50`, which skips the sleep entirely.

So a loop that counts nominal milliseconds toward a timeout is not waiting for anything:

```cpp
int waited = 0;
while (!done && waited < 120000) { process_lvgl(100); waited += 100; }   // ← 24s, at best
```

That burns a two-minute budget in seconds and never meaningfully yields to the thread it is
watching. The symptom is baffling rather than obvious: the test runs on past the assertion,
the fixture destructor tears down the screen, and teardown effects appear to happen mid-test.

Two correct answers, in order of preference:

1. **Join the worker, then drain.** Deterministic, no timeout, no flake under CI load.
   `ActivePrintMediaAsyncFixture::drain()` in `test_active_print_media_manager.cpp` is the
   model — `ThumbnailProcessor::wait_for_completion()` then drain, repeated, since a drained
   callback can commit more pool work.
2. **`wait_until(pred, timeout_ms)`** on `LVGLTestFixture` when there is no joinable handle.
   Real `steady_clock` deadline, real sleeps, and it advances the tick each pass so timers and
   animations still come due.

The frozen-clock trap has a mirror image, which is why `wait_until` lives on the fixture: a
wait that sleeps on the real clock *without* `lv_tick_inc()` leaves LVGL's clock stopped.
`lv_async_call` one-shots (period 0) still fire, but no timer with a real period ever comes
due, however long you wait.

---

## LVGL traps in tests

**`lv_obj_get_width()` reads the COMPUTED coord, not what you just set.**
`lv_obj_set_width()` writes a style; the computed value only refreshes on a layout pass. A
freshly created widget reports LVGL's default (160) no matter what you set. Force it:

```cpp
lv_obj_set_width(w, 900);
lv_obj_update_layout(w);          // ← without this, get_width() still says 160
CHECK(lv_obj_get_width(w) == 900);
```

Same applies to heights, content sizes, and anything measured off a flex/grid parent.

**Seed `NavigationManager` the way the app does.** `panel_stack_[0]` always holds the active
root panel in production, and overlay code reads `panel_stack_.back()` to find what is
beneath it. If you only ever `push_overlay()`, the *second* push still looks like the first:

```cpp
std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
for (auto& p : panels) p = lv_obj_create(lv_screen_active());
NavigationManager::instance().set_panels(panels.data());   // seeds panel_stack_[0]
NavigationManager::instance().set_active(PanelId::Advanced);
```

**Overlays must be registered before push.** `register_overlay_instance(widget, lifecycle)` —
or `(widget, nullptr)` for an intentional lifecycle-less overlay. Tests run with
`HELIX_STRICT_OVERLAY_CHECK`, so an unregistered push **aborts** rather than warning.

---

## What counts as a real test

A test must FAIL if the feature is removed. After writing one, **mutate the implementation and
watch it go red** — a test that passes against broken code is worse than no test. `make
mutate-diff` does this across the whole diff; see "Proving a test can fail" below.

| ❌ | ✅ |
|----|----|
| `REQUIRE(true)`, `REQUIRE(ptr != nullptr)` and nothing else | assert the value the feature computes |
| Happy path only | edge cases, error paths, both sides of a branch |
| Asserting a constant you also hardcoded in the test | derive the expectation independently |

Prefer extracting the rule as a **pure function** and testing that without LVGL — see
`include/overlay_class.h` + `tests/unit/test_overlay_width_class.cpp`. Then test the wiring
separately (`test_overlay_width_push.cpp`). Pure-logic tests are fast, total, and survive
refactors of the widget layer.

### Proving a test can fail

"A test must FAIL if the feature is removed" is the rule. It was stated as a
principle with no mechanism, and the result was 11 production changes in one
release range that can be reverted with the suite staying green. Every one
shipped with a test commit. The tests pin an *adjacent invariant* instead of the
changed line -- they have real assertions, execute the changed function, and
still cannot fail when it breaks.

Syntax cannot find these. Measured on this tree, 18% of cases look "all-weak" by
assertion shape and the worst-looking files are all good tests. Four tools, in
increasing cost, and only the last is an oracle:

| Command | Cost | Answers |
|---------|------|---------|
| `make check-tautology` | instant | assertions that cannot fail, from the source |
| `make test-vacuous` | seconds | assertions that cannot fail, from a real run |
| `make cov-diff` | minutes | changed lines the suite never executes |
| `make test-order-dependence` | minutes | tests that pass only because of what ran before them |
| `make mutate-diff` | ~2-4 min per hunk | **changed lines no test detects** |

`make mutate-diff` reverts each changed hunk in turn, refreshes whatever the
suite reads, and runs it. A hunk that survives reversion is a change no test
detects, whatever the diff's test files claim. It is the only tool here that sees
the adjacent-invariant failure, so scope it (`MUTATE_ARGS="--limit 5"`, or
`--tests "[ams]"`) rather than skipping it.

Four outcomes, and they are not interchangeable:

| Verdict | Means |
|---------|-------|
| `killed` | a test detects the change — the outcome you want |
| `SURVIVED` | the mutant ran and the suite stayed green: NO test detects it |
| `uncompilable` / `unreversible` | a mutant was attempted but no test ever judged it. Never a kill: a compiler error proves the code is load-bearing for the build, not that anything tests its behaviour |
| `NOT COVERED` | nothing here can mutate that file, so the gate did not look at it |

The run answers `CLEAN` only when every changed hunk was mutated and every mutant
died. Anything short of that is `INCOMPLETE` (exit 3) and names the paths it did
not examine, because a clean answer about a file the tool never opened is
precisely what a commit body would go on to cite as proof.

What it mutates, and what pays for what:

| Path | Refresh per mutant | Suite |
|------|--------------------|-------|
| `src/`, `include/` | `make test-build` | `helix-tests` |
| `ui_xml/*.xml`, `assets/config/*.json` | none — the binary reads these off the tree at run time | `helix-tests` |
| `scripts/*.py`, `scripts/*.sh` | none | `bats tests/shell` + `pytest tests/python` |

Everything else is `NOT COVERED`, with the reason printed. Two are worth knowing:
`tests/` itself, because a test is proven by mutating the code it pins rather
than by reverting itself; and anything under `lib/`, because a superproject diff
carries only a submodule's pointer and never its content. Clear one by mutating
it by hand and naming the result in the commit body, or accept it with
`--allow-incomplete`.

`make cov-diff` is the cheap screen: a changed line the suite never runs cannot
be tested, and finding that costs one run instead of one build per hunk. The
converse does not hold, so a clean coverage report is not a substitute.

### A test must pass on its own

`make test-order-dependence` re-runs each source file's cases alone and
compares against the full-suite result. A case that passes in the suite and
fails alone is reading state some other file's test established.

This is the one class none of the other tools can see. Such a test asserts real
computed values, its lines are covered, and reverting the production hunk would
report it killed -- it simply is not testing what it claims. It stays invisible
until an unrelated change perturbs ordering, which is the worst moment to find
it: adding nine test cases elsewhere changed the case count, which changed shard
composition, which moved one test's accidental prerequisite into another shard,
and a 96/96 green suite went red with nothing wrong in the changed code.

The specimen it was built against is `test_grid_edit_mode.cpp` "build_default_grid
only sets positions for anchor widgets", which passes in the suite and fails 5/5
alone. The gate also reports the opposite sign as `pollution` (fails in the
suite, passes alone) -- same root cause, different fix: the polluter needs
cleanup, the dependent needs its own setup.

### Assert that the setup reached the branch, first

An assertion about a result is worthless if the code never took the path that
produces it. Assert the precondition **before** asserting the outcome:

```cpp
const size_t emergency_budget = dense.get_cache_budget() / 2;
const size_t before = dense.get_cache_memory_usage();
REQUIRE(before > emergency_budget);      // <-- the setup actually overshoots
dense.respond_to_memory_pressure();
CHECK(dense.get_cache_memory_usage() < before);
```

Without the middle line this test passes with `respond_to_memory_pressure()`
emptied out. The fixture cached a few hundred bytes against a 1MB floor, so the
evict loop's exit condition was already true on entry and nothing could ever
change. Worse, the obvious repair is *also* vacuous: `CHECK(after <= before)`
cannot fail either, for exactly the same reason.

That case is the one to keep in mind, because it defeats three of the four
tools above. `make test-vacuous` sees a real assertion (Catch2 expands it to
`1024 <= 1024`, which differs from the source text). `make cov-diff` is green,
because the changed line does execute. `-Werror=type-limits` only ever caught
the version written as `>= 0` on a `size_t` -- fixing the compiler-visible
tautology moved it to a semantic one, which hides better. Only reverting the
hunk and watching for red finds it.

### Name the mutation in the commit body

Red-green is invisible after the fact. A test that was mutated and verified is
indistinguishable from one nobody checked, which is why the discipline decays.
One line in the commit body fixes that:

```
mutation: flipped >= to > in resolve_slot(); test_ams_topology went red
```

Or, for a whole hunk: `mutation: reverted the guard; test_print_start went red`.
It costs a sentence and it is the only record that the cycle happened.

### Tag conventions

`[core]`, `[navigation]`, `[ams]`, `[threading]`, `[compile][drift]`, `[slow]`, and a bare
issue number for bug-fix tests (`[1178]`) so the whole fix runs with
`./build/bin/helix-tests "[1178]"`.

---

## Shell tests never touch the host

`load helpers` shadows `systemctl` with an inert exit-0 shim (`tests/shell/helpers.bash`).
Installer code reaches systemctl through paths a test never names - the update-unit
stop/disable sweep is not gated on INIT_SYSTEM - and on headless CI the denied call hides
behind the installer's `|| true` while on a desktop it raises a polkit prompt per call. A
test that wants scripted systemctl behaviour still calls `mock_command*` (its later write
to the same PATH slot wins); `HELIX_TEST_REAL_SYSTEMCTL=1` disables the shadow for
debugging the helpers themselves.

---

## Test isolation

Fixture ctor **and** dtor call `reset_all()`. Cross-test leaks through `UpdateQueue` are
ratcheted by `scripts/check_update_queue_leaks.py` — if you add a test that queues callbacks
without draining, the nightly leak gate will name it.

XML subjects still register into LVGL's **global** scope (per-test scopes are blocked by LVGL
internals). Each test refreshes them with `init_subjects(true)`.

---

## Lint gates

Gates live in `scripts/check_*.py` and run from `scripts/quality-checks.sh`. Every gate gets a
**meta-test** in `tests/shell/test_*_gate.bats` pinning both halves of its contract: the shape
it must catch, and the idioms it must stay quiet about. A gate that fires on legitimate code
gets switched off, so the silent cases matter as much as the loud ones.

Verify a new gate by running it against known-good and known-bad fixtures before wiring it in.
