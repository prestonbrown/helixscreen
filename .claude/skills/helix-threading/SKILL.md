---
name: helix-threading
description: >
  HelixScreen threading & lifecycle safety. Use when writing or reviewing code that crosses the
  main-thread/background-thread boundary (WebSocket/libhv callbacks, src/network/ HTTP workers,
  src/bluetooth/ DBus threads, src/printer/ background state updates), observes a subject, deletes
  widgets from a callback, owns an lv_timer_t, or spawns a thread. Key types: UpdateQueue /
  queue_update, AsyncLifetimeGuard, SubjectLifetime, ObserverGuard, safe_delete_deferred /
  safe_clean_children, StaticSubjectRegistry, HttpExecutor, lv_subject_t touched off the main thread.
---

# HelixScreen Threading & Lifecycle Safety

`docs/devel/THREADING.md` is the single source of truth: full patterns, code examples,
enforcement and a symptom index. Read the section for what you are touching before writing
the code. This skill routes you there.

These bugs compile cleanly and crash later, usually on a customer's printer. Field crashes on
K1/AD5M/CC1 concentrate in `src/network/`, `src/bluetooth/`, `src/printer/`, and in lifecycle
code under `src/ui/`.

## The invariants

The five core invariants live in `.claude/rules/threading.md`, which loads automatically for
`src/`, `include/` and `tests/unit/`. In short:

1. No LVGL from a background thread, `lv_subject_set_*` included: `helix::ui::queue_update()` (§1).
2. No bare `if (tok.expired()) return;` before touching `this` on a background thread:
   `lifetime_.bg_cb(tag, fn)` or `tok.defer(tag, fn)` (§2).
3. No synchronous deletion inside a queued callback: `safe_delete_deferred`,
   `lv_obj_delete_async`, `safe_clean_children` (§3).
4. Hand `observe<V>` the `SubjectLifetime` the accessor filled, never a default-constructed one (§5).
5. A raw `lv_timer_t*` cancelled in `cleanup()` is cancelled in the destructor too, via a shared
   `cancel_*_timer()` and `lv_timer_cancel_safe()` (§10).

Three more that the rule file covers in one line or not at all:

| Never | Instead | Section |
|-------|---------|---------|
| `std::thread(...).detach()` for one-shot work (`EAGAIN` becomes `std::terminate`) | `HttpExecutor::fast()/slow()`, `BusThread`, or try/catch around the spawn where exceptions are allowed | §8 |
| Delete container children inside an input event handler | Null the pointer and let the rebuild clean | §9 |
| `ObserverGuard::release()` in normal cleanup | `reset()` | §6 |

Every `init_subjects()` self-registers its `deinit_subjects()` with `StaticSubjectRegistry` (§7).

Commit-time gates (`scripts/quality-checks.sh`): `scripts/check_l081_anti_pattern.py` (#2),
`scripts/check_timer_destructor_cancel.py` (#5), `scripts/check_subscription_null_safety.py`.

## Two things that surprise people

**`lifetime_.defer` does not escape the UpdateQueue batch.** It wraps `queue_update`, so the
callback fires in the next `process_pending` tick, which is still a batch that may hold other
sync deletions. The generation guard protects `this` from use-after-free, not the event list
from corruption. (§3)

**From a background thread, use `tok.defer()`, never `lifetime_.defer()`.** The latter reads
`this->lifetime_`, which is the TOCTOU race (#707). `lifetime_.defer()` is main-thread only. (§2)

## Section map: `docs/devel/THREADING.md`

| Section | Covers |
|---------|--------|
| §1 LVGL is single-threaded | UpdateQueue, main-loop order, why not `lv_async_call`, backend pattern, threading model |
| §2 Async callback safety | `AsyncLifetimeGuard`, `bg_cb` vs `tok.defer`, L081 enforcement layers, release-build carve-out |
| §3 No sync widget deletion | What counts as queued, banned-to-replacement table, true escape routes |
| §4 `ScopedFreeze` | Drain + destroy, buffer-not-drop |
| §5 Subject lifecycle | Static vs dynamic subjects, local vs member lifetimes, reset ordering, collections |
| §6 Observers | `core/observer_factory.h` factories, deferred-by-default (#82), `reset()` vs `release()` (#579) |
| §7 Shutdown | Registries, ordering, self-registration, `deinit_subjects()` |
| §8 Threads and pools | No detached spawns, workload-to-pool table |
| §9 Input event processing | No sync deletion during `indev` dispatch |
| §10 Timers | `LvglTimerGuard`, `lv_timer_cancel_safe()` |
| §11 Testing | Fixture hierarchy, cleanup order, observer immediate-fire gotcha, asan/tsan |
| §12 Symptom index | What you're seeing, then cause, then fix |
