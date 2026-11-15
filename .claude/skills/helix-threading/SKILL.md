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

The five invariants are in `.claude/rules/threading.md`, which loads automatically for `src/`,
`include/` and `tests/unit/`; read it if this file is not where you are working.

Three more that the rule file covers in one line or not at all:

| Never | Instead | Section |
|-------|---------|---------|
| `std::thread(...).detach()` for one-shot work (`EAGAIN` becomes `std::terminate`) | `HttpExecutor::fast()/slow()`, `BusThread`, or try/catch around the spawn where exceptions are allowed | §8 |
| Delete container children inside an input event handler | Null the pointer and let the rebuild clean | §9 |
| `ObserverGuard::release()` in normal cleanup | `reset()` | §6 |

Every `init_subjects()` self-registers its `deinit_subjects()` with `StaticSubjectRegistry` (§7).

Commit-time gates (`scripts/quality-checks.sh`): `scripts/check_l081_anti_pattern.py` (#2),
`scripts/check_timer_destructor_cancel.py` (#5), `scripts/check_subscription_null_safety.py`.

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
