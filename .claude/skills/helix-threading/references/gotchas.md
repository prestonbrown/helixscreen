# Threading & Lifecycle Gotchas

Symptom-indexed troubleshooting. Search by what you're seeing.

---

## "App crashes on reconnect or panel rebuild in observer callback"

**Cause:** Observing a dynamic subject (per-fan, per-sensor, per-extruder) without a
`SubjectLifetime` token — or with a local `SubjectLifetime` that dies before the observer.

**Fix:** Pair member `ObserverGuard` with a member `SubjectLifetime`. Reset lifetime BEFORE
observer. For collections, use parallel vectors kept in lockstep.

```cpp
// Header
ObserverGuard temp_observer_;
SubjectLifetime temp_lifetime_;

// Clear (order matters!)
temp_lifetime_.reset();    // FIRST
temp_observer_.reset();    // SECOND
```

See: lessons L077, L084, `include/ui_observer_guard.h`.

---

## "lifetime_.defer() from background thread crashes"

**Cause:** `lifetime_.defer()` reads `this->lifetime_` — a TOCTOU race from a background
thread. `this` can be destroyed between the check and the deref.

**Fix:** Use `tok.defer()` instead. The token holds its own `shared_ptr`.

```cpp
auto tok = lifetime_.token();
api->fetch([this, tok]() {
    if (tok.expired()) return;
    tok.defer([this]() { update_ui(); });   // Safe from BG thread
});
```

See: CLAUDE.md § Async callback safety, issue #707.

---

## "Fire-and-forget std::thread().detach() crashes on K1/AD5M/CC1"

**Cause:** `pthread_create` returns `EAGAIN` under thread exhaustion on small ARM devices.
The `std::thread` constructor throws, propagating through LVGL C frames → abort.

**Fix:** Use managed pools (`HttpExecutor`, `BusThread`). For genuinely one-shot threads,
wrap in try/catch for `std::system_error`.

See: lesson L083.

---

## "lv_obj_delete() inside queued callback → SIGSEGV"

**Cause:** Multiple sync deletions in one `UpdateQueue::process_pending()` batch corrupt
LVGL's event linked list (#776, #190, #80). `lifetime_.defer()` doesn't escape the batch.

**Fix:**

| Instead of | Use |
|---|---|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

See: CLAUDE.md § No sync widget deletion, lesson L081.

---

## "Two panel widget instances but only one gets updates"

**Cause:** Declared a per-instance subject but registered globally, or vice versa with
XML scope resolution.

**Fix:** Decide: shared subject (register into component scope) or per-instance (rarely
needed — usually a shared subject filtered by ID).

---

## "lv_subject_set_*() from WebSocket callback hangs the app"

**Cause:** Subject updates trigger bound observers, which call widget APIs. If LVGL is
rendering → assertion failure → infinite loop on ARM.

**Fix:** Wrap in `helix::ui::queue_update()`.

---

## "Crash on shutdown in lv_observer_remove"

**Cause:** Static destruction order fiasco. Widgets destroyed by `lv_deinit()` try to
remove observers from already-destroyed subjects.

**Fix:** Ensure `init_subjects()` self-registers `deinit_subjects()` with the appropriate
registry. Never register cleanup externally.

---

## "Crash on widget delete during button click"

**Cause:** Deleting container children synchronously inside `LV_EVENT_CLICKED`/`LV_EVENT_RELEASED`.
LVGL is iterating the child list during `indev_proc_release`.

**Fix:** Null the pointer and let rebuild's `lv_obj_clean()` handle deletion. Or use
`lv_obj_delete_async()` when no parent cleanup follows.

---

## "ObserverGuard::release() leaks / corrupts rendering"

**Cause:** `release()` skips `lv_observer_remove()`, leaking the `LambdaObserverContext`.
This misconception caused 17 reports in #579.

**Fix:** Use `reset()` for all normal cleanup. `release()` is only for pre-deinit registry
callbacks where the subject is already destroyed.

---

## "lv_obj_delete_async() causes double-free"

**Cause:** Parent's `lv_obj_clean()` runs before the async delete fires.

**Fix:** Only use `lv_obj_delete_async()` when no parent cleanup follows the async call.
Otherwise, null the pointer and let the parent's clean handle it.
