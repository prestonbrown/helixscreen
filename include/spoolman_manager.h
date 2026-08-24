// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "filament_display_name.h"
#include "lvgl/lvgl.h"
#include "spoolman_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class IMoonrakerAPI;

/**
 * @brief Centralized Spoolman weight polling, circuit breaker, and identity cache
 *
 * Extracted from AmsState to decouple Spoolman from AMS hardware.
 * Spoolman works independently — printers without AMS can still use
 * Spoolman for filament tracking.
 *
 * Owns:
 * - Periodic weight polling via lv_timer (refcounted start/stop)
 * - Circuit breaker to suppress error toasts when Spoolman is unavailable
 * - Print state observer to auto-refresh weights on print start/end/pause
 * - Spoolman availability observer to auto-stop polling when Spoolman disappears
 * - The transient Spoolman **identity** cache (see below)
 *
 * @note The charter used to be "polling + circuit breaker, holds no spool data".
 *       It is deliberately wider now: this class also owns a per-`spool_id`
 *       cache of `helix::SpoolIdentity` (vendor / filament name / material).
 *       That data lives here rather than on `SlotInfo` on purpose — the weight
 *       poll writes slots with `persist=false` to break a G-code feedback loop
 *       (see the comment in `refresh_spoolman_weights()`), and the
 *       firmware-vs-override merge in `docs/devel/FILAMENT_SLOT_METADATA.md` §5
 *       is a clean two-way that a third writer would make ambiguous. Keeping
 *       identity in a side channel is the quarantine.
 *
 * ### Identity cache properties
 *
 * - **Transient.** Never persisted; a cold boot renders the firmware fallback
 *   until the first poll lands. Deliberate.
 * - **Populated from the existing weight poll.** `refresh_spoolman_weights()`
 *   already fetches the whole `SpoolInfo` and throws the identity away. No new
 *   round trip is made for it.
 * - **Two cadences.** Identity is immutable in practice and is extracted once
 *   per id; weight keeps the 30s poll. A cached id skips the *extraction*, not
 *   the fetch.
 * - **Negative caching.** An id Spoolman reports as missing (deleted spool)
 *   enters an unresolvable set and is not polled again, so a dead link cannot
 *   turn into a per-cycle request storm.
 *
 * @threading Every cache entry point is `static`, checks `s_shutdown_flag`
 *            before touching the singleton, and takes `mutex_`. None of them
 *            calls into LVGL, so they are safe from the HTTP thread as well as
 *            the UI thread. Reads return **by value** — see `find_identity()`.
 */
class SpoolmanManager {
  public:
    static SpoolmanManager& instance();

    SpoolmanManager(const SpoolmanManager&) = delete;
    SpoolmanManager& operator=(const SpoolmanManager&) = delete;

    void init_subjects();
    void deinit_subjects();

    void set_api(IMoonrakerAPI* api);

    void refresh_spoolman_weights();
    void start_spoolman_polling();
    void stop_spoolman_polling();

  private:
    /**
     * @brief Create the poll timer if something wants polling and Spoolman can serve it
     *
     * The wish to poll and the ability to serve it arrive in either order, and
     * at boot it is always wish-first: panels activate synchronously inside
     * `init_ui()`, while `set_spoolman_available()` defers through the
     * UpdateQueue and has not drained yet. So `start_spoolman_polling()` records
     * the wish unconditionally and this decides when it can be acted on, called
     * again from the availability observer when Spoolman appears.
     */
    void ensure_poll_timer();

  public:
    // ========================================================================
    // Spoolman identity side channel
    // ========================================================================

    /**
     * @brief Look up the cached identity for a spool id
     *
     * Returns **by value**, not by pointer. The cache is an
     * `unordered_map` that a later poll can rehash, and it is reachable from
     * the HTTP thread; a borrowed `const SpoolIdentity*` would be a
     * use-after-free waiting for the next `emplace()`. The copy is three short
     * strings on a path that is already building a label string.
     *
     * @param spool_id Spoolman spool id; <= 0 is always a miss
     * @return The identity, or `std::nullopt` on a miss / unresolvable id
     */
    static std::optional<helix::SpoolIdentity> find_identity(int spool_id);

    /**
     * @brief Record the identity carried by a freshly fetched spool record
     *
     * Insert-if-absent: a spool id already in the cache is left alone, which is
     * what keeps identity extraction to once per id while weight keeps polling.
     * Clears any unresolvable mark for the id. A record with nothing a label can
     * use (`SpoolIdentity::valid() == false`) is not stored.
     *
     * @return true when a new identity was stored, i.e. when some label that
     *         previously resolved without it can now resolve better. Callers
     *         use this to refresh label consumers **once**, rather than on
     *         every poll -- see the weights-unchanged early return in
     *         refresh_spoolman_weights(), which this deliberately runs before.
     */
    static bool cache_identity(const SpoolInfo& spool);

    /// Mark a spool id as unresolvable (Spoolman answered "no such spool").
    static void note_identity_unresolvable(int spool_id);

    /// True when @p spool_id is known unresolvable and must not be polled.
    static bool is_identity_unresolvable(int spool_id);

    /// Drop one id from both the cache and the unresolvable set, so the next
    /// poll re-reads it. Called when the user edits the spool.
    static void invalidate_identity(int spool_id);

    /// Drop everything. Called when Spoolman goes away.
    static void clear_identity_cache();

  private:
    friend class SpoolmanManagerTestAccess;
    friend class SpoolmanIdentityTestAccess;

    SpoolmanManager() = default;
    ~SpoolmanManager();

    static std::atomic<bool> s_shutdown_flag;

    mutable std::recursive_mutex mutex_;
    IMoonrakerAPI* api_ = nullptr;
    bool initialized_ = false;

    // Polling
    lv_timer_t* poll_timer_ = nullptr;
    int poll_refcount_ = 0;

    // Circuit breaker / debounce
    static constexpr int CB_FAILURE_THRESHOLD = 3;
    static constexpr uint32_t CB_BACKOFF_MS = 30000;
    static constexpr uint32_t DEBOUNCE_MS = 5000;
    static constexpr uint32_t POLL_INTERVAL_MS = 30000;

    uint32_t last_refresh_ms_ = 0;
    int consecutive_failures_ = 0;
    uint32_t cb_tripped_at_ms_ = 0;
    bool cb_open_ = false;
    bool unavailable_notified_ = false;

    void reset_circuit_breaker();

    // Identity side channel — guarded by mutex_, never touches LVGL
    std::unordered_map<int, helix::SpoolIdentity> identity_cache_;
    std::unordered_set<int> identity_unresolvable_;

    // Observers
    ObserverGuard print_state_observer_;
    ObserverGuard spoolman_availability_observer_;
};
