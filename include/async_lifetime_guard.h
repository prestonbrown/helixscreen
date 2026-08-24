// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file async_lifetime_guard.h
 * @brief Generation-counter-based async callback safety
 *
 * Provides a lightweight mechanism to detect whether an object is still valid
 * when a deferred callback fires. The guard lives in the protected object;
 * lambdas capture a LifetimeToken (which holds a shared_ptr to the generation
 * counter, NOT a pointer to the guard itself). This makes the check safe even
 * if the guard has been destroyed before the callback executes.
 *
 * Usage:
 * @code
 * class MyOverlay {
 *     helix::AsyncLifetimeGuard guard_;
 *
 *     void start_async_work() {
 *         // Callback will silently skip if overlay is dismissed before it fires
 *         guard_.defer("MyOverlay::on_result", [this]() {
 *             update_ui_with_result();
 *         });
 *     }
 *
 *     ~MyOverlay() {
 *         // guard_ destructor calls invalidate(), expiring all outstanding tokens
 *     }
 * };
 * @endcode
 */

#pragma once

#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace helix {

class AsyncLifetimeGuard;

// Forward declaration so the skip paths inside AsyncLifetimeGuard's template
// methods can call into the counter module. Full declarations (SkipEntry,
// SkipSnapshot, take_snapshot, note_skipped's signature)
// live at the bottom of this file.
namespace async_lifetime {
void note_skipped(const char* tag) noexcept;
} // namespace async_lifetime

namespace internal {

/// Capture the calling thread as the "main" thread (the LVGL/render thread).
/// Call exactly once, as the very first thing in main(), before any thread
/// that uses LifetimeToken can spawn. Subsequent calls are silently ignored.
void set_main_thread_id() noexcept;

/// Returns true if the calling thread is the recorded main thread, OR if
/// set_main_thread_id() has not yet run (returns true conservatively during
/// early init so the bg-thread detector doesn't fire false positives).
bool on_main_thread() noexcept;

/// Report a "tok.expired() called from a bg thread while owner is alive"
/// anti-pattern hit. Per-thread first-fire-only via a small TLS seen-set,
/// so each unique callsite (LR) reports at most once per thread per session.
///
/// Captures `__builtin_return_address(0)` *inside* this function (which is
/// noinline). With `expired()` always inlined into its caller F, the
/// trampoline's caller is F itself, and the captured LR is the position in
/// F where the inlined `tok.expired()` lives — i.e. the original source
/// location. If we captured the LR in the inline `expired()` and passed it
/// in, the compiler would substitute F's caller's LR after inlining, which
/// is the wrong frame for the audit. Resolve the captured LR with addr2line.
///
/// In strict mode (HELIX_STRICT_BG_THREAD_CHECK=1 env or
/// set_strict_bg_check(true)), this aborts after warning so CI / tests
/// fail fast on any new instance of the anti-pattern.
void report_bg_expired_check() noexcept;

/// Enable strict mode programmatically — meant for HelixTestFixture so test
/// runs assert any L081 anti-pattern instead of silently warning. Production
/// code should never call this; only the env var path is safe in user builds.
void set_strict_bg_check(bool enabled) noexcept;

} // namespace internal

/**
 * @brief Lightweight, copyable token captured in lambdas to check validity
 *
 * Holds a shared_ptr to the generation counter (NOT to the guard), so the
 * token remains safe to query even after the guard is destroyed.
 */
class LifetimeToken {
  public:
    /**
     * @brief Check if the generation has advanced past the snapshot
     * @return true if the owning object has been invalidated/destroyed
     *
     * 3XNZQB2R / cluster:pstat-async-delete Mechanism C detector: if the
     * owner is still alive (returning false) AND we're on a non-main
     * thread, flag the callsite as a likely L081 anti-pattern hit
     * (`tok.expired()` check followed by inline LVGL mutation on a bg
     * thread). The correct pattern is `tok.defer([this]() { ... })`,
     * which marshals the body to the main thread before the check.
     * See `feedback_token_defer_required.md` and the audit pending in
     * `project_l081_recurrence_post_840.md`.
     */
    [[gnu::always_inline]] inline bool expired() const {
        bool is_expired = gen_->load(std::memory_order_acquire) != snapshot_;
        if (!is_expired && !internal::on_main_thread()) {
            // LR capture is inside report_bg_expired_check() (noinline) so
            // that the captured address is the position in our caller where
            // this inline expired() lives — see comment on the declaration.
            internal::report_bg_expired_check();
        }
        return is_expired;
    }

    /**
     * @brief Liveness check whose background-thread use is already audited
     * @return true if the owning object has been invalidated/destroyed
     *
     * `expired()` minus the Mechanism C report. Reach for this ONLY where the
     * code after the check mutates no LVGL state — a loop condition on a
     * thread the owner joins, a buffer exclusive to that thread, or a sweep
     * over some *other* object's token. Anything that goes on to touch a
     * widget still owes you `expired()` + `defer()`; renaming the call to
     * quiet a warning is how Mechanism C comes back. Pair each use with a
     * `// L081_OK: <why>` note — the static gate in
     * scripts/check_l081_anti_pattern.py reads this spelling too.
     *
     * Why it exists: the runtime detector cannot tell these apart from the
     * real anti-pattern, so every one of them reported. Over the 2026-08-09..18
     * telemetry window they were ~67% of all `bg_tok_expired_check` volume,
     * which left no room for a genuine hit to stand out.
     */
    inline bool expired_no_lvgl() const {
        return gen_->load(std::memory_order_acquire) != snapshot_;
    }

    /**
     * @brief Convenience: true if NOT expired (object still alive)
     *
     * Routes through expired(), so `if (tok)` on a background thread reports
     * the same way a bare `tok.expired()` does.
     */
    explicit operator bool() const {
        return !expired();
    }

    /**
     * @brief Queue a guarded callback without accessing the owning object
     *
     * Use this from background thread callbacks instead of lifetime_.defer()
     * to avoid a TOCTOU race where `this` (and thus `lifetime_`) is destroyed
     * between the tok.expired() check and the lifetime_.defer() call (#707).
     *
     * The token holds its own shared_ptr to the generation counter, so it
     * remains safe to use even after the guard (and its owner) are destroyed.
     */
    // file/line default-evaluate at the call site so an untagged defer still
    // names its producer in a cross-test leak report (see TaggedCallback).
    template <typename F>
    void defer(F&& fn, const char* file = __builtin_FILE(), int line = __builtin_LINE()) const {
        auto gen = gen_;
        auto snapshot = snapshot_;
        // See the tagged overload: an already-expired token enqueues a callback
        // that can only skip, so drop it here instead of parking it in the queue.
        if (gen->load(std::memory_order_acquire) != snapshot) {
            helix::async_lifetime::note_skipped(nullptr);
            return;
        }
        helix::ui::queue_update(
            [gen, snapshot, f = std::forward<F>(fn)]() mutable {
                if (gen->load(std::memory_order_acquire) != snapshot)
                    return;
                f();
            },
            file, line);
    }

    /// Tagged variant for crash diagnostics
    template <typename F> void defer(const char* tag, F&& fn) const {
        auto gen = gen_;
        auto snapshot = snapshot_;
        // Already dead before we even enqueue — the body below would load the
        // same generation and skip, so queueing it buys nothing and costs a
        // slot in the queue that outlives the owner. This is the common shape
        // for a debounced background worker (sleep, then defer) whose owner was
        // destroyed while it slept: AmsBackendAd5xIfs::schedule_zcolor_query
        // holds a 500ms sleep on an HttpExecutor lane, and every backend torn
        // down inside that window used to leave a doomed callback behind.
        // Checking here is safe on any thread — the token owns a shared_ptr to
        // the counter and never touches the guard or its owner.
        if (gen->load(std::memory_order_acquire) != snapshot) {
            helix::async_lifetime::note_skipped(tag);
            spdlog::trace("[LifetimeToken] Skipped expired callback before enqueue: {}",
                          tag ? tag : "unknown");
            return;
        }
        helix::ui::queue_update(tag, [gen, snapshot, tag, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                helix::async_lifetime::note_skipped(tag);
                spdlog::trace("[LifetimeToken] Skipped expired callback: {}",
                              tag ? tag : "unknown");
                return;
            }
            f();
        });
    }

  private:
    friend class AsyncLifetimeGuard;

    LifetimeToken(std::shared_ptr<std::atomic<uint64_t>> gen, uint64_t snapshot)
        : gen_(std::move(gen)), snapshot_(snapshot) {}

    std::shared_ptr<std::atomic<uint64_t>> gen_;
    uint64_t snapshot_;
};

/**
 * @brief Owned by the protected object; produces tokens and defers callbacks
 *
 * Non-copyable, non-movable. Destructor calls invalidate() to expire all
 * outstanding tokens. The defer() methods capture a shared_ptr to the
 * generation counter (not `this`), so the lambda is safe even if the guard
 * is destroyed before it fires.
 */
class AsyncLifetimeGuard {
  public:
    AsyncLifetimeGuard() : gen_(std::make_shared<std::atomic<uint64_t>>(0)) {}

    ~AsyncLifetimeGuard() {
        invalidate();
    }

    // Non-copyable, non-movable
    AsyncLifetimeGuard(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard& operator=(const AsyncLifetimeGuard&) = delete;
    AsyncLifetimeGuard(AsyncLifetimeGuard&&) = delete;
    AsyncLifetimeGuard& operator=(AsyncLifetimeGuard&&) = delete;

    /**
     * @brief Capture the current generation as a token
     *
     * The token can be copied into lambdas and checked later. It will report
     * expired() == true once invalidate() is called (or the guard is destroyed).
     */
    LifetimeToken token() const {
        return LifetimeToken(gen_, gen_->load(std::memory_order_acquire));
    }

    /**
     * @brief Advance the generation counter, expiring all outstanding tokens
     *
     * Safe to call multiple times. Each call expires tokens from every
     * previous generation.
     */
    void invalidate() {
        gen_->fetch_add(1, std::memory_order_release);
    }

    /**
     * @brief Queue a callback that is skipped if the guard has been invalidated
     *
     * Captures a shared_ptr to the generation counter and a snapshot of the
     * current generation. When the callback fires, it compares the snapshot
     * to the current generation; if they differ, the callback is silently
     * skipped.
     *
     * @tparam F Callable with signature void()
     * @param fn The callback to defer
     */
    template <typename F>
    void defer(F&& fn, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update(
            [gen, snapshot, f = std::forward<F>(fn)]() mutable {
                if (gen->load(std::memory_order_acquire) != snapshot) {
                    return;
                }
                f();
            },
            file, line);
    }

    /**
     * @brief Queue a tagged callback that is skipped if the guard has been invalidated
     *
     * Same as defer(fn), but the tag is passed to the UpdateQueue for crash
     * diagnostics. If the callback is skipped, a trace log is emitted with the tag.
     *
     * @tparam F Callable with signature void()
     * @param tag String literal identifying the caller (for crash diagnostics)
     * @param fn The callback to defer
     */
    template <typename F> void defer(const char* tag, F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        helix::ui::queue_update(tag, [gen, snapshot, tag, f = std::forward<F>(fn)]() mutable {
            if (gen->load(std::memory_order_acquire) != snapshot) {
                helix::async_lifetime::note_skipped(tag);
                spdlog::trace("[AsyncLifetimeGuard] Skipped expired callback: {}",
                              tag ? tag : "unknown");
                return;
            }
            f();
        });
    }

    /**
     * @brief Wrap a background-thread callback so its body always fires on the main thread
     *
     * Returns a callable suitable to pass directly to HTTP / WebSocket / HttpExecutor /
     * std::thread APIs. When the underlying API invokes the returned callable on a bg
     * thread, the supplied `fn` is queued via `defer(tag, ...)` — fn always runs on the
     * main thread, after a generation-guard re-check, with `this` guaranteed valid.
     * Arguments are forwarded by-value (decayed) into the deferred lambda so callers
     * never see a dangling reference to a temporary on the bg-thread side.
     *
     * Use this in preference to writing `[this, tok = lifetime_.token()](...) {
     * if (tok.expired()) return; ... }` by hand — that idiom is the L081 anti-pattern
     * the strict-mode detector aborts on.
     *
     * Pattern:
     * @code
     * api_->rest().wled_get_strips(
     *     lifetime_.bg_cb("LedController::wled_get_strips",
     *                     [this](const RestResponse& resp) {
     *                         // runs on main; safe to touch members
     *                         apply_strips(resp);
     *                     }),
     *     [](const MoonrakerError& err) { ... });
     * @endcode
     *
     * If your callback wants to do bg-side work first (parsing JSON before deferring
     * member mutations), keep using the longhand `tok.defer("Tag", [...] { ... })`
     * pattern explicitly — bg_cb defers the *whole* call, so it trades minimum-bg-work
     * for minimum-syntax-overhead.
     *
     * @tparam F  Callable invoked on the main thread when the wrapper fires
     * @param tag  String literal identifying the caller (crash diagnostics)
     * @param fn   The callback body — runs on main thread inside the defer
     */
    template <typename F> auto bg_cb(const char* tag, F&& fn) {
        auto gen = gen_;
        auto snapshot = gen_->load(std::memory_order_acquire);
        return [gen, snapshot, tag, fn = std::forward<F>(fn)](auto&&... args) mutable {
            // Owner already gone: skip before enqueueing (see LifetimeToken::defer).
            if (gen->load(std::memory_order_acquire) != snapshot) {
                helix::async_lifetime::note_skipped(tag);
                spdlog::trace("[AsyncLifetimeGuard] Skipped expired bg_cb before enqueue: {}",
                              tag ? tag : "unknown");
                return;
            }
            // Decay args into a value-tuple so the deferred body can't observe
            // references that died with the bg-thread stack frame.
            auto args_tuple = std::make_tuple(
                std::decay_t<decltype(args)>(std::forward<decltype(args)>(args))...);
            helix::ui::queue_update(
                tag, [gen, snapshot, tag, fn, t = std::move(args_tuple)]() mutable {
                    if (gen->load(std::memory_order_acquire) != snapshot) {
                        helix::async_lifetime::note_skipped(tag);
                        spdlog::trace("[AsyncLifetimeGuard] Skipped expired bg_cb: {}",
                                      tag ? tag : "unknown");
                        return;
                    }
                    std::apply([&fn](auto&&... a) { fn(std::forward<decltype(a)>(a)...); },
                               std::move(t));
                });
        };
    }

  private:
    std::shared_ptr<std::atomic<uint64_t>> gen_;
};

} // namespace helix

// ============================================================================
// Skip-rate telemetry — declared in this header because the three skip paths
// below are the only producers. See `src/system/async_lifetime_guard.cpp` for
// the implementation.
//
// Every deferred callback that fires with an invalidated generation counter
// increments a per-tag counter via `note_skipped(tag)`. `TelemetryManager`
// drains the counters on its periodic snapshot timer and emits an
// `async_lifetime_skips` event with the per-tag breakdown. A hot producer in
// that breakdown is the early signal that an owner is repeatedly dying with
// pending work — the shape of bug 5KNWUEKY before it crashes (#1165 close
// commentary).
//
// Tags are string-literal pointers (per `UpdateQueue::TaggedCallback`'s
// contract), so the counter interns by pointer equality — no hashing, no
// allocation, no string-lifetime concerns. Bounded at `MAX_TRACKED_TAGS` slots;
// overflow rolls into an "(other)" bucket. Slots are released again whenever a
// window drains them to zero, so the bound is on tags active *per window*, not
// on tags seen over the life of the process.
// ============================================================================

namespace helix::async_lifetime {

/// Number of distinct producer tags tracked within one snapshot window before
/// overflow rolls into the "(other)" bucket. 64 keeps the counter store under
/// 1 KB and the linear-scan hot path cache-friendly. Slot count is the only
/// knob — tune if a real device shows >64 distinct producers skipping inside a
/// single window (which would itself be a signal).
constexpr size_t MAX_TRACKED_TAGS = 64;

/// Increment the skip counter for `tag`. Lock-free on the repeating-tag hot
/// path (linear scan over cache-friendly slots); the first sighting of a tag
/// within a window claims a free slot by CAS. `nullptr` is normalised to
/// "(null)". Safe from any thread — and it does get called from background
/// threads: each defer path checks the generation BEFORE enqueueing (so a
/// doomed callback never occupies a queue slot), and that pre-check runs on
/// whichever thread deferred. The post-enqueue check inside the queued body
/// still fires on the LVGL main thread.
void note_skipped(const char* tag) noexcept;

struct SkipEntry {
    std::string tag;
    uint64_t count;
};

struct SkipSnapshot {
    /// Per-tag counts, sorted descending. Includes the synthetic "(other)"
    /// entry when the counter store has overflowed.
    std::vector<SkipEntry> entries;
    /// Sum of every count observed in this window, including the "(other)"
    /// bucket — the headline rate number.
    uint64_t total = 0;
    /// Count rolled into the "(other)" bucket (also present in `entries`).
    uint64_t other_count = 0;
};

/// Main-thread-only. Returns the current counters sorted by count descending
/// and atomically resets each slot to zero — the returned deltas represent
/// the window since the previous call. Every slot is released on drain, so the
/// table only ever holds tags that skipped in the current window and a producer
/// that turns hot late can always be named. Call from `TelemetryManager`'s
/// periodic snapshot timer. Discarding the result is also how a caller clears
/// the store outright — the drain is unconditional, so there is no separate
/// reset entry point to keep in sync with it.
SkipSnapshot take_snapshot() noexcept;

} // namespace helix::async_lifetime
