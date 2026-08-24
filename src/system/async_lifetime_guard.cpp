// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file async_lifetime_guard.cpp
 * @brief Bg-thread detector helpers for LifetimeToken
 *
 * The token type itself is header-only (templated defer methods), so this TU
 * only owns the bg-thread anti-pattern detector — main-thread id capture +
 * per-callsite first-fire reporting. See header for the rule the detector
 * targets and `project_l081_recurrence_post_840.md` for cluster context.
 */

#include "async_lifetime_guard.h"

#include "helix_lvgl_anomaly.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace helix::internal {

namespace {

/// Recorded main thread id. Populated by set_main_thread_id() before any
/// bg threads can spawn. on_main_thread() returns true conservatively
/// while this is unset so the detector doesn't false-positive during
/// the brief early-init window.
std::atomic<bool> g_main_thread_set{false};
pthread_t g_main_thread_id;

/// Strict mode: abort instead of just warning when the bg-thread anti-pattern
/// fires. Enabled by setting `HELIX_STRICT_BG_THREAD_CHECK=1` in the env
/// (CI runs export this) OR via `set_strict_bg_check(true)` (HelixTestFixture).
/// Resolved once at `set_main_thread_id()` time so the cost is one TLS-cache
/// read at fire.
///
/// Release builds (`HELIX_RELEASE_BUILD` defined by cross-compile targets in
/// `mk/cross.mk`) ignore the env var AND compile out the abort branch so a
/// stray env leak can never crash a user — the detector still emits the
/// telemetry anomaly + debug log. Native dev builds keep the abort so a real
/// L081 anti-pattern fails loudly with the LR on stderr (snapmaker-u1 user
/// 6d10417c hit this on 2026-05-14 with the env var somehow set; crash sig
/// 307b6f48). `set_strict_bg_check(true)` still flips the flag in any build,
/// but the release build has nothing to do with it.
std::atomic<bool> g_strict_bg_check{false};

/// Per-thread first-fire seen-set: TLS array of LRs already reported by
/// THIS thread. 64 slots × sizeof(void*) = 512 bytes per thread, linear
/// probe with cheap `>>4` hash. Each unique (thread, LR) pair fires at
/// most one anomaly per session. Multiple bg threads hitting the same LR
/// each get one report — interesting because each is a separate race vs.
/// the main thread. Table-full is silently dropped (no spam fallback).
constexpr size_t SEEN_SLOTS = 64;
thread_local void* tls_seen_lrs[SEEN_SLOTS] = {};

/// Returns true if this is the first time `lr` has been reported on the
/// calling thread; false otherwise (already seen or table full).
bool record_first_fire(void* lr) noexcept {
    if (lr == nullptr)
        return false;
    const auto h = static_cast<size_t>(reinterpret_cast<uintptr_t>(lr) >> 4) & (SEEN_SLOTS - 1);
    for (size_t step = 0; step < SEEN_SLOTS; ++step) {
        const size_t slot = (h + step) & (SEEN_SLOTS - 1);
        if (tls_seen_lrs[slot] == lr)
            return false;
        if (tls_seen_lrs[slot] == nullptr) {
            tls_seen_lrs[slot] = lr;
            return true;
        }
    }
    return false;
}

} // namespace

void set_main_thread_id() noexcept {
    if (g_main_thread_set.load(std::memory_order_acquire))
        return;
    g_main_thread_id = pthread_self();
#ifndef HELIX_RELEASE_BUILD
    // Resolve strict mode opt-in once. Test fixtures may call
    // set_strict_bg_check(true) explicitly; CI exports the env var.
    // Release builds skip both the env-var read and the abort branch
    // so a stray HELIX_STRICT_BG_THREAD_CHECK=1 can never crash a user.
    if (const char* v = std::getenv("HELIX_STRICT_BG_THREAD_CHECK");
        v != nullptr && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y')) {
        g_strict_bg_check.store(true, std::memory_order_release);
    }
#endif
    g_main_thread_set.store(true, std::memory_order_release);
}

void set_strict_bg_check(bool enabled) noexcept {
    g_strict_bg_check.store(enabled, std::memory_order_release);
}

bool on_main_thread() noexcept {
    if (!g_main_thread_set.load(std::memory_order_acquire)) {
        // Init not yet run — conservative return so the detector doesn't
        // false-positive on the genuine main thread before we recorded it.
        return true;
    }
    return pthread_equal(pthread_self(), g_main_thread_id) != 0;
}

[[gnu::noinline]] void report_bg_expired_check() noexcept {
    // expired() is [[gnu::always_inline]], so when our caller F has an
    // inlined `tok.expired()`, this trampoline's __builtin_return_address(0)
    // resolves to the LR *inside F* at the point of the inlined call — the
    // user's source-level callsite. Capturing the LR up in the inline path
    // would have given F's caller's LR after inlining (wrong frame).
    void* lr = __builtin_return_address(0);
    if (!record_first_fire(lr))
        return;

    // pthread_t is `unsigned long` on glibc/musl Linux but a pointer on
    // macOS — reinterpret_cast<uintptr_t> fails on 32-bit ARM where
    // sizeof(unsigned long) == sizeof(uintptr_t) but the integer ranks
    // differ. memcpy-into-uint64_t sidesteps both the cast restriction
    // and the platform-dependent representation.
    uint64_t tid_word = 0;
    pthread_t self = pthread_self();
    std::memcpy(&tid_word, &self,
                sizeof(self) < sizeof(tid_word) ? sizeof(self) : sizeof(tid_word));
    char ctx[96];
    std::snprintf(ctx, sizeof(ctx),
                  "lr=%p tid=0x%llx (likely L081 Mechanism C: tok.expired() on bg thread)", lr,
                  static_cast<unsigned long long>(tid_word));

    // Use the existing display-anomaly channel so the telemetry pipeline
    // captures backtrace + rate-limits across other LVGL anomalies. Telemetry
    // is the persistent signal — visible at any log level via debug bundles.
    helix_lvgl_anomaly("bg_tok_expired_check", ctx);

    // The runtime detector cannot distinguish a correct
    //   `if (tok.expired()) return; ... tok.defer([this](){ ... });`
    // pattern from a bare Mechanism C anti-pattern — both inline the same
    // `expired()` call and the captured LR points at the next instruction
    // in either case. scripts/check_l081_anti_pattern.py is the authoritative
    // static gate; this runtime emit is informational only. Field bundles
    // (#UMAX4U2G v0.99.60 et al.) showed 8 distinct LRs all resolving to
    // correct tok.defer() callsites — debug-level avoids that noise in
    // production logs while telemetry still captures occurrence rates.
    spdlog::debug("[LifetimeToken] bg-thread expired() check at lr={} "
                  "(cluster:pstat-async-delete — verify tok.defer() wraps body)",
                  lr);

#ifndef HELIX_RELEASE_BUILD
    // Strict mode (CI / test fixtures): abort so any new instance of the
    // anti-pattern fails the build. Print loudly to stderr so the abort
    // reason is visible in test output even at debug log level.
    // Compiled out in release builds — `g_strict_bg_check` may still be
    // true via `set_strict_bg_check(true)` but has no effect here.
    if (g_strict_bg_check.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
                     "\n[LifetimeToken] STRICT MODE: bg-thread expired() check at lr=%p — "
                     "wrap the callback body in tok.defer(...) or use lifetime_.bg_cb(...). "
                     "See include/async_lifetime_guard.h.\n",
                     lr);
        std::abort();
    }
#endif
}

} // namespace helix::internal

// ============================================================================
// helix::async_lifetime — skip-rate telemetry for AsyncLifetimeGuard
//
// See the declaration block at the bottom of include/async_lifetime_guard.h
// for the design rationale. Implementation notes:
//
//   * Slots are claimed by CAS on the `tag` pointer and released again by
//     `take_snapshot` whenever a slot drains to zero. Releasing is what keeps
//     the table a *per-window* view: without it the slots silt up with the
//     first MAX_TRACKED_TAGS tags the process ever sees, and every producer
//     that turns hot later is anonymised into "(other)" — losing exactly the
//     tag this telemetry exists to surface.
//   * All three skip paths fire on the LVGL main thread (they run inside
//     UpdateQueue::process_pending's lambda dispatch). `take_snapshot` is
//     also called from the main thread (TelemetryManager's periodic LVGL
//     timer), so the increment-and-snapshot pair is single-threaded in
//     practice. The atomics remain for documentation and for the rare case
//     where a bg_cb's underlying queue_update fires outside the main thread
//     (e.g. test fixtures driving process_pending manually).
//   * Tag pointers are string literals per UpdateQueue::TaggedCallback's
//     contract, so pointer-equality interning is sound and allocation-free.
// ============================================================================

namespace helix::async_lifetime {

namespace {

struct Counter {
    std::atomic<const char*> tag{nullptr};
    std::atomic<uint64_t> count{0};
};

// +1 slot for the synthetic "(other)" overflow bucket at index MAX_TRACKED_TAGS.
Counter g_counters[MAX_TRACKED_TAGS + 1];

constexpr const char* OTHER_TAG = "(other)";
constexpr const char* NULL_TAG = "(null)";

} // namespace

void note_skipped(const char* tag) noexcept {
    // nullptr is normalised once, up front, so the hot-path scan and the claim
    // path agree on the key and two nullptr skips can never land in two slots.
    const char* key = tag ? tag : NULL_TAG;

    // Hot path: a tag already holding a slot this window. Cache-friendly linear
    // scan over a 1 KB table. Acquire-load pairs with the CAS release below.
    for (size_t i = 0; i < MAX_TRACKED_TAGS; ++i) {
        if (g_counters[i].tag.load(std::memory_order_acquire) == key) {
            g_counters[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Cold path: first sighting this window. Claim a free slot by CAS. Slots
    // are freed on drain, so "free" is common — this is not a once-per-process
    // allocation. fetch_add (not store) publishes the count so a concurrent
    // hot-path increment on the same slot cannot be clobbered.
    for (size_t i = 0; i < MAX_TRACKED_TAGS; ++i) {
        const char* expected = nullptr;
        if (g_counters[i].tag.compare_exchange_strong(expected, key, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
            g_counters[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Lost the CAS to a thread claiming this same slot for this same tag.
        if (expected == key) {
            g_counters[i].count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Every slot is held by a different tag. A non-zero `other_count` in a
    // snapshot is itself a signal: more than MAX_TRACKED_TAGS distinct producers
    // skipped within this one window.
    g_counters[MAX_TRACKED_TAGS].count.fetch_add(1, std::memory_order_relaxed);
}

SkipSnapshot take_snapshot() noexcept {
    SkipSnapshot snap;

    // Drain the overflow bucket first so the per-tag loop below can early-exit
    // on empty slots without worrying about ordering vs. the overflow read.
    uint64_t other = g_counters[MAX_TRACKED_TAGS].count.exchange(0, std::memory_order_acq_rel);

    // Coalesce slots that may hold the same tag (two threads can each CAS a
    // different free slot for the same first-sighting tag — see note_skipped).
    for (size_t i = 0; i < MAX_TRACKED_TAGS; ++i) {
        const char* tag = g_counters[i].tag.load(std::memory_order_acquire);
        if (tag == nullptr)
            continue;
        uint64_t c = g_counters[i].count.exchange(0, std::memory_order_acq_rel);
        // Release the slot unconditionally, so the table holds exactly the tags
        // that skipped in the *current* window. Freeing only slots that were
        // already quiet would lag by a window, and a producer turning hot in
        // that gap would still be anonymised into "(other)" — the case this
        // whole mechanism exists to catch. A producer that is still hot simply
        // re-claims a slot on its next skip; skips are rare by construction, so
        // one cold-path scan per active tag per window costs nothing.
        //
        // Both sides are main-thread-only. The worst a racing producer could do
        // is land one increment in a slot mid-release, misattributing a single
        // count to whichever tag claims that slot next. Negligible for a rate.
        g_counters[i].tag.store(nullptr, std::memory_order_release);
        if (c == 0)
            continue;
        // Linear-find; the slot population is bounded so this stays
        // O(MAX_TRACKED_TAGS^2) worst case = 4096 compares, negligible.
        bool merged = false;
        for (auto& e : snap.entries) {
            if (e.tag == tag) {
                e.count += c;
                merged = true;
                break;
            }
        }
        if (!merged) {
            snap.entries.push_back({tag, c});
        }
        snap.total += c;
    }

    if (other > 0) {
        snap.entries.push_back({OTHER_TAG, other});
        snap.total += other;
        snap.other_count = other;
    }

    std::sort(snap.entries.begin(), snap.entries.end(),
              [](const SkipEntry& a, const SkipEntry& b) { return a.count > b.count; });
    return snap;
}

} // namespace helix::async_lifetime
