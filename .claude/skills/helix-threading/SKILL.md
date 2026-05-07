---
name: helix-threading
description: >
  HelixScreen threading & lifecycle safety — triggers when editing code in src/ that crosses the
  main-thread/background-thread boundary: WebSocket/libhv callbacks, src/network/ HTTP workers,
  src/bluetooth/ DBus threads, or src/printer/ background state updates. Also for UpdateQueue/queue_update
  (include/ui_update_queue.h), AsyncLifetimeGuard (include/async_lifetime_guard.h), SubjectLifetime,
  ObserverGuard (include/ui_observer_guard.h), safe_delete_deferred/safe_clean_children (include/ui_utils.h),
  StaticSubjectRegistry shutdown ordering (include/static_subject_registry.h), HttpExecutor thread pools
  (include/http_executor.h), or any code touching lv_subject_t from non-main threads.
---

# HelixScreen Threading & Lifecycle Safety

The **#1 crash prevention** reference for HelixScreen. Threading violations in src/network/, src/bluetooth/,
src/printer/, and lifecycle bugs in src/ui/ account for the majority of field crashes on K1/AD5M/CC1 targets.
Key headers: include/async_lifetime_guard.h, include/ui_observer_guard.h, include/ui_update_queue.h,
include/static_subject_registry.h. Every rule here exists because something crashed in production.

## CRITICAL: Main Thread Only

LVGL is **NOT** thread-safe. All `lv_obj_*()` and `lv_subject_set_*()` calls must run on
the main thread. Subject updates trigger observers that call widget APIs — even seemingly
safe `lv_subject_set_int()` will crash if LVGL is rendering.

**From background threads (WebSocket/libhv callbacks, HTTP workers, std::thread):**
```cpp
// ❌ CRASH — direct subject update from background thread
void on_ws_data(int temp) {
    lv_subject_set_int(&temp_subject, temp);  // Assertion failure → infinite loop on ARM
}

// ✅ CORRECT — defer to main thread
void on_ws_data(int temp) {
    helix::ui::queue_update([temp]() {
        lv_subject_set_int(&temp_subject, temp);
    });
}
```

`helix::ui::queue_update()` processes lambdas at the START of `lv_timer_handler()`,
**before** rendering begins. This is why it's used instead of raw `lv_async_call()`,
which can fire during the render phase.

Reference: `printer_state.cpp` → `set_*_internal()` pattern.

## CRITICAL: No Bare `std::thread().detach()`

On AD5M/CC1/MIPS32, `pthread_create` returns `EAGAIN` under thread exhaustion. The
`std::thread` constructor throws `std::system_error`, which propagates through LVGL
C frames → `std::terminate` → hard crash (#724, #837, #811).

| Workload | Use |
|----------|-----|
| HTTP REST/thumbnails/small uploads | `helix::http::HttpExecutor::fast().submit(fn)` |
| HTTP bundles/gcode/large transfers | `helix::http::HttpExecutor::slow().submit(fn)` |
| sd-bus / BlueZ DBus | `helix::bluetooth::BusThread::run_sync(fn)` |
| BT-over-RFCOMM / USB print / QR decode | `try { std::thread([]{}).detach(); } catch(const std::system_error&) { /* toast */ }` |
| Long-lived worker (joined in dtor) | `std::thread` is fine — the issue is detached one-shots |

## CRITICAL: No Sync Deletion in Queued Callbacks

Multiple sync deletions in one `UpdateQueue::process_pending()` batch corrupt LVGL's
global event linked list → SIGSEGV in `lv_event_mark_deleted` (#776, #190, #80).

**"Queued callback" includes:** `queue_update()` lambdas, `async_call()` (our wrapper),
`lifetime_.defer()` / `tok.defer()` lambdas, `observe_int_sync`/`observe_string` callbacks,
and `register_overlay_close_callback()` lambdas.

| ❌ BANNED inside queued callbacks | ✅ USE INSTEAD |
|-----------------------------------|----------------|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

**`lifetime_.defer` does NOT escape the batch.** It fires in the next `process_pending`
tick — still a batch. The generation guard protects `this`, not event-list integrity.

**True escape routes** (outside UpdateQueue batches): `safe_delete_deferred()`,
`safe_delete_deferred_raw()`, `helix::ui::safe_clean_children()`, `lv_obj_delete_async()`,
raw `lv_async_call()` (LVGL native, NOT our wrapper).

## CRITICAL: AsyncLifetimeGuard for Background Callbacks

Background-thread callbacks that touch UI MUST use `AsyncLifetimeGuard` to prevent
use-after-free when the owning object is dismissed.

```cpp
// Modal and OverlayBase provide lifetime_ automatically.
// Standalone classes: helix::AsyncLifetimeGuard lifetime_;

// BG-thread callback pattern:
auto tok = lifetime_.token();
api->fetch([this, tok]() {
    if (tok.expired()) return;         // Owner dismissed — skip
    tok.defer([this]() {               // Safe: uses token's shared_ptr
        update_ui();
    });
});
```

**TOCTOU rule:** From background threads, use `tok.defer()` NOT `lifetime_.defer()`.
`lifetime_.defer()` reads `this->lifetime_` — a race if `this` is being destroyed (#707).
`lifetime_.defer()` is safe ONLY from the main thread.

**Cancel-and-retry:** `lifetime_.invalidate(); auto tok = lifetime_.token();`

**Deprecated patterns** (do NOT use in new code): `shared_ptr<bool> callback_guard_`,
`shared_ptr<atomic<bool>> alive_`, `weak_ptr<bool>`, `async_call(guard_widget, cb, data)`.

## CRITICAL: SubjectLifetime for Dynamic Subjects

Per-fan, per-sensor, per-extruder subjects are **dynamic** — destroyed and recreated on
reconnect. Observing without `SubjectLifetime` = use-after-free crash.

**The lifetime MUST outlive the observer.** A local `SubjectLifetime lt;` paired with a
member `ObserverGuard` is a UAF. Always pair them as parallel members in the header:

```cpp
// ✅ Header — parallel members
ObserverGuard temp_observer_;
SubjectLifetime temp_lifetime_;

// ✅ Clear — lifetime FIRST, then observer
temp_lifetime_.reset();
temp_observer_.reset();

// ✅ Bind
auto* s = tsm.get_temp_subject(name, temp_lifetime_);
temp_observer_ = observe_int_sync(s, handler, temp_lifetime_);
```

For collections: `std::vector<ObserverGuard> observers_;` + `std::vector<SubjectLifetime> lifetimes_;`
— keep aligned, push/pop in lockstep.

**Dynamic subject sources** (always require lifetime): `PrinterFanState::get_fan_speed_subject(name, lt)`,
`TemperatureSensorManager::get_temp_subject(name, lt)`, `PrinterTemperatureState::get_extruder_temp_subject(name, lt)`.

**Static subjects** (no lifetime needed): `get_fan_speed_subject()` (no args), `get_bed_temp_subject()`, etc.

## CRITICAL: ObserverGuard::reset() Only

Use `reset()` for ALL normal cleanup — panel teardown, `LV_EVENT_DELETE` callbacks,
repopulate. `reset()` internally checks `s_subjects_valid` and `lv_is_initialized()`.

`release()` is ONLY for pre-deinit cleanup in `StaticSubjectRegistry::register_deinit()`
callbacks where the subject is already destroyed. Using `release()` in normal cleanup
leaks `LambdaObserverContext` and corrupts rendering state (#579).

## CRITICAL: UpdateQueue ScopedFreeze for Drain+Destroy

When destroying widgets that may have pending deferred callbacks, freeze the queue to
close the race where the background thread enqueues between `drain()` and destruction:

```cpp
auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
helix::ui::UpdateQueue::instance().drain();
lv_obj_clean(container);  // or safe_delete(), lv_obj_delete()
// freeze thaws on scope exit
```

## CRITICAL: Subject Shutdown Self-Registration

Every `init_subjects()` MUST self-register its `deinit_subjects()` with
`StaticSubjectRegistry` or `StaticPanelRegistry`. Never register externally:

```cpp
void MyState::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(
        "MyState", []() { MyState::instance().deinit_subjects(); });
}
```

Shutdown order: `StaticPanelRegistry::destroy_all()` → `StaticSubjectRegistry::deinit_all()` → `lv_deinit()`.

## CRITICAL: No lv_obj_delete() in Input Event Handlers

Never delete container children synchronously inside `LV_EVENT_CLICKED`/`LV_EVENT_RELEASED`.
LVGL may be iterating the child list. Null pointers and let a rebuild's `lv_obj_clean()`
handle it, or use `lv_obj_delete_async()`.

## Observer Defer Behavior

`observe_int_sync` and `observe_string` **defer callbacks** via `queue_update()` to
prevent re-entrant observer destruction (#82). Use `observe_int_immediate` /
`observe_string_immediate` ONLY if the callback won't modify observer lifecycle.

## No lv_obj_delete() on Container Children During Input Events

During `LV_EVENT_CLICKED`/`LV_EVENT_RELEASED` (indev dispatch), LVGL iterates the parent's
child list. Sync deletion corrupts iteration → SIGSEGV. Null the pointer and let a
rebuild's `lv_obj_clean()` handle it.

## File Index

| File | Content |
|------|---------|
| `references/thread-safety.md` | UpdateQueue internals, when to use queue_update, backend integration |
| `references/subject-lifecycle.md` | SubjectLifetime, ObserverGuard, dynamic vs static subjects, shutdown registries |
| `references/async-safety.md` | AsyncLifetimeGuard, tok.defer vs lifetime_.defer, escape routes, no-bare-threads |
| `references/testing-patterns.md` | Test fixtures, UpdateQueue drain, guard cleanup, observer test gotchas |
| `references/gotchas.md` | Symptom-indexed troubleshooting for threading and lifecycle issues |

## Key Source Files

| File | What |
|------|------|
| `include/async_lifetime_guard.h` | AsyncLifetimeGuard + LifetimeToken |
| `include/ui_observer_guard.h` | ObserverGuard + SubjectLifetime |
| `include/ui_update_queue.h` | UpdateQueue + queue_update() |
| `include/ui_utils.h` | safe_delete_deferred, safe_clean_children |
| `include/static_subject_registry.h` | Shutdown cleanup registry |
| `include/http_executor.h` | HttpExecutor pools |
| `include/ui_timer_guard.h` | LvglTimerGuard RAII |
| `include/ui_widget_memory.h` | lvgl_unique_ptr / lvgl_make_unique |
| `src/application/application.cpp` | Shutdown order |
