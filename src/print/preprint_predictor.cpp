// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_predictor.h"

#include "config.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

namespace helix {

void PreprintPredictor::load_entries(const std::vector<PreprintEntry>& entries,
                                     StartCondition condition) {
    entries_.clear();

    for (const auto& e : entries) {
        // temp_bucket 0 = legacy/unknown, always include
        if (e.temp_bucket == 0) {
            entries_.push_back(e);
        } else if (condition == StartCondition::COLD && e.temp_bucket == 1) {
            entries_.push_back(e);
        } else if (condition == StartCondition::WARM && e.temp_bucket == 2) {
            entries_.push_back(e);
        }
    }

    // FIFO trim to MAX_ENTRIES (keep newest)
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

bool PreprintPredictor::entry_matches_window(const PreprintEntry& e, PreprintWindow filter) {
    if (filter == PreprintWindow::Unknown) {
        return true; // no window filter requested
    }
    if (e.window == filter) {
        return true;
    }
    // A legacy entry carries no window because commit arming did not exist when
    // it was recorded, which means it can only have measured a printer-edge
    // window. Treating it as such is a fact about the data, not a guess - and
    // the asymmetry is the point: legacy history stays usable for externally
    // started prints, but must never stand in for a window that included a
    // host-side pre-start block it never saw.
    return e.window == PreprintWindow::Unknown && filter == PreprintWindow::PrinterEdge;
}

void PreprintPredictor::load_entries(const std::vector<PreprintEntry>& entries, int temp_bucket,
                                     PreprintWindow window) {
    // Window filtering runs first so the temp-bucket paths below - including the
    // ones that delegate and return - all see the same already-narrowed set.
    std::vector<PreprintEntry> windowed;
    windowed.reserve(entries.size());
    for (const auto& e : entries) {
        if (entry_matches_window(e, window)) {
            windowed.push_back(e);
        }
    }
    const std::vector<PreprintEntry>& in_window = windowed;

    entries_.clear();

    if (temp_bucket == 0) {
        // No temp filter — load everything left in the window
        entries_ = in_window;
    } else if (temp_bucket == 1) {
        // Cold start bucket
        load_entries(in_window, StartCondition::COLD);
        return;
    } else if (temp_bucket == 2) {
        // Warm start bucket
        load_entries(in_window, StartCondition::WARM);
        return;
    } else {
        // Legacy temp_bucket values (e.g. 200, 250): filter by matching bucket or legacy 0
        for (const auto& e : in_window) {
            if (e.temp_bucket == temp_bucket || e.temp_bucket == 0) {
                entries_.push_back(e);
            }
        }
    }

    // FIFO trim to MAX_ENTRIES (keep newest)
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

void PreprintPredictor::add_entry(const PreprintEntry& entry) {
    entries_.push_back(entry);

    // FIFO trim
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}

std::vector<PreprintEntry> PreprintPredictor::get_entries() const {
    return entries_;
}

bool PreprintPredictor::has_predictions() const {
    return !entries_.empty();
}

std::vector<double> PreprintPredictor::compute_weights() const {
    if (entries_.empty())
        return {};

    std::vector<double> weights(entries_.size());
    // ln(10)/10 ≈ 0.23 — oldest of 10 entries gets ~10% relative weight
    constexpr double lambda = 0.23;
    double total = 0.0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        weights[i] = std::exp(lambda * static_cast<double>(i));
        total += weights[i];
    }
    for (auto& w : weights) {
        w /= total;
    }
    return weights;
}

std::map<int, int> PreprintPredictor::default_phase_durations() {
    // Per-printer DB override wins over the generic defaults so first-print
    // ETAs aren't wildly off on printers whose PRINT_START / slicer start
    // gcode doesn't exercise the full QGL/mesh/wipe flow (e.g. Elegoo CC1).
    if (auto* cfg = Config::get_instance()) {
        std::string printer_type =
            cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");
        if (!printer_type.empty()) {
            auto db_phases = PrinterDetector::get_print_start_default_phases(printer_type);
            if (!db_phases.empty()) {
                return db_phases;
            }
        }
    }

    // Generic ceiling used when the DB has no override — tuned so a printer
    // that actually runs all six phases still gets a ballpark first-print ETA.
    return {
        {static_cast<int>(PrintStartPhase::HOMING), 30},
        {static_cast<int>(PrintStartPhase::BED_MESH), 90},
        {static_cast<int>(PrintStartPhase::QGL), 60},
        {static_cast<int>(PrintStartPhase::Z_TILT), 45},
        {static_cast<int>(PrintStartPhase::CLEANING), 20},
        {static_cast<int>(PrintStartPhase::PURGING), 15},
    };
}

std::map<int, int> PreprintPredictor::predicted_phases() const {
    if (entries_.empty()) {
        return default_phase_durations();
    }

    // Collect all phases that appear in any entry
    std::set<int> all_phases;
    for (const auto& entry : entries_) {
        for (const auto& [phase, _] : entry.phase_durations) {
            all_phases.insert(phase);
        }
    }

    auto weights = compute_weights();

    std::map<int, int> result;
    for (int phase : all_phases) {
        // Step 1: Collect durations for entries that have this phase
        std::vector<std::pair<size_t, double>> phase_entries; // (index, duration)
        for (size_t i = 0; i < entries_.size(); ++i) {
            auto it = entries_[i].phase_durations.find(phase);
            if (it != entries_[i].phase_durations.end()) {
                phase_entries.emplace_back(i, static_cast<double>(it->second));
            }
        }

        if (phase_entries.empty())
            continue;

        // Step 2: MAD anomaly rejection
        // Compute median
        std::vector<double> durations;
        durations.reserve(phase_entries.size());
        for (const auto& [_, dur] : phase_entries) {
            durations.push_back(dur);
        }
        std::sort(durations.begin(), durations.end());

        double median;
        size_t n = durations.size();
        if (n % 2 == 0) {
            median = (durations[n / 2 - 1] + durations[n / 2]) / 2.0;
        } else {
            median = durations[n / 2];
        }

        // Compute MAD = median of |each - median|
        std::vector<double> abs_devs;
        abs_devs.reserve(n);
        for (double d : durations) {
            abs_devs.push_back(std::abs(d - median));
        }
        std::sort(abs_devs.begin(), abs_devs.end());

        double mad;
        if (n % 2 == 0) {
            mad = (abs_devs[n / 2 - 1] + abs_devs[n / 2]) / 2.0;
        } else {
            mad = abs_devs[n / 2];
        }

        // Step 3: Weighted average of non-anomalous entries
        double total_weight = 0.0;
        double weighted_sum = 0.0;
        for (const auto& [idx, dur] : phase_entries) {
            // Reject if MAD > 0 and deviation exceeds 3*MAD
            if (mad > 0.0 && std::abs(dur - median) > 3.0 * mad) {
                continue;
            }
            total_weight += weights[idx];
            weighted_sum += weights[idx] * dur;
        }

        if (total_weight > 0.0) {
            result[phase] = static_cast<int>(std::round(weighted_sum / total_weight));
        }
    }

    return result;
}

int PreprintPredictor::predicted_total() const {
    // With no history, fall back to sum of default phase durations so UI
    // callers still get a ballpark.
    if (entries_.empty()) {
        int total = 0;
        for (const auto& [_, duration] : default_phase_durations()) {
            total += duration;
        }
        return total;
    }

    // Use weighted average of recorded wall-clock totals. This is the honest
    // end-to-end pre-print duration — it includes heating phases and any
    // gcode/macro time the phase-matching regexes failed to map to a
    // PrintStartPhase. Summing predicted_phases() would silently lose that
    // unmapped time (on AD5M-style printers the phase map captures <40% of
    // real elapsed time).
    auto weights = compute_weights();
    double weighted_sum = 0.0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        weighted_sum += weights[i] * static_cast<double>(entries_[i].total_seconds);
    }
    return static_cast<int>(std::round(weighted_sum));
}

int PreprintPredictor::remaining_seconds(const std::set<int>& completed_phases, int current_phase,
                                         int elapsed_in_current_phase_seconds) const {
    // Only return remaining time when we have real history entries.
    // Defaults are useful for predicted_total()/has_predictions() but not here —
    // the collector uses thermal model for heating and profile weights for progress
    // when no history exists yet.
    if (entries_.empty()) {
        return 0;
    }
    auto phases = predicted_phases();
    int remaining = 0;

    for (const auto& [phase, predicted_duration] : phases) {
        if (completed_phases.count(phase)) {
            // Already done, actual time was spent (not predicted)
            continue;
        }

        if (phase == current_phase && current_phase != 0) {
            // Currently in this phase - subtract elapsed
            remaining += std::max(0, predicted_duration - elapsed_in_current_phase_seconds);
        } else {
            // Future phase
            remaining += predicted_duration;
        }
    }

    return remaining;
}

std::vector<PreprintEntry> PreprintPredictor::load_entries_from_config() {
    auto* cfg = Config::get_instance();
    if (!cfg) {
        return {};
    }

    try {
        auto entries_json =
            cfg->get<nlohmann::json>("/print_start_history/entries", nlohmann::json::array());
        if (!entries_json.is_array() || entries_json.empty()) {
            return {};
        }

        std::vector<PreprintEntry> entries;
        int dropped_legacy = 0;
        for (const auto& ej : entries_json) {
            PreprintEntry entry;
            entry.total_seconds = ej.value("total", 0);
            entry.timestamp = ej.value("timestamp", static_cast<int64_t>(0));
            entry.temp_bucket = ej.value("temp_bucket", 0);
            const int window_raw = ej.value("window", static_cast<int>(PreprintWindow::Unknown));
            entry.window = (window_raw == static_cast<int>(PreprintWindow::PrinterEdge) ||
                            window_raw == static_cast<int>(PreprintWindow::HostPreStart))
                               ? static_cast<PreprintWindow>(window_raw)
                               : PreprintWindow::Unknown;
            // Drop legacy entries that predate the cold/warm bucket scheme.
            // Older versions stored the raw nozzle target temperature here
            // (e.g. 200, 225, 250). Only 0 (unknown), 1 (cold), and 2 (warm)
            // are valid under the current scheme. Dropping at load time
            // causes the next save_prediction_entry() to persist a clean
            // list, so this acts as a one-shot migration.
            if (entry.temp_bucket != 0 && entry.temp_bucket != 1 && entry.temp_bucket != 2) {
                ++dropped_legacy;
                continue;
            }
            if (ej.contains("phases") && ej["phases"].is_object()) {
                for (auto& [key, val] : ej["phases"].items()) {
                    entry.phase_durations[std::stoi(key)] = val.get<int>();
                }
            }
            entries.push_back(std::move(entry));
        }
        if (dropped_legacy > 0) {
            spdlog::info("[PreprintPredictor] Dropped {} legacy history entries "
                         "(pre-cold/warm-bucket scheme)",
                         dropped_legacy);
        }
        return entries;
    } catch (...) {
        return {};
    }
}

int PreprintPredictor::predicted_total_from_config() {
    // Cache result for 60s to avoid re-parsing config on every call
    struct CacheEntry {
        int value{-1};
        int64_t timestamp{0};
    };
    static std::mutex cache_mutex;
    static CacheEntry cache;

    auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache.value >= 0 && (now_sec - cache.timestamp) < 60) {
            return cache.value;
        }
    }

    auto entries = load_entries_from_config();
    int result = 0;
    if (!entries.empty()) {
        PreprintPredictor predictor;
        predictor.load_entries(entries);
        result = predictor.predicted_total();
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache.value = result;
        cache.timestamp = now_sec;
    }
    return result;
}

} // namespace helix
