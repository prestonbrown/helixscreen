# Subject Lifecycle Reference

## Subject Types

### Static Subjects (Singleton Lifetime)
Created once, live for app lifetime. No `SubjectLifetime` token needed.
- `get_fan_speed_subject()` (no args)
- `get_bed_temp_subject()`
- `get_nozzle_temp_subject()`

### Dynamic Subjects (Reconnectable)
Destroyed and recreated when hardware is rediscovered. **Always require SubjectLifetime.**
- `PrinterFanState::get_fan_speed_subject(name, lifetime)` — per-fan speeds
- `TemperatureSensorManager::get_temp_subject(name, lifetime)` — per-sensor temps
- `PrinterTemperatureState::get_extruder_temp_subject(name, lifetime)` / `get_extruder_target_subject(name, lifetime)`

## SubjectLifetime Pattern

A `SubjectLifetime` is a `shared_ptr` wrapper. When it's destroyed (via `reset()` or
going out of scope), any `ObserverGuard` created with it sees its `weak_ptr` expire.
This prevents the observer from operating on a freed/recreated subject.

### Pairing Rule

The lifetime MUST outlive the observer. Always pair as parallel **members** in the header:

```cpp
// Header
ObserverGuard temp_observer_;
SubjectLifetime temp_lifetime_;

// Never use a local SubjectLifetime with a member ObserverGuard:
void bind() {
    SubjectLifetime lt;                          // ❌ dies at function exit
    auto* s = tsm.get_temp_subject(name, lt);
    temp_observer_ = observe_int_sync(s, ...lt); // UAF on rediscover
}
```

### Reset Ordering (MANDATORY)

Lifetime BEFORE observer. The observer guard's `weak_ptr` only expires if the
`shared_ptr` (SubjectLifetime) is destroyed first. Wrong order = `lv_observer_remove()`
on a freed subject (#705).

```cpp
// ✅ CORRECT
speed_lifetime_.reset();
speed_observer_.reset();

// ❌ CRASH — observer tries remove() while weak_ptr still alive
speed_observer_.reset();
speed_lifetime_.reset();
```

### Collection Pattern (Carousels, Slot Lists)

```cpp
std::vector<ObserverGuard>     carousel_observers_;
std::vector<SubjectLifetime>   carousel_lifetimes_;   // MUST clear before observers

// Clear:
carousel_lifetimes_.clear();
carousel_observers_.clear();

// Add (in lockstep):
carousel_lifetimes_.emplace_back();
auto* s = state.get_subject(name, carousel_lifetimes_.back());
carousel_observers_.push_back(observe_int_sync(s, handler, carousel_lifetimes_.back()));
```

### Read-Only Access

If you call `get_temp_subject(name, lt)` and never create an observer, the lifetime can
be local. But prefer the no-lifetime overload: `tsm.get_temp_subject(name)` (exists for
this exact case).

## ObserverGuard

### reset() vs release()

- **`reset()`** — Default for ALL normal cleanup. Internally checks `s_subjects_valid` and
  `lv_is_initialized()`. Safe even if LVGL is already torn down.
- **`release()`** — ONLY for pre-deinit cleanup in `StaticSubjectRegistry::register_deinit()`
  callbacks where the subject is already destroyed. Using `release()` in normal code leaks
  `LambdaObserverContext` and corrupts rendering (#579 — 17 reports).

```cpp
// ✅ Normal cleanup (widget delete callback, panel teardown, repopulate)
data->color_observer.reset();

// ❌ Wrong for normal cleanup — leaks context
data->color_observer.release();
```

### Observer Defer Behavior

`observe_int_sync` and `observe_string` **defer callbacks** via `queue_update()` to prevent
re-entrant observer destruction crashes (#82).

Use `observe_int_immediate` / `observe_string_immediate` ONLY if you're certain the
callback won't modify observer lifecycle (no reassignment, no widget destruction).

## Shutdown: StaticPanelRegistry & StaticSubjectRegistry

C++ doesn't guarantee destruction order of statics across translation units. When
`lv_deinit()` runs, it deletes widgets which try to remove observers from subjects. If
subjects aren't deinitialized first → crash in `lv_observer_remove`.

### Shutdown Order

```
1. StaticPanelRegistry::destroy_all()     ← Panels destroy their own subjects
2. StaticSubjectRegistry::deinit_all()    ← Core singleton subjects deinitialized
3. lv_deinit()                            ← Safe — all observers disconnected
```

### Self-Registration (MANDATORY)

Each `init_subjects()` MUST self-register its cleanup. Never register externally (e.g.,
in SubjectInitializer). Co-locating init+cleanup prevents forgotten registrations.

```cpp
void PrinterState::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(
        "PrinterState", []() { PrinterState::instance().deinit_subjects(); });
}
```

SubjectInitializer just calls `init_subjects()` — it does NOT register cleanup.

### deinit_subjects() Pattern

```cpp
void AmsState::deinit_subjects() {
    if (!initialized_) return;
    lv_subject_deinit(&ams_type_);
    lv_subject_deinit(&ams_action_);
    // ... all subjects ...
    initialized_ = false;
}
```

Key properties: reverse registration order (LIFO), idempotent (guard with `initialized_`),
panels before singletons.

### Static Destructors and Logging

In destructors of static/global objects, use `fprintf(stderr, ...)` instead of spdlog —
spdlog may already be destroyed (static destruction order fiasco).

## Timer Lifecycle

LVGL timers (`lv_timer_create()`) are NOT automatically cleaned up. Use `LvglTimerGuard`
for RAII, or manually delete with `lv_is_initialized()` guard in destructor.

```cpp
#include "ui_timer_guard.h"
LvglTimerGuard update_timer_;  // Auto-deleted on destruction
```

## Key Files

- `include/ui_observer_guard.h` — ObserverGuard, SubjectLifetime
- `include/static_subject_registry.h` — StaticSubjectRegistry
- `src/application/static_panel_registry.cpp` — Panel registry
- `src/application/subject_initializer.cpp` — Init-time registration
- `include/ui_timer_guard.h` — LvglTimerGuard
