# Async Safety Reference

## AsyncLifetimeGuard

Generation-counter-based utility for preventing use-after-free when background-thread
callbacks fire after the owning object is dismissed. Located in `include/async_lifetime_guard.h`.

### Who Has It Built-In

- `Modal` — `lifetime_` member, `hide()` calls `invalidate()` automatically
- `OverlayBase` — `lifetime_` member, `cleanup()`/`on_deactivate()` calls `invalidate()`
- **Standalone panels** — add `helix::AsyncLifetimeGuard lifetime_;` as a member directly

### Basic Pattern (90% of cases)

```cpp
auto token = lifetime_.token();
api->fetch([this, token]() {
    if (token.expired()) return;          // Owner was dismissed
    lifetime_.defer([this]() {            // Queue to main thread, auto-guarded
        update_ui();
    });
});
```

### TOCTOU Rule: tok.defer() vs lifetime_.defer()

**From background threads:** ALWAYS use `tok.defer()`. The token holds its own
`shared_ptr`, safe from BG thread access.

**From the main thread:** `lifetime_.defer()` is fine — `this` is guaranteed valid.

```cpp
// ❌ CRASH from BG thread — reads this->lifetime_ which may be destroyed (#707)
api->fetch([this]() {
    if (lifetime_.token().expired()) return;  // TOCTOU: this might be dead
    lifetime_.defer([this]() { update_ui(); });
});

// ✅ CORRECT from BG thread — token owns its shared_ptr
auto tok = lifetime_.token();
api->fetch([this, tok]() {
    if (tok.expired()) return;
    tok.defer([this]() { update_ui(); });
});

// ✅ CORRECT from main thread
void MyPanel::on_activate() {
    lifetime_.defer([this]() { rebuild_layout(); });  // OK — main thread
}
```

### Cancel-and-Retry

```cpp
lifetime_.invalidate();                // Expire all outstanding tokens
auto tok = lifetime_.token();          // Fresh token for new operation
api->test([this, tok]() { ... });
```

### Key Properties

- `defer()` queues via `queue_update()`, silently skipping if invalidated
- `token()` returns a `LifetimeToken` for manual checking in non-queue callbacks
- Safe after owner destruction — tokens hold `shared_ptr` to counter, not pointer to owner
- Destructor calls `invalidate()` automatically

### Deprecated Patterns (Do NOT Use)

- `shared_ptr<bool> callback_guard_` / `alive_guard_`
- `shared_ptr<atomic<bool>> alive_`
- `shared_ptr<atomic<uint64_t>>` generation counters
- `weak_ptr<bool>` for callback safety
- `async_call(guard_widget, cb, data)` for modal/overlay callback guards

## No Sync Widget Deletion in Queued Callbacks

### What Counts as "Inside a Queued Callback"

Everything through `process_pending()`:
- `helix::ui::queue_update(...)` / `ui_queue_update(...)` lambdas
- `helix::ui::async_call(cb, ud)` (our wrapper, NOT LVGL native)
- `register_overlay_close_callback(...)` lambdas
- `AsyncLifetimeGuard::defer(...)` / `lifetime_.defer(...)` lambdas
- `LifetimeToken::defer(...)` / `tok.defer(...)` lambdas
- `observe_int_sync` / `observe_string` callbacks (deferred through queue_update since #82)

### Banned → Replacement Table

| ❌ Banned inside queued callbacks | ✅ Use instead |
|-----------------------------------|----------------|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

### Why Replacements Are Safe

They route deletion through `lv_obj_delete_async()`, which posts to LVGL's own async list
(processed at end of `lv_timer_handler()`, after our `process_pending()` returns). Hits
one-at-a-time across ticks, not batched in our drain.

`safe_clean_children()` reparents each child to `lv_layer_top()` and async-deletes it —
the container appears empty immediately, so callers can add new children right after.

### lifetime_.defer Does NOT Escape the Batch

The generation guard protects `this` against use-after-free. It does NOT move the callback
out of `process_pending()`. The callback runs in the next batch, which still contains
whatever else was queued for that tick. Any comment claiming "defer outside process_pending"
paired with `lifetime_.defer` is wrong — fix the comment.

### Code Examples

```cpp
// ❌ CRASH — sync deletion inside UpdateQueue batch
helix::ui::async_call([dialog]() {
    helix::ui::safe_delete(dialog);
});

// ❌ STILL CRASH — lifetime_.defer goes through queue_update
lifetime_.defer([this]() {
    lv_obj_clean(container_);
});

// ✅ CORRECT — use deferred variant
lifetime_.defer([this]() {
    helix::ui::safe_clean_children(container_);
    rebuild(container_);
});

// ✅ CORRECT — safe_delete_deferred for single pointer
helix::ui::queue_update("cleanup", [this]() {
    helix::ui::safe_delete_deferred(overlay_);
});
```

### True Escape Routes

These run outside UpdateQueue batches:
- `safe_delete_deferred()` / `safe_delete_deferred_raw()` (`include/ui_utils.h`)
- `helix::ui::safe_clean_children()` (`include/ui_utils.h`)
- `lv_obj_delete_async(obj)` (raw LVGL)
- `lv_async_call(cb, ud)` (raw LVGL — NOT our `helix::ui::async_call` wrapper)

## No Bare std::thread().detach()

### The Problem

On K1/AD5M/CC1/MIPS32, `pthread_create` returns `EAGAIN` under thread exhaustion. The
`std::thread` constructor throws `std::system_error`, which propagates through LVGL C
event-dispatch frames or `noexcept` boundaries → `std::terminate` → hard crash.

Crashes appeared as unrelated code paths: #724 (wizard camera probe), #837 (debug-bundle
upload), #811-adjacent (HTTP storm).

### Managed Pool Routing

| Workload | Use |
|----------|-----|
| HTTP REST/thumbnails/small uploads | `helix::http::HttpExecutor::fast().submit(fn)` (4 workers) |
| HTTP bundles/gcode/large transfers | `helix::http::HttpExecutor::slow().submit(fn)` (1 worker) |
| sd-bus / BlueZ DBus calls | `helix::bluetooth::BusThread::run_sync(fn)` |
| BT-over-RFCOMM / USB print / QR decode / device discovery | `try { std::thread([...]{ }).detach(); } catch (const std::system_error& e) { /* toast + error callback */ }` |
| Long-lived worker (member, joined in dtor) | `std::thread` is fine |

**Before adding a new `std::thread`:** grep for an existing managed pool that covers the
domain. Raw detached spawns reintroduce the anti-pattern.

## No lv_obj_delete() in Input Event Handlers

Never delete container children synchronously inside `LV_EVENT_CLICKED`/`LV_EVENT_RELEASED`.
LVGL may be iterating the parent's child list during `indev_proc_release`.

```cpp
// ❌ CRASH
void on_done_clicked(lv_event_t* e) {
    lv_obj_delete(overlay_);    // Corrupts child list mid-iteration
    overlay_ = nullptr;
    rebuild_widgets();
}

// ✅ CORRECT
void on_done_clicked(lv_event_t* e) {
    overlay_ = nullptr;         // Drop reference
    rebuild_widgets();          // lv_obj_clean() handles it after input processing
}
```

When no rebuild follows: use `lv_obj_delete_async()` or `helix::ui::safe_delete()`.

**Caveat:** `lv_obj_delete_async()` is NOT safe if a subsequent `lv_obj_clean()` on the
parent runs before the async fires — causes double-free. Only use when no parent cleanup
follows.

## Key Files

- `include/async_lifetime_guard.h` — AsyncLifetimeGuard, LifetimeToken
- `include/ui_utils.h` — safe_delete_deferred, safe_clean_children, safe_delete_deferred_raw
- `include/http_executor.h` — HttpExecutor fast()/slow() pools
- `include/bt_bus_thread.h` — BlueZ BusThread
