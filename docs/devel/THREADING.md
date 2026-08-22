# Threading & Lifecycle

The single source of truth for HelixScreen's threading, async-callback, and object-lifetime
rules. **Every rule here exists because something crashed in production** — issue numbers
are cited so you can read the original failure.

Threading violations in `src/network/`, `src/bluetooth/`, and `src/printer/`, plus lifecycle
bugs in `src/ui/`, account for the majority of field crashes on K1/AD5M/CC1 targets. Those
devices are 32-bit, memory-constrained, and single-core enough that races which are
theoretical on a desktop are reproducible on hardware.

Read this before writing code that crosses a thread boundary, observes a subject, or
destroys a widget. `ARCHITECTURE.md` describes what the system *is*; this doc describes what
you must *do*.

## Invariants at a glance

Each of these fails **silently at compile time** and crashes later, usually on a customer's
printer rather than your desk.

| # | Invariant | Failure mode if violated | Refs |
|---|-----------|--------------------------|------|
| 1 | Never call `lv_*` or `lv_subject_set_*` from a background thread | LVGL assertion → infinite `while(1)` on ARM; app hangs | — |
| 2 | Never write bare `if (tok.expired()) return;` on a bg thread, then touch `this` | TOCTOU use-after-free | L081, #707 |
| 3 | Never delete a widget synchronously inside a queued callback | Event-list corruption → SIGSEGV in `lv_event_mark_deleted` | #776, #190, #80 |
| 4 | Never observe a dynamic subject without passing its `SubjectLifetime` to the `observe_*` factory | Use-after-free on reconnect/rediscovery | #705 |
| 5 | Never `std::thread(...).detach()` for one-shot work | `EAGAIN` → `std::system_error` → `std::terminate` | #724, #837, L083 |
| 6 | Never `ObserverGuard::release()` in normal cleanup — use `reset()` | Leaks `LambdaObserverContext`, corrupts rendering | #579 |
| 7 | Never delete container children inside an input event handler | Child-list iteration corrupted → SIGSEGV | — |
| 8 | Every `init_subjects()` self-registers its `deinit_subjects()` | Crash in `lv_observer_remove` at shutdown | — |
| 9 | A raw `lv_timer_t*` cancelled in `cleanup()` must also be cancelled in the destructor | Armed timer firing on a freed `this` | #1173, #750, #751 |

Invariants 2 and 9 have automated gates (`scripts/check_l081_anti_pattern.py`,
`scripts/check_timer_destructor_cancel.py`), run by `scripts/quality-checks.sh` on every
commit. The rest are reviewed by humans.

---

## 1. LVGL is single-threaded

**LVGL is not thread-safe.** All widget creation and modification must happen on the main
thread.

The dangerous misconception is that `lv_subject_set_*()` is safe because it only updates a
value. It is not:

```cpp
// ❌ DANGEROUS — looks safe, isn't
void update_from_websocket_thread(int temp) {
    lv_subject_set_int(temp_subject, temp);  // Triggers observers!
}
```

Subject updates trigger bound observers, and those observers call widget APIs —
`lv_label_set_text()` → `lv_obj_invalidate()`, `lv_obj_add_flag()` → `lv_obj_invalidate()`.
Any widget modification while `lv_timer_handler()` is rendering trips the
`!disp->rendering_in_progress` assertion, which on embedded targets is an infinite loop.

Real case: a WebSocket callback called `FilamentSensorManager::discover_sensors()`, which
called `lv_subject_set_int()`. When LVGL happened to be mid-render, the app hung and stopped
answering pings.

### The safe bridge: `queue_update()`

```cpp
#include "ui_update_queue.h"

// ✅ CORRECT — defers to the main thread
void update_from_websocket_thread(int temp) {
    helix::ui::queue_update([temp]() {
        lv_subject_set_int(&temp_subject, temp);
    });
}

// With owned data — unique_ptr overload for RAII transfer
auto data = std::make_unique<MyData>(value, text);
helix::ui::queue_update(std::move(data), [](MyData* d) {
    lv_subject_set_int(&my_subject, d->value);
    // d is deleted automatically after the callback
});
```

`UpdateQueue` is a mutex-protected `std::queue<std::function>`. Lambdas are enqueued from any
thread and drained on the main thread at the **start** of each `lv_timer_handler()` cycle,
before rendering:

```
1. UpdateQueue::process_pending()  ← drains all queued lambdas (1 ms LVGL timer)
2. LVGL timers (input polling, animations)
3. process_notifications()         ← dequeue Moonraker JSON
4. lv_refr_now()                   ← render to framebuffer
```

**Why not LVGL's native `lv_async_call()`?** It can fire *during* the render phase, causing
the same assertion failure. `queue_update()` runs before rendering, so every subject value is
current when widgets draw.

### When you need it

Always, when on a background thread and you:
- create or modify widgets (`lv_obj_*()`)
- update subjects (`lv_subject_set_*()`) — subjects trigger observers
- are in a WebSocket callback (libhv event loop thread)
- are in a network or file-I/O completion handler
- are in a timer callback from a non-LVGL timer

Not needed when:
- already on the main thread (LVGL event handlers, `lv_timer_create()` callbacks)
- doing pure computation with no LVGL calls
- only logging, or updating non-LVGL state

**Key heuristic:** if you're in a callback from libhv, `std::thread`, or any networking
library, assume you are on a background thread.

### Threading model

```
MAIN THREAD              LIBHV THREAD           UTILITY THREADS
─────────────            ─────────────          ───────────────
lv_timer_handler()       libhv Event Loop       UpdateChecker
  ├ process_pending()      ├ WebSocket conn     TelemetryManager
  ├ LVGL timers            ├ JSON-RPC parse     CrashReporter
  ├ process_notifs()       ├ Auto-reconnect     ───────────────
  └ lv_refr_now()          └ HTTP transfers            │
         ▲                        │                     │
         │                        │ queue_update(λ)     │ queue_update(λ)
         │                        ▼                     ▼
         └──────────────── UpdateQueue (mutex) ◄────────┘
```

### Reference implementations

- `src/printer/printer_state.cpp` — the `set_*_internal()` pattern (`set_klippy_state()` →
  `set_klippy_state_internal()`, and likewise for `set_klipper_version()`,
  `set_printer_connection_state()`)
- `src/api/wifi_manager.cpp` — every event handler parses on the background thread, then
  marshals to main:

```cpp
void WiFiManager::handle_scan_complete(const std::string& data) {
    auto networks = parse_networks(data);   // BG thread — no LVGL calls
    helix::ui::queue_update([networks = std::move(networks), cb = scan_callback_]() {
        cb(networks);                        // Main thread — widgets now safe
    });
}
```

### Multi-backend coordination (AmsState)

`AmsState` guards its `backends_` vector with a `std::recursive_mutex`. A backend emitting an
event on a background thread acquires the mutex, reads backend state, then posts subject
updates via `queue_update()`. Each backend's callback captures its index at registration time
so events route to the correct per-backend subject storage.

For secondary backends (index 1+), slot subjects live in `BackendSlotSubjects` structs rather
than the flat `slot_colors_[]` / `slot_statuses_[]` arrays; `sync_backend(int)` and
`update_slot_for_backend(int, int)` handle the routing. Because all subject writes happen on
the LVGL thread, the subject values themselves need no further synchronization.

---

## 2. Async callback safety: `AsyncLifetimeGuard`

Background-thread callbacks that touch UI must be guarded against the owning
modal/overlay/panel being dismissed before the callback fires. `AsyncLifetimeGuard`
(`include/async_lifetime_guard.h`) is a generation-counter utility for exactly this.

**Who has one already:**
- `Modal` — `lifetime_` member; `hide()` calls `invalidate()` automatically
- `OverlayBase` — `lifetime_` member; `cleanup()` / `on_deactivate()` call `invalidate()`
- **Standalone classes** — declare your own: `helix::AsyncLifetimeGuard lifetime_;`

### Two correct forms

**Short form — `lifetime_.bg_cb(tag, fn)`.** Preferred when there is no background-side
parsing worth keeping off the main thread. It returns a callable that auto-defers the whole
body, so no `expired()` check ever appears on the background thread:

```cpp
api_->rest().get_strips(
    lifetime_.bg_cb("LedController::on_strips", [this](const Resp& r) {
        // runs on the main thread; safe to touch this->member
        wled_.add_strip(r);
        emit_event(EVENT);
    }),
    [](const Err& e) { spdlog::warn("get_strips failed: {}", e.message); });
```

**Long form — `tok.defer(tag, fn)`.** Use when you have real background-side work (a large
JSON parse) worth keeping off the main thread. Do the parsing into **local** objects, then
defer only the mutation:

```cpp
auto tok = lifetime_.token();
api_->rest().get_strips(
    [this, tok](const Resp& r) {
        // BG: parse, validate, build LOCAL objects — no `this` access
        Local out = parse(r);
        // MAIN: only the mutation
        tok.defer("LedController::on_strips_apply",
                  [this, out = std::move(out)]() mutable {
            wled_.set_all(std::move(out));
        });
    },
    [](const Err& e) { spdlog::warn("get_strips failed: {}", e.message); });
```

### FORBIDDEN: bare `if (tok.expired()) return;` on a background thread

This is L081 Mechanism C (cluster `pstat-async-delete`) — a TOCTOU race. The owner can be
destroyed between the check and the access:

```cpp
// ❌ BANNED — UAF if `this` is destroyed after the check
api_->rest().get_strips([this, tok](const Resp& r) {
    if (tok.expired()) return;
    member_ = r;            // race with the destructor
    emit_event(EVENT);
});

// ❌ ALSO BANNED — `lifetime_.defer` is a member access on `this`
api->fetch([this, token]() {
    if (token.expired()) return;
    lifetime_.defer([this]() { update_ui(); });
});
```

The fix is never "add a check" — it is to defer, so the guard is evaluated atomically on the
main thread. Use `bg_cb` or `tok.defer` as shown above.

**Enforcement, in three layers:**

1. **Lint gate** — `scripts/check_l081_anti_pattern.py` flags an `expired()` check followed
   within 10 lines by a `this` access (`this->`, `api_->`, `emit_event(`, any
   `member_trailing_underscore` deref, member mutex locks). Runs on every commit via
   `scripts/quality-checks.sh`. An `expired()` check followed *only* by `return;` or
   local-only work is permitted.
2. **Runtime detector** — lives inside `LifetimeToken::expired()`, so it fires on both
   `.expired()` and `->expired()`. Emits a `cluster:pstat-async-delete Mechanism C` warning
   once per callsite.
3. **Strict mode** — native/dev builds with `HELIX_STRICT_BG_THREAD_CHECK=1` abort on hit, so
   a new instance fails the test suite. `HelixTestFixture` opts in by default.

**Release builds are exempt from the abort.** `HELIX_RELEASE_BUILD` (defined by the
`mk/cross.mk` cross-targets) compiles out the abort branch and ignores the env var. The
detector still emits the telemetry anomaly and a debug log, but it never crashes a user — a
Snapmaker U1 dev unit (`6d10417c`) hit a stray strict-mode abort on 2026-05-14 (sig
`307b6f48`), which is why the gating exists. Don't "fix" a missing abort in a release build.

**Per-line opt-out** (rare — only for dtor-joined worker threads with thread-private state):

```cpp
if (tok.expired()) return; // L081_OK: synchronous wait wrapper, dtor joins this thread
```

See `src/system/camera_stream.cpp` for legitimate examples.

**The comment silences the lint gate, not the runtime detector.** For a site that runs on a
background thread every launch, that leaves the anomaly channel emitting a hit per launch
forever. Use `expired_no_lvgl()` there — same atomic load, no report:

```cpp
// L081_OK: loop condition on the thread the owner joins; no LVGL below.
while (!poll_token.expired_no_lvgl() && running_.load()) { ... }
```

It is legitimate only where nothing after the check touches LVGL: a loop condition on a
dtor-joined thread, a buffer exclusive to that thread, or a sweep over some *other* object's
token. Anything that mutates a widget still owes you `expired()` + `defer()`. The lint gate
matches this spelling too, so it keeps watching the site — and it still wants the
`// L081_OK: <why>` note next to it.

Why it exists: the detector cannot distinguish these from the real anti-pattern, so all of
them reported. Over the 2026-08-09..18 telemetry window, four such sites (the WiFi
state-observer sweep plus three in `CameraStream`) were ~67% of all `bg_tok_expired_check`
volume — enough that a genuine Mechanism C hit would have been one event in 131.

### TOCTOU rule: `tok.defer()` vs `lifetime_.defer()`

- **From a background thread:** always `tok.defer()`. The token holds its own `shared_ptr` to
  the generation counter, so it is safe even while the owner is being destroyed.
- **From the main thread:** `lifetime_.defer()` is fine — `this` is guaranteed valid.

`lifetime_.defer()` on a background thread reads `this->lifetime_`, which is the #707 race.

### Cancel-and-retry

```cpp
lifetime_.invalidate();            // expire all outstanding tokens
auto tok = lifetime_.token();      // fresh token for the new attempt
api->test([this, tok]() { ... });
```

### Key properties

- `defer()` queues via `queue_update()` and silently skips if invalidated before it runs
- `token()` returns a `LifetimeToken` for non-queue callbacks (timers, state-machine handlers)
- Safe after owner destruction — tokens hold a `shared_ptr` to the counter, not a pointer to
  the owner
- The destructor calls `invalidate()` automatically

### Deprecated — do not use in new code

`shared_ptr<bool> callback_guard_` / `alive_guard_`, `shared_ptr<atomic<bool>> alive_`,
`shared_ptr<atomic<uint64_t>>` generation counters, `weak_ptr<bool>` for callback safety, and
`async_call(guard_widget, cb, data)` for modal/overlay guards. All replaced by
`AsyncLifetimeGuard`.

---

## 3. No synchronous widget deletion inside queued callbacks

Every synchronous widget deletion inside an UpdateQueue-drained callback is a latent #776
crash. Multiple sync deletions batched into one `process_pending()` corrupt LVGL's global
event linked list → SIGSEGV in `lv_event_mark_deleted` (#776, #190, #80).

**What counts as "inside a queued callback"** — everything that runs through
`process_pending()`:

- `helix::ui::queue_update(...)` / `ui_queue_update(...)` lambdas
- `helix::ui::async_call(cb, ud)` — our wrapper, **not** LVGL's native call
- `register_overlay_close_callback(...)` lambdas
- `AsyncLifetimeGuard::defer(...)` / `lifetime_.defer(...)` lambdas
- `LifetimeToken::defer(...)` / `tok.defer(...)` lambdas
- `observe_int_sync` / `observe_string` callbacks (deferred through `queue_update` since #82)

**Banned APIs and their replacements:**

| ❌ Banned inside queued callbacks | ✅ Use instead |
|-----------------------------------|----------------|
| `safe_delete(ptr)` | `safe_delete_deferred(ptr)` |
| `lv_obj_delete(obj)` | `lv_obj_delete_async(obj)` |
| `lv_obj_clean(container)` | `helix::ui::safe_clean_children(container)` |

**Why the replacements are safe:** they route deletion through `lv_obj_delete_async()`, which
posts to LVGL's *own* async list — processed at the end of `lv_timer_handler()`, after our
`process_pending()` returns. Deletions land one at a time across ticks instead of batched
inside our drain. `safe_clean_children()` reparents each child to `lv_layer_top()` and
async-deletes it, so the container appears empty immediately and callers can add new children
right away.

### `lifetime_.defer` does NOT escape the batch

The generation guard protects `this` against use-after-free. It does **not** move the callback
out of `process_pending()` — the callback runs in the *next* batch, which still contains
whatever else was queued for that tick. If you find a comment claiming
"SAFETY: defer runs outside process_pending()" next to a `lifetime_.defer`, the comment is
wrong; fix it.

```cpp
// ❌ CRASH — sync deletion inside an UpdateQueue batch
helix::ui::async_call([dialog]() {
    helix::ui::safe_delete(dialog);
});

// ❌ STILL CRASH — lifetime_.defer routes through queue_update
lifetime_.defer([this]() {
    lv_obj_clean(container_);
});

// ✅ CORRECT — replace the inner deletion; the outer defer is fine
lifetime_.defer([this]() {
    helix::ui::safe_clean_children(container_);
    rebuild(container_);
});

// ✅ CORRECT — single owned pointer
helix::ui::queue_update("cleanup", [this]() {
    helix::ui::safe_delete_deferred(overlay_);
});
```

**Rule of thumb:** inside any queued callback, treat sync widget deletion as banned. The outer
queue/defer is not the problem — the inner deletion is.

### True escape routes

These genuinely run outside UpdateQueue batches:

- `safe_delete_deferred()` / `safe_delete_deferred_raw()` (`include/ui_utils.h`)
- `helix::ui::safe_clean_children()` (`include/ui_utils.h`)
- `helix::ui::safe_delete_subtree(obj)` (`include/ui_utils.h`) — teardown-safe deletion of a
  whole grid/flex subtree before a rebuild. Synchronously detaches `obj` into an off-tree,
  layout-less condemned container (so an ancestor relayout of the original parent can no
  longer iterate it), sets `LV_LAYOUT_NONE` on `obj`, then async-deletes the condemned
  subtree. This makes a `grid_update` / `flex_update` pass over a being-deleted subtree
  *structurally impossible* rather than merely time-shifted — the #983 teardown counterpart,
  for relayout racing the teardown of a grid during a modal close or panel rebuild.
- `lv_obj_delete_async(obj)` — raw LVGL
- `lv_async_call(cb, ud)` — raw LVGL, **not** our `helix::ui::async_call` wrapper

---

## 4. `ScopedFreeze` for drain + destroy

When destroying widgets that may have pending deferred callbacks, freeze the queue. This
closes the race where a background thread enqueues between `drain()` and destruction:

```cpp
auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
helix::ui::UpdateQueue::instance().drain();
lv_obj_clean(container);       // or safe_delete(), lv_obj_delete()
// freeze thaws on scope exit
```

### The freeze buffers; it no longer drops

`tok.defer(...)` and `queue_update(...)` both route through `UpdateQueue::queue()`. During a
`scoped_freeze()` window, callbacks are diverted into `frozen_buffer_` instead of `pending_`.
When the last `ScopedFreeze` destructs, the buffer is spliced back into `pending_` and the
work fires on the next `process_pending()` tick.

This is safe even though the freeze exists to prevent enqueueing against widgets being torn
down: the apply side gates on `AsyncLifetimeGuard`'s generation counter. If the owner died
during the freeze (drain → destroy → freeze releases → buffer splices into pending), the
deferred body sees `gen->load() != snapshot` and no-ops. The freeze still serializes
background threads against drain+destroy — it just no longer loses the work.

**There is one path, not two.** Code before 2026-05-11 used a separate
`tok.defer_critical(...)` / `queue_critical(...)` API to bypass the freeze for first-fire
baseline state that the rest of the app waited on. That API has been **removed**: with
buffer-not-drop, plain `defer` already preserves first-fire callbacks across the freeze
window.

```cpp
client_->register_notify_update(
    [this, token = lifetime_.token()](const json& notification) {
        // No special variant needed — plain defer buffers across the freeze.
        token.defer("Backend::notify_update", [this, notification]() {
            handle_status_update(notification);
        });
    });
```

`shut_down_` still drops, because post-shutdown enqueues are unrecoverable. A
`[UpdateQueue] DROPPED (shutdown): <tag>` line in a device log means a background thread is
enqueueing after `update_queue_shutdown()` ran — that is a real bug, not noise.

---

## 5. Subject lifecycle

- **Creation:** in each class's `init_subjects()` (`PrinterState::init_subjects()`,
  `HomePanel::init_subjects()`, …)
- **Lifetime:** app runtime for static subjects; until hardware changes for dynamic ones
- **Updates:** `lv_subject_set_*()` from the main thread only; background threads go through
  `queue_update()`
- **Cleanup:** each `init_subjects()` self-registers its `deinit_subjects()` (see §7)

### Static vs dynamic

**Static subjects** exist for the whole app run and need no lifetime token — e.g. the
primary-fan `get_fan_speed_subject()` (the genuinely no-argument overload). Teardown across a
soft restart is covered by `ObserverGuard`'s invalidation epoch instead (§6).

**Dynamic subjects** are destroyed and recreated when hardware is rediscovered after a
disconnect. Observing one without a `SubjectLifetime` is a use-after-free:
`lv_subject_deinit()` frees the subject's observer list, but `ObserverGuard` still holds a
dangling pointer into it.

| Source | Method |
|--------|--------|
| `PrinterFanState` | `get_fan_speed_subject(name, lifetime)` — per-fan speeds |
| `TemperatureSensorManager` | `get_temp_subject(name, lifetime)` — per-sensor temperatures |
| `PrinterTemperatureState` | `get_extruder_temp_subject(name, lifetime)`, `get_extruder_target_subject(name, lifetime)` |

The bed and chamber subjects sit between the two: they are owned directly by
`PrinterTemperatureState` for its whole life, but `init_subjects()` / `deinit_subjects()` do
create and expire real tokens for them, and token'd overloads exist
(`get_bed_temp_subject(lifetime)`, `get_bed_target_subject(lifetime)`, and the chamber
equivalents in `include/printer_temperature_state.h`). Use the token'd overload when you
create an observer; the no-argument one is for one-shot reads.

### The pairing rule: hand the token to `observe_*`

`SubjectLifetime` is `std::shared_ptr<bool>` (`include/ui_observer_guard.h`), and every
token'd accessor is an **out-param that hands you a copy of a token the owner keeps**:

```cpp
// src/sensors/temperature_sensor_manager.cpp, TemperatureSensorManager::get_temp_subject()
lifetime = it->second->lifetime;   // copy of the manager's own shared_ptr
```

The same shape is in `PrinterFanState::get_fan_speed_subject()`
(`src/printer/printer_fan_state.cpp`) and `PrinterTemperatureState::get_extruder_temp_subject()`
(`src/printer/printer_temperature_state.cpp`).

Death is signalled by the **value**, not by the refcount. Before deiniting a subject the owner
writes `*lifetime = false` and only then drops its own copy — `printer_fan_state.cpp`
(orphaned-fan sweep), `temperature_sensor_manager.cpp` (`// Signal death (#816)`),
`printer_temperature_state.cpp` (`init_extruders()` / `deinit_subjects()`).
`ObserverGuard::reset()` treats either signal as death:

```cpp
auto locked = alive_token_.lock();
subject_dead = !locked || !*locked;   // include/ui_observer_guard.h
```

**The rule that actually matters: if you fetch a token, you must pass it to the `observe_*`
factory.**

```cpp
// ❌ Token fetched, never handed to the observer
SubjectLifetime lt;
auto* s = tsm.get_temp_subject(name, lt);
obs_ = observe_int_sync<Panel>(s, this, handler);          // lifetime arg defaults to {}

// ✅
obs_ = observe_int_sync<Panel>(s, this, handler, lt);
```

The lifetime parameter of `observe_int_sync` / `observe_string` / `AnimatedValue::bind` defaults
to `{}` (`include/observer_factory.h`), so omitting it compiles silently. The guard's
`has_alive_token_` stays `false`, `subject_dead` can therefore never become `true`, and
`reset()` calls `lv_observer_remove()` on a subject `lv_subject_deinit()` already freed.

### Local or member? Not what decides correctness

An earlier revision of this section claimed that a caller-local `SubjectLifetime` paired with a
*member* `ObserverGuard` was itself a use-after-free, and the "2026-04-22 audit found no
violations" note rested on that claim. **Both were wrong.** The owner keeps its copy of the
token, so the caller's copy falling off the stack does not drop the refcount to zero and does
not expire the guard's `weak_ptr`.

Correct-today examples that the old rule would have flagged:
`FanStackWidget::bind_fan_observer()` (`src/ui/panel_widgets/fan_stack_widget.cpp`),
`ControlsPanel::subscribe_to_secondary_temp_subjects()` (`src/ui/ui_panel_controls.cpp`),
`FanControlOverlay::subscribe_to_fan_speeds()` (`src/ui/ui_fan_control_overlay.cpp`),
`src/ui/widgets/power_device_widget.cpp`.

Parallel **members** are still the shape to reach for, but for weaker reasons than "otherwise it
crashes": it is self-documenting at the declaration site that the observer sits on a dynamic
subject, and it stays correct if the token ever becomes exclusively owned by the observing code
(next section).

### Declaration and reset order: either works, given owner-held tokens

For every token in the tree today, order is **not** load-bearing. The caller's copy is never the
last reference while the subject is alive, so destroying it first does not expire the guard's
`weak_ptr` and destroying it last changes nothing. Both of these shipped shapes are safe:

```cpp
// include/ui/temperature_observer_bundle.h — lifetimes declared FIRST,
// so reverse-declaration order destroys the observers first.
SubjectLifetime nozzle_temp_lifetime_;
ObserverGuard   nozzle_temp_observer_;

// include/ui_panel_controls.h — lifetimes declared AFTER the observers,
// so the tokens are destroyed first.
std::vector<ObserverGuard>   secondary_fan_observers_;
std::vector<SubjectLifetime> secondary_fan_lifetimes_;
```

Order **would** matter for a token the observing code exclusively owns — one it created with
`make_shared<bool>` with nobody else holding a copy. No such token exists today: every
`make_shared<bool>` lifetime token in the tree is created by a subject *owner*
(`grep -rn 'make_shared<bool>' src include`). If you introduce one, destroy the **observer
first, token second** — i.e. declare the token **before** the observer:

- **Observer first (token still alive):** `lock()` succeeds, `*token == true`, so
  `lv_observer_remove()` runs against a live subject. Correct.
- **Token first:** the `weak_ptr` expires, `reset()` *skips* `lv_observer_remove()`, and a live
  observer is left orphaned on a live subject while its `LambdaObserverContext` is freed — UAF on
  the next notify. Same failure family the `created_epoch_` machinery exists to prevent
  (`include/ui_observer_guard.h`, debug bundles 449TVQ82 / X3RA4252).

When the subject is already dead, both orders are safe: the owner set `*token = false` before
freeing it, and that flag is visible regardless of who dropped their copy when.

### Collections (carousels, slot lists)

Parallel vectors, kept index-aligned, so the declaration reads as a pair:

```cpp
std::vector<ObserverGuard>   carousel_observers_;
std::vector<SubjectLifetime> carousel_lifetimes_;

// Clear
carousel_lifetimes_.clear();
carousel_observers_.clear();

// Add, in lockstep
auto& lt = carousel_lifetimes_.emplace_back();
auto* s = state.get_subject(name, lt);
carousel_observers_.push_back(observe_int_sync<Panel>(s, this, handler, lt));
```

Real usage: `ThermistorWidget::bind_carousel_sensors()` in
`src/ui/panel_widgets/thermistor_widget.cpp`. If a subject lookup fails, `pop_back()` the
lifetime slot so the two vectors stay aligned.

### Read-only access

If you only need to read a value once, use the no-token overload —
`tsm.get_temp_subject(name)` and `ps.get_extruder_temp_subject(name)` exist for exactly that, and
they make the intent obvious at the call site. Taking a token and never observing is harmless,
just misleading.

Real usage: `ThermistorWidget::update_display()` in
`src/ui/panel_widgets/thermistor_widget.cpp`.

### Klippy-volatile subjects

Moonraker sends **delta** status updates — changed fields only. A subject fed by a
delta-only field keeps its last value indefinitely across a Klipper restart, because the
field is simply absent from later payloads until it next changes. When a stale value like
that *gates behaviour*, it is a live bug rather than a cosmetic one.

That is #1129: a cached `idle_timeout.state == "Printing"`, captured mid-`G28`, survived a
Klipper restart. `is_blocking_operation_active()` therefore treated a freshly-restarted,
idle printer as busy, routed LED/fan/temp commands down the fire-and-forget queue path, and
left the LED in-flight counter pinned — both light buttons greyed out for the rest of the
session.

Declare such a subject with `INIT_SUBJECT_INT_VOLATILE` instead of `INIT_SUBJECT_INT`
(`include/state/subject_macros.h`). It registers the subject and its default into a
`helix::subjects::VolatileSubjects` member, writing that default exactly once so init and
reset cannot drift apart:

```cpp
INIT_SUBJECT_INT(retract_length, 0, subjects_, register_xml);                     // config-derived
INIT_SUBJECT_INT_VOLATILE(idle_timeout_printing, 0, subjects_, volatile_, register_xml);
```

`PrinterState::set_klippy_state_internal()` is the single chokepoint for every Klippy state
change — the webhooks JSON parse, the `helix::async::call_method` wrapper, and
`set_klippy_state_sync()` all funnel through it — and it calls `reset_klippy_volatile()`
on a genuine edge only.

**The reset is edge-triggered, in both directions**, and that is load-bearing: a live
`if (klippy_state != READY)` predicate would *not* have fixed #1129, because the stale value
survived past the return to READY. `READY → dead` means nothing Klipper was doing survives;
`dead → READY` means a fresh Klipper with nothing blocking yet.

**Membership is deliberately narrow.** Two rules:

- Only include a subject if its field is delta-only *and* a stale value gates behaviour.
  Continuously-streamed state (temperatures, positions) self-heals within a tick — resetting
  it just causes visible flicker.
- Only include it if the reset value is *more* correct than the stale one. `motors_enabled`
  was tried and removed for exactly this reason: right after a Klipper shutdown the steppers
  are affirmatively de-energized, so the stale `0` was truer than a reset to `1`.

Current members, all on `PrinterCalibrationState`: `idle_timeout_printing`,
`manual_probe_active`, `manual_probe_z_position`.

Real usage: `src/printer/printer_calibration_state.cpp`, dispatched from
`src/printer/printer_state.cpp`.

---

## 6. Observers

Create observers with the factories in `include/observer_factory.h` rather than raw
`lv_subject_add_observer()` plus `lv_observer_get_user_data()`:

| Factory | Use |
|---------|-----|
| `observe_int_sync<T>()` | int subject, callback deferred via `queue_update` |
| `observe_int_async<T>()` | int subject, explicitly async |
| `observe_string()` | string subject, callback deferred |
| `observe_string_async()` | string subject, explicitly async |
| `observe_print_state<T>()` | typed `PrintJobState` over the raw `print_state_enum` subject, deferred |
| `observe_print_state_immediate<T>()` | the same typing, firing synchronously like `observe_int_immediate` |
| `observe_print_lifecycle<T>()` | typed `PrintState` over the derived `print_lifecycle` subject |

All return an `ObserverGuard` (`include/ui_observer_guard.h`) for RAII removal.

The print factories are not sugar: `PrintJobState` and `PrintState` do not share
numbering past index 0, so pairing a factory with the wrong subject (or hand-casting the
subject's int) compiles and silently answers a different question. That mistake shipped
twice — see `architecture/05-printer-state.md` § "Reading print state: typed accessors,
not hand-cast ints".

### Deferred by default

`observe_int_sync` and `observe_string` **defer their callbacks** through `queue_update()` to
prevent re-entrant observer destruction crashes (#82). Use the `observe_int_immediate` /
`observe_string_immediate` variants **only** when you are certain the callback won't modify
observer lifecycle — no reassignment, no widget destruction.

### `reset()` is the default; `release()` almost never is

- **`reset()`** — for all normal cleanup: panel teardown, `LV_EVENT_DELETE` callbacks,
  repopulate paths. It already handles the shutdown case internally via the
  `s_invalidation_epoch` comparison (bumped by `ObserverGuard::invalidate_all()` after
  `StaticSubjectRegistry::deinit_all()`) and the `lv_is_initialized()` guard. The epoch
  replaced an older global `s_subjects_valid` boolean, which could not distinguish an observer
  created *during* a reinit window — on a live subject, so it must be removed — from one
  created before teardown.
- **`release()`** — *only* for the last pre-deinit cleanup inside
  `StaticSubjectRegistry::register_deinit()` callbacks, where the subject is already
  destroyed.

If you reason *"`release()` skips `lv_observer_remove()`, so it must be safer"* — that is the
exact misconception behind 17 separate #579 reports. Skipping the remove leaks the
`LambdaObserverContext` and corrupts rendering state. Don't write `release()` in new cleanup
code.

---

## 7. Shutdown: registries and ordering

C++ does not guarantee destruction order of statics across translation units. When
`lv_deinit()` runs it deletes widgets, which try to remove their observers from subjects. If
singleton subjects haven't been deinitialized first, that corrupts a linked list and crashes
in `lv_observer_remove`.

Two self-registration registries enforce the order:

| Registry | Purpose | Cleans up |
|----------|---------|-----------|
| `StaticPanelRegistry` | UI panels/overlays with widgets | EmergencyStopOverlay, StatusBar, Keypad, Wizard subjects |
| `StaticSubjectRegistry` | Core state singletons with subjects | PrinterState, AmsState, SettingsManager, FilamentSensorManager |

Order, in `Application::shutdown()`:

```
1. StaticPanelRegistry::destroy_all()     ← panels destroy their own subjects
2. StaticSubjectRegistry::deinit_all()    ← core singleton subjects deinitialized
3. lv_deinit()                            ← safe now: all observers disconnected
```

### Self-registration is mandatory

Each component's `init_subjects()` must register its own cleanup. Never register externally
(e.g. from `SubjectInitializer`) — co-locating init and cleanup is what prevents forgotten
registrations.

```cpp
void PrinterState::init_subjects() {
    if (subjects_initialized_) return;
    // ... create subjects ...
    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "PrinterState", [this]() { deinit_subjects(); });
}
```

`SubjectInitializer` only calls `init_subjects()`; it does not register cleanup.

### `deinit_subjects()` pattern

```cpp
void AmsState::deinit_subjects() {
    if (!initialized_) return;
    spdlog::debug("[AMS State] Deinitializing subjects");
    lv_subject_deinit(&ams_type_);
    lv_subject_deinit(&ams_action_);
    // ... every subject ...
    initialized_ = false;
}
```

Properties that matter: reverse (LIFO) registration order, idempotent via the `initialized_`
guard, panels before singletons, and always log for shutdown debugging.

**Logging in static destructors:** use `fprintf(stderr, ...)`, not spdlog — spdlog may
already be destroyed (the static destruction order fiasco again).

---

## 8. Threads and pools

### No `std::thread(...).detach()` for fire-and-forget work

On AD5M/CC1/MIPS32, `pthread_create` returns `EAGAIN` under thread exhaustion. The
`std::thread` constructor then throws `std::system_error`, which — propagating through an
LVGL C event-dispatch frame or a `noexcept` boundary — aborts the process with
`std::terminate without active exception`. The crashes look like unrelated code paths: #724
(wizard camera probe), #837 (debug-bundle upload), #811-adjacent (HTTP storm on RatOS).

| Workload | Use |
|----------|-----|
| HTTP: REST / API / timelapse / thumbnails / small uploads | `helix::http::HttpExecutor::fast().submit(fn)` (4 workers) |
| HTTP: bundles / gcode / large transfers | `helix::http::HttpExecutor::slow().submit(fn)` (1 worker) |
| sd-bus / BlueZ DBus call | `helix::bluetooth::BusThread::run_sync(fn)` |
| BT-over-RFCOMM / USB print / QR decode / device discovery | `try { std::thread([...]{}).detach(); } catch (const std::system_error& e) { /* toast + error callback */ }` |
| Long-lived worker (member variable, joined in dtor) | plain `std::thread` is fine |

The problem is one-shot **detached** spawns, not threads as such. Lambdas submitted to an
executor still run on a worker thread, so their callbacks still need `queue_update()` or
`tok.defer()` for any UI work.

**Before adding a new `std::thread`, grep for an existing managed pool covering that domain.**
Adding a raw detached spawn reintroduces the anti-pattern and will crash on the smallest
device you ship to. See `docs/devel/MOONRAKER_ARCHITECTURE.md` § "HTTP Work Execution
(HttpExecutor)".

### Never hold a subsystem lock across a call into another subsystem

Two managers that each take their own `recursive_mutex` and call each other form an ABBA
cycle the moment one of them calls out while holding its lock. TSan reports it as
`lock-order-inversion (potential deadlock)`. It has happened twice:
`AmsState` <-> `SpoolmanManager`, and `AmsState` <-> `FilamentSensorManager`.

**Established order: `AmsState` -> `FilamentSensorManager`.** AmsState may notify the sensor
layer while holding `mutex_`; the sensor layer must **not** hold its own lock when it queries
AmsState. The direction is forced by arithmetic, not taste: `AmsState` takes its lock at ~49
sites and `FilamentSensorManager` at ~2, so "the AMS lock is not held" is not a property
anyone can maintain, while "the sensor lock is not held" is.

Two shapes that keep a lock off an outbound call, both in
`src/print/filament_sensor_manager.cpp`:

```cpp
// 1. Hoist: read the other subsystem BEFORE taking your lock. Works when the
//    value is whole-printer rather than per-item.
const bool ams_active = AmsState::instance().is_filament_operation_active();
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ...use ams_active...
}

// 2. Snapshot: take the lock only long enough to copy what you need, then
//    decide outside it. Works when the query is per-item (has_real_runout()).
std::vector<Candidate> candidates;
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ...fill candidates from members...
}
for (const auto& c : candidates) { ...ask AmsState... }
```

**A `recursive_mutex` defeats the obvious fix.** Deferring the outbound call to the end of the
locking function does *not* release the lock when a caller above you already holds it:
`AmsState::sync_backend()` locks `mutex_` and then calls `sync_from_backend()`, so releasing
the inner acquisition leaves the outer one held and the cycle intact. Any fix anchored to one
scope inside a recursive lock has this hole. Fix the side that can actually guarantee the
invariant.

`make test-tsan` is the gate; a targeted run is `make test-tsan-one TEST="[ams]"`. Note that
both halves of a cycle must execute in the *same process* for TSan to see it, so a filter
narrow enough to exclude one of the two tests reports nothing.

---

## 9. No deletion during input event processing

Never call `lv_obj_delete()` on container children inside an input event callback
(`LV_EVENT_CLICKED`, `LV_EVENT_RELEASED`, anything dispatched from `indev_proc_release` /
`indev_proc_press`). LVGL may be iterating the parent's child list; synchronous deletion
corrupts the iteration state and `lv_obj_get_parent` dereferences freed memory → SIGSEGV.

```cpp
// ❌ CRASH — deletes a container child mid-iteration
void on_done_clicked(lv_event_t* e) {
    lv_obj_delete(overlay_);
    overlay_ = nullptr;
    rebuild_widgets();
}

// ✅ CORRECT — drop the reference; let the rebuild's lv_obj_clean() delete it
void on_done_clicked(lv_event_t* e) {
    overlay_ = nullptr;
    rebuild_widgets();
}
```

**When a rebuild follows:** individual `lv_obj_delete()` calls on container children are both
redundant and dangerous. Null the pointers and let `lv_obj_clean()` handle it — that runs
after input processing completes.

**When no rebuild follows:** use `lv_obj_delete_async()` or `helix::ui::safe_delete()`.

**Caveat:** `lv_obj_delete_async()` is *not* safe if a subsequent `lv_obj_clean()` on the
parent runs before the async fires — that is a double-free. Only use it when no parent cleanup
follows.

---

## 10. Timers

LVGL timers created with `lv_timer_create()` are **not** cleaned up automatically. Use
`LvglTimerGuard` (`include/ui_timer_guard.h`) for RAII, or delete manually in the destructor
behind an `lv_is_initialized()` guard.

```cpp
#include "ui_timer_guard.h"
LvglTimerGuard update_timer_;   // deleted on destruction
```

### A raw `lv_timer_t*` must also be cancelled in the destructor

`StaticPanelRegistry::destroy_all()` runs **before** `lv_deinit()` in `Application::shutdown()`
(§7), so any teardown path that destroys the owner without the explicit stop leaves the timer
armed in LVGL's timer list holding a freed `this` — the callback then fires on freed memory
(#1173, twice: the wizard auto-probe timer and the PID-calibration ETA timer). The rule: **a
raw `lv_timer_t*` cancelled in `cleanup()` must also be cancelled in the destructor.**

Share one `cancel_*_timer()` helper between both paths, and cancel with
`lv_timer_cancel_safe()` (`include/ui_timer_guard.h`) — it self-guards on
`lv_is_initialized()` and neuters the timer instead of unlinking it, so it is safe to call
from a destructor and from inside `lv_timer_handler()` (#750, #751). Exemplar:
`FlyingToasterScreensaver::cancel_timer()` in `src/ui/ui_screensaver.cpp`.

A `LifetimeToken`-guarded timer callback is the other valid answer — annotate those
`// TIMER_DTOR_OK: <reason>`.

**Gate:** `scripts/check_timer_destructor_cancel.py`, run by `scripts/quality-checks.sh`
(`--max-allowed 0`). The check is transitive: a destructor that calls a `cleanup()` /
`deinit_subjects()` which cancels the timer counts.

---

## 11. Testing

### Fixture hierarchy

| Fixture | Base | Provides |
|---------|------|----------|
| `HelixTestFixture` (`tests/helix_test_fixture.h`) | — | Drains UpdateQueue, resets `SystemSettingsManager` language, clears `ModalStack` |
| `LVGLTestFixture` (`tests/lvgl_test_fixture.h`) | `HelixTestFixture` | Headless DRM display + test screen |
| `XMLTestFixture` (`tests/test_fixtures.h`) | `LVGLTestFixture` | Per-instance `PrinterState` / `MoonrakerClient` / `MoonrakerAPI`, XML subject registration |

`HelixTestFixture`'s ctor and dtor both call `reset_all()`, which drains the update queue so
queued callbacks can't leak between tests. It also opts into
`HELIX_STRICT_BG_THREAD_CHECK`, so any new L081 instance aborts the suite.

XML subjects register into LVGL's global scope — per-test scopes are blocked by LVGL
internals. Each test's `init_subjects(true)` overwrites prior entries with fresh pointers, and
`XMLTestFixture`'s destructor tears the screen down *before* deinitializing subjects to avoid
dangling observer references.

### Cleanup order

Reverse of creation, and always drain before destroying widgets:

```cpp
~MyFixture() {
    UITest::cleanup();                    // first
    if (panel)   lv_obj_delete(panel);    // then widgets
    if (screen)  lv_obj_delete(screen);
    if (display) lv_display_delete(display);
}
```

Creating multiple LVGL UI instances in sequence segfaults if cleanup is incomplete — each test
must fully release its LVGL objects, subjects, and observers before the next fixture runs.

### Observer gotcha

`lv_subject_add_observer()` fires the callback **immediately** with the current value:

```cpp
lv_subject_add_observer(subject, callback, &count);
REQUIRE(count == 1);          // fired immediately!
state.set_value(new_value);
REQUIRE(count == 2);          // fired again on change
```

### Commands

```bash
make test-run      # parallel, excludes [slow] and hidden
make test-serial   # sequential, for debugging
make test-asan     # AddressSanitizer — UAF, leaks, overflows
make test-tsan     # ThreadSanitizer — data races, deadlocks
```

Always append `"~[.]"` when running by tag, to exclude hidden tests that may hang:

```bash
./build/bin/helix-tests "[connection]" "~[.]"
```

Relevant tags: `[state]` (subjects/observers), `[connection]` (WebSocket lifecycle),
`[application]` (lifecycle/shutdown), `[core]` (must-pass), `[slow]` (>500ms, excluded from
`test-run`).

---

## 12. Symptom index

| Symptom | Cause | Fix |
|---------|-------|-----|
| Crash on reconnect or panel rebuild in an observer callback | Dynamic subject observed with a token that was fetched but never passed to `observe_*` | Pass the token as the factory's `lifetime` argument (§5) |
| `lifetime_.defer()` from a background thread crashes | Reads `this->lifetime_` — TOCTOU (#707) | `tok.defer()` or `lifetime_.bg_cb()` (§2) |
| `std::terminate without active exception` on K1/AD5M/CC1 | Detached `std::thread` hit `EAGAIN` | Managed pool, or try/catch around the spawn (§8) |
| SIGSEGV in `lv_event_mark_deleted` | Sync widget deletion inside a queued callback | `*_deferred` / `_async` / `safe_clean_children` (§3) |
| App hangs, stops answering pings | `lv_subject_set_*()` from a background thread during render | `queue_update()` (§1) |
| Crash on shutdown in `lv_observer_remove` | `init_subjects()` never self-registered its cleanup | Register inside `init_subjects()` (§7) |
| Crash deleting a widget during a button click | Sync deletion during `indev` dispatch | Null the pointer, let the rebuild clean (§9) |
| Timer callback fires after its owner was destroyed | Raw `lv_timer_t*` cancelled in `cleanup()` but not the destructor | Share a `cancel_*_timer()` + `lv_timer_cancel_safe()` between both paths (§10) |
| Rendering corrupted / observer context leaked | `ObserverGuard::release()` used for normal cleanup (#579) | `reset()` (§6) |
| `lv_obj_delete_async()` double-free | Parent's `lv_obj_clean()` ran before the async fired | Only async-delete when no parent cleanup follows (§9) |
| `[UpdateQueue] DROPPED (shutdown): <tag>` in a device log | Background thread enqueueing after `update_queue_shutdown()` | Real bug — find the thread that outlived shutdown (§4) |
| Two panel widget instances, only one updates | Per-instance subject registered globally, or XML scope mismatch | Pick one: shared subject in component scope, or filter a shared subject by ID |
| TSan `lock-order-inversion (potential deadlock)` between two managers | One holds its own `recursive_mutex` across a call into the other | Hoist the read above the lock, or snapshot under it and decide outside (§8) |

---

## Key files

| File | Contents |
|------|----------|
| `include/ui_update_queue.h` | `UpdateQueue`, `queue_update()`, `scoped_freeze()` |
| `include/async_lifetime_guard.h` | `AsyncLifetimeGuard`, `LifetimeToken`, `bg_cb()` |
| `include/ui_observer_guard.h` | `ObserverGuard`, `SubjectLifetime` |
| `include/observer_factory.h` | `observe_int_sync/async`, `observe_string/async`, `observe_print_state[_immediate]`, `observe_print_lifecycle` |
| `include/ui_utils.h` | `safe_delete_deferred`, `safe_clean_children`, `safe_delete_subtree` |
| `include/static_subject_registry.h` | Shutdown cleanup registry |
| `include/http_executor.h` | `HttpExecutor::fast()` / `slow()` pools |
| `src/bluetooth/bt_bus_thread.h` | BlueZ `BusThread` |
| `include/ui_timer_guard.h` | `LvglTimerGuard` RAII |
| `include/ui_widget_memory.h` | `lvgl_unique_ptr` / `lvgl_make_unique` |
| `src/printer/printer_state.cpp` | `set_*_internal()` reference pattern |
| `src/api/wifi_manager.cpp` | Backend integration reference |
| `src/application/application.cpp` | Main loop and shutdown order |
| `scripts/check_l081_anti_pattern.py` | L081 lint gate |
| `scripts/check_timer_destructor_cancel.py` | Timer dtor-cancellation lint gate |
