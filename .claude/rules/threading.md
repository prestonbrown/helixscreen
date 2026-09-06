---
paths:
  - "src/**/*"
  - "include/**/*"
  - "tests/unit/**/*"
---
# Threading & Lifecycle

Full rules: `docs/devel/THREADING.md` (the `helix-threading` skill routes into its
sections). Read it before code that crosses a thread boundary, observes a subject, or
destroys a widget. Each invariant below fails silently at compile time and crashes
later, usually on a customer's printer.

1. **Never touch LVGL from a background thread.** WebSocket/libhv, HTTP and timer
   callbacks are background threads, and `lv_subject_set_*()` counts because it fires
   observers that call widget APIs. Route through `ui_queue_update()`
   (`ui_update_queue.h`). Pattern: `src/printer/printer_state.cpp` `set_*_internal()`.
2. **Never write a bare `if (tok.expired()) return;` on a background thread and then
   touch `this`.** TOCTOU use-after-free (#707). Use `lifetime_.bg_cb(tag, fn)`, or
   `tok.defer(tag, fn)` when there is bg-side parsing worth keeping off the main thread.
   `lifetime_.defer()` is main-thread only. Gate: `scripts/check_l081_anti_pattern.py`.
3. **Never delete synchronously inside a queued callback.** `safe_delete()`,
   `lv_obj_delete()` and `lv_obj_clean()` corrupt LVGL's event list mid-batch (#776).
   Use `safe_delete_deferred()`, `lv_obj_delete_async()`, `safe_clean_children()`.
   `lifetime_.defer` does NOT escape the batch: it fires in the next `process_pending`
   tick, which is still a batch.
4. **A fetched `SubjectLifetime` must be handed to `observe_*`.** The factories take it
   as a defaulted 4th parameter, so omitting it is silent: the guard never sees the
   subject die and `reset()` calls `lv_observer_remove()` on freed memory (#705). Local
   vs member is not what decides correctness: the `get_*_subject(name, lifetime)`
   accessors assign the owner's own `shared_ptr`, so a caller's copy dying never
   expires the guard.
5. **A raw `lv_timer_t*` cancelled in `cleanup()` must also be cancelled in the
   destructor.** `StaticPanelRegistry::destroy_all()` runs before `lv_deinit()`, so
   teardown that skips the explicit stop leaves the timer armed on a freed `this`
   (#1173). Share one `cancel_*_timer()` between both paths and use
   `lv_timer_cancel_safe()`, which self-guards on `lv_is_initialized()` and neuters
   instead of unlinking, so it is safe from a destructor and from inside
   `lv_timer_handler`. A `LifetimeToken`-guarded callback is the other valid answer;
   annotate those `// TIMER_DTOR_OK: <reason>`. Gate:
   `scripts/check_timer_destructor_cancel.py`.

Also: no `std::thread(...).detach()` for one-shot work (`EAGAIN` becomes
`std::terminate` on AD5M/CC1, #724); use `HttpExecutor::fast()/slow()` or `BusThread`.
`ObserverGuard::reset()` for all normal cleanup, never `release()` (#579). Every
`init_subjects()` self-registers its `deinit_subjects()` with `StaticSubjectRegistry`.
