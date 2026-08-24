// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "moonraker_types.h"
#include "printer_state.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Single temperature sample with timestamp
 *
 * Uses decidegrees (x10) for precision without floating point.
 * Example: 2053 = 205.3°C
 */
struct TempSample {
    int temp_deci = 0;        ///< Temperature × 10 (e.g., 2053 = 205.3°C)
    int target_deci = 0;      ///< Target temperature × 10
    int64_t timestamp_ms = 0; ///< Unix timestamp in milliseconds
};

/**
 * @brief Heater type classification
 */
enum class TempHistoryHeaterType { EXTRUDER, BED, CHAMBER };

/**
 * @brief Manages temperature history collection for all heaters
 *
 * Collects temperature samples from helix::PrinterState subjects at app startup,
 * stores 20 minutes of history (1200 samples @ 1Hz) per heater,
 * and provides observer notifications when new samples arrive.
 *
 * ## Thread Safety
 * - Data reads (get_samples, get_sample_count) are protected by mutex
 * - Writes are expected from the main thread via subject observers
 *
 * ## Usage Example
 *
 * ```cpp
 * // Create manager with helix::PrinterState reference
 * TemperatureHistoryManager manager(printer_state);
 *
 * // Register observer for updates
 * TemperatureHistoryManager::HistoryCallback cb = [](const std::string& heater) {
 *     spdlog::info("New sample for {}", heater);
 * };
 * manager.add_observer(&cb);
 *
 * // Query history
 * auto samples = manager.get_samples("extruder");
 * auto recent = manager.get_samples_since("heater_bed", now_ms - 60000); // last minute
 * ```
 */
class TemperatureHistoryManager {
  public:
    static constexpr int HISTORY_SIZE = 1200;           ///< 20 minutes at 1Hz
    static constexpr int64_t SAMPLE_INTERVAL_MS = 1000; ///< 1 second minimum between samples
    static constexpr int64_t RECENT_SAMPLE_WINDOW_MS =
        100; ///< Window for retroactive target updates

    /**
     * @brief Construct TemperatureHistoryManager with helix::PrinterState reference
     *
     * Pre-populates heater map with "extruder" and "heater_bed".
     * Subscribes to temperature subjects for automatic sample collection.
     *
     * @param printer_state Reference to helix::PrinterState for subject subscription
     */
    explicit TemperatureHistoryManager(helix::PrinterState& printer_state);

    /**
     * @brief Destructor - unsubscribes from subjects
     */
    ~TemperatureHistoryManager();

    // Non-copyable
    TemperatureHistoryManager(const TemperatureHistoryManager&) = delete;
    TemperatureHistoryManager& operator=(const TemperatureHistoryManager&) = delete;

    // ========================================================================
    // Data Access (thread-safe reads)
    // ========================================================================

    /**
     * @brief Get all samples for a heater
     *
     * Returns samples in chronological order (oldest first).
     *
     * @param heater_name Heater name (e.g., "extruder", "heater_bed")
     * @return Vector of samples, oldest first. Empty if heater unknown.
     */
    [[nodiscard]] std::vector<TempSample> get_samples(const std::string& heater_name) const;

    /**
     * @brief Get samples since a given timestamp
     *
     * Returns only samples with timestamp_ms > since_ms.
     *
     * @param heater_name Heater name
     * @param since_ms Unix timestamp in ms - return samples newer than this
     * @return Vector of samples since timestamp, oldest first
     */
    [[nodiscard]] std::vector<TempSample> get_samples_since(const std::string& heater_name,
                                                            int64_t since_ms) const;

    /**
     * @brief Get list of known heater names
     *
     * Returns at minimum "extruder" and "heater_bed".
     *
     * @return Vector of heater names
     */
    [[nodiscard]] std::vector<std::string> get_heater_names() const;

    /**
     * @brief Get number of samples stored for a heater
     *
     * @param heater_name Heater name
     * @return Sample count (0 to HISTORY_SIZE), 0 if heater unknown
     */
    [[nodiscard]] int get_sample_count(const std::string& heater_name) const;

    // ========================================================================
    // Observer Pattern
    // ========================================================================

    /**
     * @brief Callback type for history change notifications
     *
     * Called when new samples are stored. Parameter is the heater name.
     */
    using HistoryCallback = std::function<void(const std::string& heater_name)>;

    /**
     * @brief Register observer for history changes
     *
     * Callback is invoked when a sample is stored (not throttled).
     * Uses pointer-based registration for reliable removal.
     *
     * @param cb Pointer to callback function (caller owns memory)
     */
    void add_observer(HistoryCallback* cb);

    /**
     * @brief Unregister observer
     *
     * @param cb Pointer to callback previously registered
     */
    void remove_observer(HistoryCallback* cb);

    /**
     * @brief Get cached target temperature for a heater
     * @param heater_name Heater name
     * @return Target in decidegrees
     */
    [[nodiscard]] int get_cached_target(const std::string& heater_name) const;

    /**
     * @brief Set cached target temperature for a heater
     * @param heater_name Heater name
     * @param target_deci Target in decidegrees
     */
    void set_cached_target(const std::string& heater_name, int target_deci);

    /**
     * @brief Update target in the most recent sample if stored recently
     *
     * Used when target is set after temp in the same update cycle.
     * Updates the most recent sample's target if it was stored within
     * RECENT_SAMPLE_WINDOW_MS milliseconds.
     *
     * @param heater_name Heater name
     * @param target_deci New target value
     */
    void update_recent_sample_target(const std::string& heater_name, int target_deci);

    // ========================================================================
    // Bulk Seeding
    // ========================================================================

    /**
     * @brief Seed history from Moonraker's cached temperature_store
     *
     * Bulk-loads ~20 minutes of 1 Hz history fetched via
     * server.temperature_store so graphs are populated immediately on connect
     * instead of filling in live over several minutes.
     *
     * For each sensor, timestamps are synthesized backward from @p now_ms at
     * SAMPLE_INTERVAL_MS spacing so the newest seeded sample lands at @p now_ms.
     * Temperatures/targets are converted to decidegrees (×10). Powers are
     * ignored (TempSample has no power field).
     *
     * The store is authoritative inside its own window: local samples older than
     * the oldest store sample are kept as a prefix, and anything the window
     * covers is superseded. That keeps history a restarted Klipper no longer has
     * while still discarding the near-duplicate pairs a plain merge produced
     * (a live wall-clock sample next to the synthetic 1 Hz grid). Store values
     * pass the same sanity filter as live samples, and the newest HISTORY_SIZE
     * are kept. This bypasses the SAMPLE_INTERVAL_MS write throttle and is
     * race-safe against a live sample that may already have been appended before
     * the async fetch returned.
     *
     * @param store Per-sensor history keyed by Klipper object name
     * @param now_ms Wall-clock timestamp (Unix ms) for the newest seeded sample.
     *               MUST be on the same clock feeding live samples so seeded and
     *               live samples are comparable.
     */
    void seed_from_store(const TemperatureStore& store, int64_t now_ms);

  private:
    friend class TemperatureHistoryManagerTestAccess;

    /**
     * @brief Per-heater circular buffer for temperature samples
     */
    struct HeaterHistory {
        std::array<TempSample, HISTORY_SIZE> samples{}; ///< Circular buffer
        int write_index = 0;        ///< Next write position (0 to HISTORY_SIZE-1)
        int count = 0;              ///< Number of samples stored (0 to HISTORY_SIZE)
        int64_t last_sample_ms = 0; ///< Timestamp of last stored sample (for throttling)
    };

    /**
     * @brief Add a sample to heater history (internal, must hold mutex)
     *
     * @param heater_name Heater name
     * @param temp_deci Temperature in decidegrees
     * @param target_deci Target in decidegrees
     * @param timestamp_ms Timestamp in milliseconds
     * @return true if sample was stored, false if throttled
     */
    bool add_sample_internal(const std::string& heater_name, int temp_deci, int target_deci,
                             int64_t timestamp_ms);

    /**
     * @brief Notify all registered observers
     *
     * @param heater_name Heater that received new sample
     */
    void notify_observers(const std::string& heater_name);

    /**
     * @brief Subscribe to helix::PrinterState temperature subjects
     */
    void subscribe_to_subjects();

    /**
     * @brief Unsubscribe from helix::PrinterState temperature subjects
     */
    void unsubscribe_from_subjects();

    /**
     * @brief Rebuild every per-sensor recorder from what is currently discovered
     *
     * Drops all existing sample subscriptions and re-derives one per Klipper
     * object we can sample: every discovered extruder, the bed, a chamber
     * heater, and every TemperatureSensorManager sensor. Re-run whenever
     * discovery republishes the extruder or sensor list — the subjects are
     * recreated on rediscovery, so the old observers point at freed memory
     * (their lifetime tokens neuter them; this reattaches to the new ones).
     *
     * Main thread only: touches LVGL subjects and observers.
     */
    void resubscribe();

    /**
     * @brief Attach temp/target observers for a single Klipper object
     *
     * No-op when @p temp_subject is null (object not discovered yet) or when
     * @p key already has a recorder — dedupe matters because a sensor-based
     * chamber appears both as the chamber subject and in the sensor list.
     *
     * @param key           History bucket name; MUST be the Klipper object name
     *                      that TempGraphController's backfill will ask for
     * @param temp_subject  Temperature subject (decidegrees), may be null
     * @param temp_lifetime Lifetime token for @p temp_subject
     * @param target_subject Target subject, or null for sensors with no target
     * @param target_lifetime Lifetime token for @p target_subject
     */
    void subscribe_one(const std::string& key, lv_subject_t* temp_subject,
                       const SubjectLifetime& temp_lifetime, lv_subject_t* target_subject,
                       const SubjectLifetime& target_lifetime);

    /**
     * @brief Static callback for temperature observer notifications
     *
     * Called by LVGL when temperature subjects change. Implemented as static
     * member to access private ObserverContext struct.
     */
    static void temp_observer_callback(lv_observer_t* observer, lv_subject_t* subject);

    /**
     * @brief Static callback for target temperature observer notifications
     *
     * Called by LVGL when target subjects change. Updates cached target
     * and retroactively patches recent samples.
     */
    static void target_observer_callback(lv_observer_t* observer, lv_subject_t* subject);

    // Dependencies
    helix::PrinterState& printer_state_;

    // Per-heater circular buffers
    std::unordered_map<std::string, HeaterHistory> heaters_;

    // Cached targets per Klipper object, updated by the target observers.
    // Keyed the same way as heaters_ so every tool on a changer carries its own
    // setpoint — a single shared extruder cache would stamp the active tool's
    // target onto every other tool's samples.
    // Thread-safety note: only accessed from the main thread via LVGL observer
    // callbacks. No mutex protection needed as LVGL runs single-threaded.
    std::unordered_map<std::string, int> cached_targets_;

    // Thread safety
    mutable std::mutex mutex_;

    // Observers (stored as pointers for reliable removal)
    std::vector<HistoryCallback*> observers_;

    /**
     * @brief Context for tracking initial observer callback skip
     *
     * Implementation detail used by static observer callbacks in the .cpp file.
     * Contains the manager pointer, heater name, and a flag to skip the initial
     * callback that fires during observer registration.
     */
    struct ObserverContext {
        TemperatureHistoryManager* manager = nullptr;
        bool first_callback_skipped = false;
        std::string heater_name; ///< Which heater this context is for
    };

    /**
     * @brief One sample recorder: the contexts and guards for a single key
     *
     * Member order is load-bearing: the guards are declared last so they
     * destruct FIRST, coming off the subjects before the contexts they
     * reference are freed.
     */
    struct Subscription {
        std::unique_ptr<ObserverContext> temp_ctx;
        std::unique_ptr<ObserverContext> target_ctx;
        SubjectLifetime temp_lifetime;
        SubjectLifetime target_lifetime;
        ObserverGuard temp_observer;
        ObserverGuard target_observer;
    };

    // One entry per Klipper object being recorded, rebuilt by resubscribe().
    // Contexts are heap-stable (unique_ptr) because the observers hold raw
    // pointers into them.
    std::vector<std::unique_ptr<Subscription>> subscriptions_;

    // Discovery watchers — bumped when the extruder or sensor list changes,
    // which is when the per-object subjects are recreated.
    ObserverGuard extruder_version_observer_;
    ObserverGuard sensor_count_observer_;
};
