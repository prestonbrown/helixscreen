# Thread Safety Reference

## UpdateQueue Architecture

`UpdateQueue` is the single safe bridge from background threads to the LVGL main thread.
It's a `std::queue<std::function>` protected by a mutex. Lambdas are queued from any thread
and drained on the main thread at the START of each `lv_timer_handler()` cycle, BEFORE
rendering begins.

### Main Loop Execution Order

```
1. UpdateQueue::process_pending()  ← HIGHEST PRIORITY — drains all queued lambdas
2. LVGL timers (input polling, animations)
3. process_notifications()         ← Dequeue Moonraker JSON
4. lv_refr_now()                   ← Render to framebuffer
```

### Why queue_update() Not lv_async_call()

LVGL's native `lv_async_call()` can fire DURING the render phase, causing assertion
failures (`!disp->rendering_in_progress`). Our `queue_update()` processes before
rendering, so all subject values are current when widgets draw.

## When to Use queue_update()

**ALWAYS use when on a background thread and you need to:**
- Create/modify widgets (`lv_obj_*()` functions)
- Update subjects (`lv_subject_set_*()`) — subjects trigger observers!
- Any WebSocket callback (libhv event loop thread)
- Network/file I/O completion handlers
- Timer callbacks from non-LVGL timers

**Don't need when:**
- Already on main thread (LVGL event handlers, `lv_timer_create()` callbacks)
- Pure computation with no LVGL calls
- Just logging or updating non-LVGL state

**Key insight:** If you're in a callback from libhv, std::thread, or any networking
library, assume you're on a background thread.

## Backend Integration Pattern

```cpp
#include "ui_update_queue.h"

void WiFiManager::handle_scan_complete(const std::string& data) {
    auto networks = parse_networks(data);           // Parse on BG thread (no LVGL)
    helix::ui::queue_update([networks = std::move(networks), cb = scan_callback_]() {
        cb(networks);                                // Main thread — safe LVGL calls
    });
}
```

**unique_ptr overload** for RAII data transfer:
```cpp
auto data = std::make_unique<MyData>(value, text);
helix::ui::queue_update(std::move(data), [](MyData* d) {
    lv_subject_set_int(&my_subject, d->value);
    // d is automatically deleted after callback
});
```

## ScopedFreeze for Drain+Destroy

When destroying widgets that have pending deferred callbacks:
```cpp
auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
helix::ui::UpdateQueue::instance().drain();
lv_obj_clean(container);
// freeze thaws on scope exit — closes race window
```

Without the freeze, the WebSocket background thread can enqueue new callbacks between
`drain()` and widget destruction.

## AmsState Multi-Backend Coordination

`AmsState` uses `std::recursive_mutex` to protect `backends_` vector. Backend event
callbacks on background threads acquire the mutex, read state, then post subject updates
via `queue_update()`. All subject writes happen on the LVGL thread — no additional
synchronization needed for subject values.

## Threading Model Diagram

```
MAIN THREAD              LIBHV THREAD           UTILITY THREADS
─────────────            ─────────────           ───────────────
lv_timer_handler()       libhv Event Loop        UpdateChecker
  ├ process_pending()    ├ WebSocket conn        TelemetryManager
  ├ LVGL timers          ├ JSON-RPC parse        CrashReporter
  ├ process_notifs()     ├ Auto-reconnect        ───────────────
  └ lv_refr_now()        └ HTTP transfers              │
         ▲                      │                        │
         │                      │ queue_update(λ)        │ queue_update(λ)
         │                      ▼                        ▼
         └────────────── UpdateQueue (mutex) ◄───────────┘
```

## Key Files

- `include/ui_update_queue.h` — UpdateQueue, queue_update(), scoped_freeze()
- `src/api/wifi_manager.cpp` — Reference backend integration
- `src/application/application.cpp` — Main loop order
- `src/printer_state.cpp` — set_*_internal() pattern
