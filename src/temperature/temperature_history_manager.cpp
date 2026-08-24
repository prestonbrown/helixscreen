// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_history_manager.h"

#include "spdlog/spdlog.h"
#include "temperature_sensor_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace helix;

namespace {
/// Upper bound for a plausible temperature reading, in deci-degrees (400°C).
/// Anything at or below 0 is Klipper's "no data" / disconnect / inactive-tool
/// placeholder. Shared by the live recorder and the store seed so a sample can
/// never enter the history through one door that the other would have rejected.
constexpr int MAX_VALID_TEMP_DECI = 4000;
} // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

TemperatureHistoryManager::TemperatureHistoryManager(PrinterState& printer_state)
    : printer_state_(printer_state) {
    // Pre-populate heater map with standard heaters
    heaters_["extruder"] = HeaterHistory{};
    heaters_["heater_bed"] = HeaterHistory{};

    // Subscribe to temperature subjects for automatic sample collection
    subscribe_to_subjects();

    spdlog::debug("TemperatureHistoryManager: initialized with {} heaters", heaters_.size());
}

TemperatureHistoryManager::~TemperatureHistoryManager() {
    unsubscribe_from_subjects();
    spdlog::debug("TemperatureHistoryManager: destroyed");
}

// ============================================================================
// Data Access (thread-safe reads)
// ============================================================================

std::vector<TempSample>
TemperatureHistoryManager::get_samples(const std::string& heater_name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = heaters_.find(heater_name);
    if (it == heaters_.end()) {
        return {};
    }

    const HeaterHistory& history = it->second;
    std::vector<TempSample> result;
    result.reserve(static_cast<size_t>(history.count));

    if (history.count == 0) {
        return result;
    }

    // Calculate where the oldest sample is
    // If buffer is not full: oldest is at index 0
    // If buffer is full: oldest is at write_index (next to be overwritten)
    int oldest_index;
    int num_samples;

    if (history.count < HISTORY_SIZE) {
        // Buffer not full yet - samples start at 0
        oldest_index = 0;
        num_samples = history.count;
    } else {
        // Buffer full - oldest is at current write position
        oldest_index = history.write_index;
        num_samples = HISTORY_SIZE;
    }

    // Copy samples in chronological order (oldest first)
    for (int i = 0; i < num_samples; ++i) {
        int idx = (oldest_index + i) % HISTORY_SIZE;
        result.push_back(history.samples[static_cast<size_t>(idx)]);
    }

    return result;
}

std::vector<TempSample> TemperatureHistoryManager::get_samples_since(const std::string& heater_name,
                                                                     int64_t since_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = heaters_.find(heater_name);
    if (it == heaters_.end()) {
        return {};
    }

    const HeaterHistory& history = it->second;
    if (history.count == 0) {
        return {};
    }

    // Calculate where the oldest sample is
    int oldest_index;
    int num_samples;

    if (history.count < HISTORY_SIZE) {
        oldest_index = 0;
        num_samples = history.count;
    } else {
        oldest_index = history.write_index;
        num_samples = HISTORY_SIZE;
    }

    // Filter samples in chronological order, collecting only those after since_ms
    std::vector<TempSample> result;
    result.reserve(static_cast<size_t>(num_samples));

    for (int i = 0; i < num_samples; ++i) {
        int idx = (oldest_index + i) % HISTORY_SIZE;
        const TempSample& sample = history.samples[static_cast<size_t>(idx)];
        if (sample.timestamp_ms > since_ms) {
            result.push_back(sample);
        }
    }

    return result;
}

std::vector<std::string> TemperatureHistoryManager::get_heater_names() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> names;
    names.reserve(heaters_.size());

    for (const auto& [name, history] : heaters_) {
        names.push_back(name);
    }

    return names;
}

int TemperatureHistoryManager::get_sample_count(const std::string& heater_name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = heaters_.find(heater_name);
    if (it == heaters_.end()) {
        return 0;
    }

    return std::min(it->second.count, HISTORY_SIZE);
}

// ============================================================================
// Observer Pattern
// ============================================================================

void TemperatureHistoryManager::add_observer(HistoryCallback* cb) {
    if (cb == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already registered
    auto it = std::find(observers_.begin(), observers_.end(), cb);
    if (it == observers_.end()) {
        observers_.push_back(cb);
    }
}

void TemperatureHistoryManager::remove_observer(HistoryCallback* cb) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find(observers_.begin(), observers_.end(), cb);
    if (it != observers_.end()) {
        observers_.erase(it);
    }
}

void TemperatureHistoryManager::notify_observers(const std::string& heater_name) {
    // Copy observers under lock, then call outside lock to avoid deadlock
    std::vector<HistoryCallback*> observers_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observers_copy = observers_;
    }

    for (auto* cb : observers_copy) {
        if (cb != nullptr && *cb) {
            (*cb)(heater_name);
        }
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

bool TemperatureHistoryManager::add_sample_internal(const std::string& heater_name, int temp_deci,
                                                    int target_deci, int64_t timestamp_ms) {
    // Reject "no data" / disconnect / inactive-extruder readings BEFORE storing.
    // The temp subject momentarily reads 0 on disconnect, on partial Klipper
    // status updates, and for an idle extruder on a multi-tool printer (the U1).
    // Storing a 0 makes the history backfill/replay path draw a real data point
    // at the chart's 0°C floor — a solid vertical line dropping to the baseline
    // at the replayed live edge (the reported U1 artifact). The live-push filter
    // in TempGraphController only guards new pushes; the 0 enters here, via the
    // recorder, and is replayed later, so it must be rejected at this boundary —
    // the single source feeding every consumer (overlay, mini graph, panel).
    // Upper bound rejects obviously-bogus spikes (deci-degrees; 4000 = 400°C).
    if (temp_deci <= 0 || temp_deci > MAX_VALID_TEMP_DECI) {
        spdlog::debug("[TempHistory] dropping invalid sample for '{}': {} deci-°C", heater_name,
                      temp_deci);
        return false;
    }

    // Get or create heater history
    HeaterHistory& history = heaters_[heater_name];

    // Throttle: reject if within SAMPLE_INTERVAL_MS of last sample
    if (history.last_sample_ms > 0 &&
        (timestamp_ms - history.last_sample_ms) < SAMPLE_INTERVAL_MS) {
        return false;
    }

    // Store sample in circular buffer
    TempSample sample;
    sample.temp_deci = temp_deci;
    sample.target_deci = target_deci;
    sample.timestamp_ms = timestamp_ms;

    history.samples[static_cast<size_t>(history.write_index)] = sample;

    // Advance write index (circular)
    history.write_index = (history.write_index + 1) % HISTORY_SIZE;

    // Update count (capped at HISTORY_SIZE)
    if (history.count < HISTORY_SIZE) {
        history.count++;
    }

    // Update last sample time for throttling
    history.last_sample_ms = timestamp_ms;

    return true;
}

// ============================================================================
// Bulk Seeding
// ============================================================================

void TemperatureHistoryManager::seed_from_store(const TemperatureStore& store, int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    int keys_seeded = 0;

    for (const auto& [key, series] : store) {
        const size_t n = series.temperatures.size();
        if (n == 0) {
            continue;
        }

        // Moonraker's store is a bare array with no timestamps, so it is
        // reconstructed as an even 1 Hz run ending at now_ms.
        const int64_t store_oldest_ms = now_ms - static_cast<int64_t>(n - 1) * SAMPLE_INTERVAL_MS;

        HeaterHistory& hh = heaters_[key];

        // Merge on a window boundary, not a blanket replace. Two properties
        // have to hold simultaneously:
        //
        //  - No near-duplicate timestamps. Live samples recorded while the RPC
        //    was in flight sit at real wall-clock times a few hundred ms off
        //    the synthetic store grid; interleaving the two draws a phantom
        //    spike (#1245). So every local sample the store window covers is
        //    dropped — inside its own window the store is authoritative.
        //  - No loss of history the store does not have. Seeding re-runs on
        //    every discovery, and a restarted Klipper returns only a few
        //    seconds of store data; a blanket replace collapsed a 20-minute
        //    graph to seconds. Local samples strictly OLDER than the store
        //    window survive and are kept as the prefix.
        //
        // Both halves are chronological, so the merged result is strictly
        // increasing by construction.
        std::vector<TempSample> merged;
        merged.reserve(static_cast<size_t>(hh.count) + n);

        const int existing = std::min(hh.count, HISTORY_SIZE);
        const int oldest_index = (hh.count < HISTORY_SIZE) ? 0 : hh.write_index;
        for (int i = 0; i < existing; ++i) {
            const TempSample& s =
                hh.samples[static_cast<size_t>((oldest_index + i) % HISTORY_SIZE)];
            if (s.timestamp_ms < store_oldest_ms) {
                merged.push_back(s);
            }
        }
        const size_t local_kept = merged.size();

        for (size_t i = 0; i < n; ++i) {
            // Same sanity filter add_sample_internal() applies to live samples.
            // The store replays 0.0 for anything that was offline or not yet
            // reporting, and a seeded 0 draws a solid vertical drop to the
            // chart's 0°C floor exactly like a live one would.
            const int temp_deci = static_cast<int>(std::lround(series.temperatures[i] * 10.0f));
            if (temp_deci <= 0 || temp_deci > MAX_VALID_TEMP_DECI) {
                continue;
            }
            TempSample s;
            s.temp_deci = temp_deci;
            s.target_deci = (i < series.targets.size())
                                ? static_cast<int>(std::lround(series.targets[i] * 10.0f))
                                : 0;
            s.timestamp_ms = now_ms - static_cast<int64_t>(n - 1 - i) * SAMPLE_INTERVAL_MS;
            merged.push_back(s);
        }
        const size_t store_added = merged.size() - local_kept;

        if (merged.empty()) {
            // Every store value failed the sanity filter and there was nothing
            // local to keep. Leave the bucket exactly as it was.
            spdlog::debug("[TempHistory] seed '{}': all {} store samples rejected, history "
                          "left untouched",
                          key, n);
            continue;
        }

        // Keep the newest HISTORY_SIZE.
        const size_t keep = std::min(merged.size(), static_cast<size_t>(HISTORY_SIZE));
        const size_t start = merged.size() - keep;
        for (size_t i = 0; i < keep; ++i) {
            hh.samples[i] = merged[start + i];
        }
        hh.count = static_cast<int>(keep);
        hh.write_index = static_cast<int>(keep) % HISTORY_SIZE;
        hh.last_sample_ms = hh.samples[keep - 1].timestamp_ms;

        spdlog::debug("[TempHistory] seeded '{}': {} samples ({} local kept + {} from store, "
                      "{} trimmed), span_min={:.1f} oldest_ts={} newest_ts={} "
                      "first_temp={:.1f}C last_temp={:.1f}C",
                      key, hh.count, local_kept, store_added, merged.size() - keep,
                      (hh.last_sample_ms - hh.samples[0].timestamp_ms) / 60000.0f,
                      hh.samples[0].timestamp_ms, hh.last_sample_ms,
                      hh.samples[0].temp_deci / 10.0f, hh.samples[keep - 1].temp_deci / 10.0f);
        ++keys_seeded;
    }

    spdlog::debug("[TempHistory] seed_from_store: {} of {} store keys seeded", keys_seeded,
                  store.size());
}

// ============================================================================
// Subject Subscription
// ============================================================================

namespace {

/**
 * @brief Get current Unix timestamp in milliseconds
 */
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

void TemperatureHistoryManager::temp_observer_callback(lv_observer_t* observer,
                                                       lv_subject_t* subject) {
    auto* ctx = static_cast<ObserverContext*>(lv_observer_get_user_data(observer));
    if (ctx == nullptr || ctx->manager == nullptr) {
        return;
    }

    // Skip the initial callback fired during subscription (value is just initial 0)
    if (!ctx->first_callback_skipped) {
        ctx->first_callback_skipped = true;
        return;
    }

    int temp_deci = lv_subject_get_int(subject);
    // Read target from the manager's cached value
    int target_deci = ctx->manager->get_cached_target(ctx->heater_name);

    bool stored;
    {
        std::lock_guard<std::mutex> lock(ctx->manager->mutex_);
        stored =
            ctx->manager->add_sample_internal(ctx->heater_name, temp_deci, target_deci, now_ms());
    }
    if (stored) {
        ctx->manager->notify_observers(ctx->heater_name);
    }
}

void TemperatureHistoryManager::target_observer_callback(lv_observer_t* observer,
                                                         lv_subject_t* subject) {
    auto* ctx = static_cast<ObserverContext*>(lv_observer_get_user_data(observer));
    if (ctx == nullptr || ctx->manager == nullptr) {
        return;
    }

    int target_deci = lv_subject_get_int(subject);

    ctx->manager->set_cached_target(ctx->heater_name, target_deci);

    // Update the most recent sample if it was stored very recently
    ctx->manager->update_recent_sample_target(ctx->heater_name, target_deci);
}

void TemperatureHistoryManager::subscribe_to_subjects() {
    // Discovery republishes the extruder and sensor lists — and recreates their
    // subjects — after the WebSocket connects, long after this manager is built
    // at startup. Watch both version subjects so the recorders reattach instead
    // of silently sampling nothing for the rest of the session.
    if (lv_subject_t* extruder_version = printer_state_.get_extruder_version_subject()) {
        extruder_version_observer_ = ObserverGuard(
            extruder_version,
            [](lv_observer_t* observer, lv_subject_t*) {
                auto* self =
                    static_cast<TemperatureHistoryManager*>(lv_observer_get_user_data(observer));
                if (self != nullptr) {
                    self->resubscribe();
                }
            },
            this);
    }

    auto& sensor_mgr = helix::sensors::TemperatureSensorManager::instance();
    if (lv_subject_t* sensor_count = sensor_mgr.get_sensor_count_subject()) {
        sensor_count_observer_ = ObserverGuard(
            sensor_count,
            [](lv_observer_t* observer, lv_subject_t*) {
                auto* self =
                    static_cast<TemperatureHistoryManager*>(lv_observer_get_user_data(observer));
                if (self != nullptr) {
                    self->resubscribe();
                }
            },
            this);
    }

    resubscribe();
}

void TemperatureHistoryManager::subscribe_one(const std::string& key, lv_subject_t* temp_subject,
                                              const SubjectLifetime& temp_lifetime,
                                              lv_subject_t* target_subject,
                                              const SubjectLifetime& target_lifetime) {
    if (temp_subject == nullptr) {
        return; // Not discovered yet — resubscribe() runs again when it is
    }

    // Dedupe: a sensor-based chamber reaches us twice, once as the chamber
    // subject and once through the sensor list. Two observers on one bucket
    // would double-sample and fight the write throttle.
    for (const auto& existing : subscriptions_) {
        if (existing->temp_ctx && existing->temp_ctx->heater_name == key) {
            return;
        }
    }

    auto sub = std::make_unique<Subscription>();

    sub->temp_ctx = std::make_unique<ObserverContext>();
    sub->temp_ctx->manager = this;
    sub->temp_ctx->heater_name = key;
    sub->temp_lifetime = temp_lifetime;
    sub->temp_observer = ObserverGuard(temp_subject, temp_observer_callback, sub->temp_ctx.get());
    // Only for genuinely dynamic subjects. Handing over an EMPTY token reads as
    // "subject already dead", which makes reset() skip lv_observer_remove() and
    // strand a live observer on a freed context (#816's guard, inverted).
    if (temp_lifetime) {
        sub->temp_observer.set_alive_token(temp_lifetime);
    }

    if (target_subject != nullptr) {
        sub->target_ctx = std::make_unique<ObserverContext>();
        sub->target_ctx->manager = this;
        sub->target_ctx->heater_name = key;
        sub->target_lifetime = target_lifetime;
        sub->target_observer =
            ObserverGuard(target_subject, target_observer_callback, sub->target_ctx.get());
        if (target_lifetime) {
            sub->target_observer.set_alive_token(target_lifetime);
        }
    }

    // Materialize the bucket so get_heater_names() reports what we record,
    // even before the first sample lands.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        heaters_[key];
    }

    subscriptions_.push_back(std::move(sub));
}

void TemperatureHistoryManager::resubscribe() {
    // Drop every recorder first. Guards carry lifetime tokens, so ones whose
    // subject was already freed by rediscovery neuter instead of touching it.
    subscriptions_.clear();

    const auto& temp_state = printer_state_.temperature_state();

    // One bucket per discovered tool, keyed by its real Klipper name. This is
    // the key TempGraphController::backfill_history() asks for, so an idle tool
    // gets its own trace instead of inheriting the active tool's.
    const auto& extruders = temp_state.extruders();
    for (const auto& [name, info] : extruders) {
        SubjectLifetime temp_lt;
        SubjectLifetime target_lt;
        lv_subject_t* temp = printer_state_.get_extruder_temp_subject(name, temp_lt);
        lv_subject_t* target = printer_state_.get_extruder_target_subject(name, target_lt);
        subscribe_one(name, temp, temp_lt, target, target_lt);
    }

    // Pre-discovery fallback: no per-extruder subjects exist yet, so the active
    // subject is the only nozzle source. It goes away the moment discovery
    // lands, because keeping it would refile the ACTIVE tool's readings under
    // "extruder" — the bug this whole path exists to avoid.
    if (extruders.empty()) {
        subscribe_one("extruder", printer_state_.get_active_extruder_temp_subject(), {},
                      printer_state_.get_active_extruder_target_subject(), {});
    }

    SubjectLifetime bed_temp_lt;
    SubjectLifetime bed_target_lt;
    lv_subject_t* bed_temp = printer_state_.get_bed_temp_subject(bed_temp_lt);
    lv_subject_t* bed_target = printer_state_.get_bed_target_subject(bed_target_lt);
    subscribe_one("heater_bed", bed_temp, bed_temp_lt, bed_target, bed_target_lt);

    // A heater_generic/temperature_fan chamber is not a TemperatureSensorManager
    // sensor, so it needs its own recorder. Sensor-based chambers fall through
    // to the sensor loop below and dedupe on the same key.
    if (!temp_state.chamber_heater_name().empty()) {
        SubjectLifetime chamber_temp_lt;
        SubjectLifetime chamber_target_lt;
        lv_subject_t* chamber_temp = printer_state_.get_chamber_temp_subject(chamber_temp_lt);
        lv_subject_t* chamber_target = printer_state_.get_chamber_target_subject(chamber_target_lt);
        subscribe_one(temp_state.chamber_heater_name(), chamber_temp, chamber_temp_lt,
                      chamber_target, chamber_target_lt);
    }

    // Every auxiliary sensor: chamber thermistors, beacon coil, the per-tool
    // T0_temp..TN_temp probes on a changer. No targets on these.
    auto& sensor_mgr = helix::sensors::TemperatureSensorManager::instance();
    for (const auto& sensor : sensor_mgr.get_sensors()) {
        SubjectLifetime sensor_lt;
        lv_subject_t* temp = sensor_mgr.get_temp_subject(sensor.klipper_name, sensor_lt);
        subscribe_one(sensor.klipper_name, temp, sensor_lt, nullptr, {});
    }

    spdlog::debug("[TempHistory] recording {} sensors ({} extruders discovered)",
                  subscriptions_.size(), extruders.size());
}

void TemperatureHistoryManager::unsubscribe_from_subjects() {
    // ObserverGuard::reset() handles nullptr checks and lv_is_initialized() safety
    extruder_version_observer_.reset();
    sensor_count_observer_.reset();
    subscriptions_.clear();
}

// ============================================================================
// Cached Target Methods
// ============================================================================

int TemperatureHistoryManager::get_cached_target(const std::string& heater_name) const {
    auto it = cached_targets_.find(heater_name);
    return it != cached_targets_.end() ? it->second : 0;
}

void TemperatureHistoryManager::set_cached_target(const std::string& heater_name, int target_deci) {
    cached_targets_[heater_name] = target_deci;
}

void TemperatureHistoryManager::update_recent_sample_target(const std::string& heater_name,
                                                            int target_deci) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = heaters_.find(heater_name);
    if (it == heaters_.end() || it->second.count == 0) {
        return;
    }

    HeaterHistory& history = it->second;

    // Find the most recent sample
    int recent_idx = (history.write_index - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    TempSample& recent = history.samples[static_cast<size_t>(recent_idx)];

    // Check if it was stored recently (within RECENT_SAMPLE_WINDOW_MS)
    using namespace std::chrono;
    int64_t current_ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    int64_t age_ms = current_ms - recent.timestamp_ms;

    // Always update if sample was stored very recently
    // Use a generous window since temp and target are typically set together
    if (age_ms <= RECENT_SAMPLE_WINDOW_MS) {
        recent.target_deci = target_deci;
    }
}
