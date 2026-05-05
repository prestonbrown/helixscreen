# Declarative UI Rules & Threading Safety

## Reactive-First Principle

**ALL UI control MUST be reactive via subjects.** Direct widget manipulation is an anti-pattern.

### Banned Patterns

| Pattern | Why Banned | Alternative |
|---------|------------|-------------|
| `lv_obj_add_event_cb()` | Tight coupling | XML `<event_cb>` + `lv_xml_register_event_cb()` |
| `lv_label_set_text()` | Bypasses binding | `bind_text` subject + `lv_subject_set_string()` |
| `lv_obj_add_flag(HIDDEN)` | Visibility is UI | `<bind_flag_if_eq>` in XML |
| `lv_obj_set_style_*()` | Styling in XML | Design tokens in XML |
| `lv_obj_add_state(DISABLED)` | State is UI | `<bind_state_if_eq>` in XML |

### Reactive Patterns for Common UI Tasks

| UI Task | ✅ Reactive Way |
|---------|----------------|
| Update text | `bind_text` in XML, `lv_subject_set_string()` in C++ |
| Enable/disable | `bind_flag_if_eq` for `clickable` or `disabled` flag |
| Show/hide | `bind_flag_if_eq` for `hidden` flag |
| Update value | `bind_value` in XML, `lv_subject_set_int()` in C++ |
| Visual feedback | `bind_style` + conditional styles |

### When Direct Access IS Acceptable

1. `LV_EVENT_DELETE` cleanup
2. Widget pool recycling (virtual scroll)
3. Chart data points
4. Animations
5. One-time `setup()` widget lookup

**After initialization, ALL updates must be reactive.**

## Threading Rules

### Background Thread Safety

WebSocket/libhv callbacks run on background threads. **NEVER** call `lv_subject_set_*()` directly.

```cpp
// ✅ CORRECT — queue update to main thread
ui_queue_update([]() {
    lv_subject_set_int(&subject, value);
});
```

### Async Callback Safety

When background threads need to update UI, use `AsyncLifetimeGuard`:

```cpp
auto tok = lifetime_.token();
api->fetch([this, tok]() {
    if (tok.expired()) return;         // Owner dismissed
    tok.defer([this]() {               // Safe main-thread update
        update_ui();
    });
});
```

**CRITICAL:** Use `tok.defer()` (NOT `lifetime_.defer()`) from background threads. `lifetime_.defer()` reads `this->lifetime_` which is a TOCTOU race.

### No `std::thread(...).detach()` for Fire-and-Forget

Use managed pools:

| Workload | Use |
|----------|-----|
| HTTP (REST/thumbnails) | `HttpExecutor::fast().submit(fn)` |
| HTTP (bundles/large) | `HttpExecutor::slow().submit(fn)` |
| sd-bus / BlueZ | `BusThread::run_sync(fn)` |

### No Sync Widget Deletion in Queued Callbacks

| ❌ BANNED in queued callbacks | ✅ USE INSTEAD |
|------------------------------|----------------|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

### UpdateQueue ScopedFreeze for Drain+Destroy

```cpp
auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
helix::ui::UpdateQueue::instance().drain();
lv_obj_clean(container);
// freeze thaws on scope exit
```

## Subject Lifecycle Safety

### Subject Shutdown Registration

Any class creating subjects MUST self-register cleanup:

```cpp
void MyState::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;
    StaticSubjectRegistry::instance().register_deinit(
        "MyState", []() { MyState::instance().deinit_subjects(); });
}
```

Never register cleanup externally — co-locate init + cleanup.

### Dynamic Subject Lifetime

Per-fan, per-sensor, per-extruder subjects are dynamic — they can be destroyed and recreated.

**Must use `SubjectLifetime` token** for dynamic subjects. Pair member `ObserverGuard` with member `SubjectLifetime`:

```cpp
// Header
ObserverGuard temp_observer_;
SubjectLifetime temp_lifetime_;    // MUST be member, never local

// Reset ordering: lifetime FIRST, then observer
temp_lifetime_.reset();
temp_observer_.reset();
```

Dynamic subject sources (require lifetime token):
- `PrinterFanState::get_fan_speed_subject(name, lifetime)`
- `TemperatureSensorManager::get_temp_subject(name, lifetime)`
- `PrinterTemperatureState::get_extruder_temp_subject(name, lifetime)`

### ObserverGuard::reset() vs release()

Always use `reset()` for normal cleanup. Use `release()` ONLY in `StaticSubjectRegistry::register_deinit()` callbacks where the observer ctx must be intentionally leaked.

## Observer Cleanup in DELETE Handlers

Track and remove observers before freeing:

```cpp
// Binding
data->text_observer = lv_label_bind_text(label, &subject, "%s");

// DELETE handler
static void on_delete(lv_event_t* e) {
    auto* data = get_data(e);
    if (data->text_observer) lv_observer_remove(data->text_observer);
    delete data;
}
```

For custom widgets with owned subjects, detach children BEFORE deiniting:

```cpp
lv_obj_remove_from_subject(child_label, nullptr);  // Remove ALL observers
lv_subject_deinit(&owned_subject);                  // Now safe
```

## No Object Deletion During Input Events

Never delete container children synchronously in `LV_EVENT_CLICKED`/`LV_EVENT_RELEASED` handlers. LVGL may be iterating the child list. Null pointers and let rebuild handle deletion.

## Common Gotchas (Symptom → Cause → Fix)

| Symptom | Cause | Fix |
|---------|-------|-----|
| Component doesn't render | Not registered | Add to `xml_registration.cpp` |
| `bind_style` has no effect | Inline style overrides | Remove inline, use two `bind_style` |
| Subject stuck at default | XML `<subjects>` shadows C++ | Don't declare in both places |
| Button click does nothing | Callback not registered | `lv_xml_register_event_cb()` |
| Icon renders as tofu | Font not regenerated | `make regen-fonts` |
| XML edit not showing | Wrong file or stale binary | Check path, relaunch (no rebuild needed) |
| App crashes on reconnect | Dynamic subject without lifetime | Add member `SubjectLifetime` |
| `std::thread().detach()` crash | Thread exhaustion on ARM | Use managed pool |
| `lv_obj_delete()` SIGSEGV in callback | Sync delete in queued callback | Use `lv_obj_delete_async()` |
