// SPDX-License-Identifier: GPL-3.0-or-later

#include "preprint_predictor.h"

#include "config.h"
#include "json_utils.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <string>

namespace helix {

namespace {
constexpr const char* kHistoryPath = "/print_start_history/entries";
}

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
        load_entries(in_window, StartCondition::COLD);
        return;
    } else if (temp_bucket == 2) {
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

int PreprintPredictor::temp_bucket_for(float bed_c) {
    return bed_c >= kWarmBedC ? 2 : 1;
}

std::set<int> PreprintPredictor::phases_skipped_by(const PreprintPhaseOverrides& overrides) {
    std::set<int> skipped;
    for (const auto& [phase, runs] : overrides) {
        if (!runs) {
            skipped.insert(phase);
        }
    }
    return skipped;
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
    auto* cfg = Config::get_instance();
    std::string printer_type = cfg->get<std::string>(cfg->df() + helix::wizard::PRINTER_TYPE, "");
    if (!printer_type.empty()) {
        auto db_phases = PrinterDetector::get_print_start_default_phases(printer_type);
        if (!db_phases.empty()) {
            return db_phases;
        }
    }

    return generic_phase_durations();
}

std::map<int, int> PreprintPredictor::generic_phase_durations() {
    // Tuned so a printer that actually runs all six phases still gets a
    // ballpark first-print ETA.
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

int PreprintPredictor::predicted_homing_seconds() const {
    const auto phases = predicted_phases();
    const auto it = phases.find(static_cast<int>(PrintStartPhase::HOMING));
    const int predicted = it != phases.end() ? it->second : 0;
    return std::max(predicted, kMinHomingSeconds);
}

PreprintHeating
PreprintPredictor::with_declared_heating(PreprintHeating heating,
                                         const PreprintDeclaredDurations& declared) {
    if (const auto it = declared.find(static_cast<int>(PrintStartPhase::HEATING_BED));
        it != declared.end()) {
        heating.bed_seconds = static_cast<float>(it->second);
    }
    if (const auto it = declared.find(static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
        it != declared.end()) {
        heating.nozzle_seconds = static_cast<float>(it->second);
        heating.nozzle_target_known = true;
    }
    return heating;
}

PreprintEstimate PreprintPredictor::estimate(const PreprintHeating& priced_heating,
                                             const PreprintPhaseOverrides& requested_overrides,
                                             const PreprintDeclaredDurations& declared) const {
    const PreprintHeating heating = with_declared_heating(priced_heating, declared);
    // The plan is what the macro is about to do with this print's parameters;
    // an override is only what the user asked for before the macro rendered.
    PreprintPhaseOverrides overrides = requested_overrides;
    for (const auto& [phase, _] : declared) {
        overrides.erase(phase);
    }
    const int nozzle_phase = static_cast<int>(PrintStartPhase::HEATING_NOZZLE);
    const int bed_phase = static_cast<int>(PrintStartPhase::HEATING_BED);

    PreprintEstimate result;
    auto& durations = result.durations;
    if (heating.nozzle_seconds > 0.0f)
        durations[nozzle_phase] = heating.nozzle_seconds;
    if (heating.bed_seconds > 0.0f)
        durations[bed_phase] = heating.bed_seconds;

    for (const auto& [phase, secs] : predicted_phases()) {
        if (phase != nozzle_phase && phase != bed_phase) {
            durations[phase] = static_cast<float>(secs);
        }
    }
    durations[static_cast<int>(PrintStartPhase::HOMING)] =
        static_cast<float>(predicted_homing_seconds());

    // With history the recorded wall clock already holds every phase the
    // start ran, including ones the matcher never named, so a run phase adds
    // to it only when every start in the window recorded skipping it, and a
    // skipped one takes off what it averaged across the window. Without
    // history the durations are the total, and a run phase the defaults lack
    // joins them.
    const bool history = has_predictions();
    float wall_clock_delta = 0.0f;
    std::map<int, int> generic;
    for (const auto& [phase, runs] : overrides) {
        const auto it = durations.find(phase);
        if (!runs) {
            if (it == durations.end())
                continue;
            durations.erase(it);
            if (history) {
                wall_clock_delta -= static_cast<float>(
                    weighted_entry_mean([p = phase](const PreprintEntry& e) {
                        const auto d = e.phase_durations.find(p);
                        return d != e.phase_durations.end() ? static_cast<double>(d->second) : 0.0;
                    }).value_or(0.0));
            }
        } else if (it == durations.end() && (!history || every_entry_skipped(phase))) {
            if (generic.empty())
                generic = generic_phase_durations();
            const auto g = generic.find(phase);
            if (g != generic.end()) {
                durations[phase] = static_cast<float>(g->second);
                if (history)
                    wall_clock_delta += static_cast<float>(g->second);
            }
        }
    }

    for (const auto& [phase, secs] : declared) {
        if (secs > 0) {
            durations[phase] = static_cast<float>(secs);
        } else {
            durations.erase(phase);
        }
    }

    // A bed-first macro issues its M109 after homing and leveling; without a
    // placeholder the known phases would carry all the weight.
    if (!heating.nozzle_target_known && durations.find(nozzle_phase) == durations.end()) {
        durations[nozzle_phase] = kUnknownNozzleHeatSeconds;
    }

    float durations_sum = 0.0f;
    for (const auto& [_, dur] : durations) {
        durations_sum += dur;
    }

    if (!history) {
        result.total_seconds = durations_sum;
        return result;
    }

    if (!declared.empty()) {
        result.total_seconds = planned_total(declared, durations).value_or(durations_sum);
        return result;
    }

    // The heating this start owes, priced from where the heaters are now: the
    // recorded wall clock speaks for starts like the ones it measured, and a
    // start colder or aimed hotter than any of them pays heating no history
    // can know. Read from the durations map so the unknown-nozzle placeholder
    // is honoured exactly as the shape honours it.
    float heating_now = 0.0f;
    if (const auto it = durations.find(nozzle_phase); it != durations.end()) {
        heating_now += it->second;
    }
    if (const auto it = durations.find(bed_phase); it != durations.end()) {
        heating_now += it->second;
    }

    // Entries that recorded their priced heating separate the constant part of
    // a start (homing, mesh, soak, purge) from the heating that start owed, so
    // this start pays its own heating plus the learned remainder. A start that
    // cannot price its heating yet (a silent macro sets its targets later)
    // pays the heating those entries learned instead, never less than the
    // placeholder. Legacy entries cannot be split; they keep the recorded wall
    // clock as the honest total, floored by this start's own heating.
    float total = static_cast<float>(predicted_total()) + wall_clock_delta;
    const std::optional<double> residual =
        weighted_entry_mean([](const PreprintEntry& e) -> std::optional<double> {
            if (!e.heating_seconds) {
                return std::nullopt;
            }
            return std::max(0.0, static_cast<double>(e.total_seconds) -
                                     static_cast<double>(*e.heating_seconds));
        });
    if (residual) {
        float heating_owed = heating_now;
        if (!heating.priced_seconds()) {
            const std::optional<double> learned =
                weighted_entry_mean([](const PreprintEntry& e) -> std::optional<double> {
                    if (!e.heating_seconds) {
                        return std::nullopt;
                    }
                    return static_cast<double>(*e.heating_seconds);
                });
            heating_owed = std::max(heating_owed, static_cast<float>(learned.value_or(0.0)));
        }
        total = heating_owed + static_cast<float>(*residual) + wall_clock_delta;
    } else {
        total = std::max(total, heating_now);
    }
    result.total_seconds = std::max(0.0f, total);
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
    return static_cast<int>(std::round(weighted_entry_mean([](const PreprintEntry& e) {
                                           return static_cast<double>(e.total_seconds);
                                       }).value_or(0.0)));
}

std::optional<float> PreprintPredictor::planned_total(const PreprintDeclaredDurations& declared,
                                                      const std::map<int, float>& durations) const {
    const int nozzle_phase = static_cast<int>(PrintStartPhase::HEATING_NOZZLE);
    const int bed_phase = static_cast<int>(PrintStartPhase::HEATING_BED);

    // Only entries that declared a plan can be split: an unplanned wall clock
    // holds its soak unattributed, and subtracting a learned mean would miss it.
    // A declared heater is already inside the entry's priced heating.
    const std::optional<double> residual =
        weighted_entry_mean([&](const PreprintEntry& e) -> std::optional<double> {
            if (!e.declared || !e.heating_seconds) {
                return std::nullopt;
            }
            double left =
                static_cast<double>(e.total_seconds) - static_cast<double>(*e.heating_seconds);
            for (const auto& [phase, secs] : *e.declared) {
                if (phase == nozzle_phase || phase == bed_phase) {
                    continue;
                }
                const auto recorded = e.phase_durations.find(phase);
                left -= static_cast<double>(recorded != e.phase_durations.end() ? recorded->second
                                                                                : secs);
            }
            return std::max(0.0, left);
        });
    if (!residual) {
        return std::nullopt;
    }

    float total = static_cast<float>(*residual);
    for (const int heater : {nozzle_phase, bed_phase}) {
        if (const auto it = durations.find(heater); it != durations.end()) {
            total += it->second;
        }
    }
    for (const auto& [phase, secs] : declared) {
        if (phase != nozzle_phase && phase != bed_phase) {
            total += static_cast<float>(secs);
        }
    }
    return total;
}

bool PreprintPredictor::every_entry_skipped(int phase) const {
    return !entries_.empty() &&
           std::all_of(entries_.begin(), entries_.end(), [phase](const PreprintEntry& e) {
               return e.skipped_phases && e.skipped_phases->count(phase) > 0;
           });
}

std::optional<double> PreprintPredictor::weighted_entry_mean(
    const std::function<std::optional<double>(const PreprintEntry&)>& value) const {
    const auto weights = compute_weights();
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const std::optional<double> v = value(entries_[i]);
        if (!v) {
            continue;
        }
        weighted_sum += weights[i] * *v;
        weight_sum += weights[i];
    }
    if (weight_sum <= 0.0) {
        return std::nullopt;
    }
    return weighted_sum / weight_sum;
}

std::vector<PreprintEntry> PreprintPredictor::load_entries_from_config() {
    auto* cfg = Config::get_instance();

    auto entries_json = cfg->get<nlohmann::json>(kHistoryPath, nlohmann::json::array());
    if (!entries_json.is_array() || entries_json.empty()) {
        return {};
    }

    std::vector<PreprintEntry> entries;
    int dropped_legacy = 0;
    int dropped_phases = 0;
    // A record with a wrongly typed field is dropped; the rest of the history
    // stays, and a zero-filled record never enters a prediction.
    const auto wrongly_typed = [](const nlohmann::json& ej, const char* key) {
        return ej.contains(key) && !ej[key].is_number();
    };
    for (const auto& ej : entries_json) {
        if (!ej.is_object() || wrongly_typed(ej, "total") || wrongly_typed(ej, "timestamp") ||
            wrongly_typed(ej, "temp_bucket") || wrongly_typed(ej, "window")) {
            continue;
        }
        PreprintEntry entry;
        entry.total_seconds = json_util::safe_int(ej, "total", 0);
        entry.timestamp = json_util::safe_int64(ej, "timestamp", 0);
        entry.temp_bucket = json_util::safe_int(ej, "temp_bucket", 0);
        const int window_raw =
            json_util::safe_int(ej, "window", static_cast<int>(PreprintWindow::Unknown));
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
                // A name this build does not know belongs to a newer one.
                // The rest of the entry is still a real measurement, so the
                // key is dropped rather than the entry.
                const auto phase = print_start_phase_from_name(key);
                if (!phase || !val.is_number()) {
                    ++dropped_phases;
                    continue;
                }
                entry.phase_durations[static_cast<int>(*phase)] = val.get<int>();
            }
        }
        if (ej.contains("heating") && ej["heating"].is_number()) {
            entry.heating_seconds = ej["heating"].get<int>();
        }
        if (ej.contains("skipped") && ej["skipped"].is_array()) {
            std::set<int> skipped;
            for (const auto& name : ej["skipped"]) {
                const auto phase = name.is_string()
                                       ? print_start_phase_from_name(name.get<std::string>())
                                       : std::nullopt;
                if (phase) {
                    skipped.insert(static_cast<int>(*phase));
                } else {
                    ++dropped_phases;
                }
            }
            entry.skipped_phases = std::move(skipped);
        }
        if (ej.contains("declared") && ej["declared"].is_object()) {
            PreprintDeclaredDurations declared;
            for (auto& [key, val] : ej["declared"].items()) {
                const auto phase = print_start_phase_from_name(key);
                const auto seconds = val.is_number()
                                         ? declared_seconds_from_number(val.get<double>())
                                         : std::nullopt;
                if (phase && seconds && phase_accepts_declared_duration(*phase)) {
                    declared[static_cast<int>(*phase)] = *seconds;
                } else {
                    ++dropped_phases;
                }
            }
            entry.declared = std::move(declared);
        }
        entries.push_back(std::move(entry));
    }
    if (dropped_legacy > 0) {
        spdlog::info("[PreprintPredictor] Dropped {} legacy history entries "
                     "(pre-cold/warm-bucket scheme)",
                     dropped_legacy);
    }
    if (dropped_phases > 0) {
        spdlog::info("[PreprintPredictor] Skipped {} history phase durations with an "
                     "unrecognised name",
                     dropped_phases);
    }
    return entries;
}

json PreprintPredictor::entries_to_json(const std::vector<PreprintEntry>& entries) {
    json entries_json = json::array();
    for (const auto& e : entries) {
        json entry_json;
        entry_json["total"] = e.total_seconds;
        entry_json["timestamp"] = e.timestamp;

        json phases_json = json::object();
        for (const auto& [phase, duration] : e.phase_durations) {
            const auto name = print_start_phase_name(static_cast<PrintStartPhase>(phase));
            if (name.empty()) {
                spdlog::warn("[PreprintPredictor] Dropping duration for unnamed phase {}", phase);
                continue;
            }
            phases_json[std::string(name)] = duration;
        }
        entry_json["phases"] = phases_json;
        if (e.temp_bucket > 0) {
            entry_json["temp_bucket"] = e.temp_bucket;
        }
        // Omitted for Unknown so the field only appears once it means
        // something; a reader treats its absence as PrinterEdge.
        if (e.window != PreprintWindow::Unknown) {
            entry_json["window"] = static_cast<int>(e.window);
        }
        // Omitted when unknown, so an absent field and a zero differ.
        if (e.heating_seconds) {
            entry_json["heating"] = *e.heating_seconds;
        }
        // Omitted when unknown, so an absent field and an empty list differ.
        if (e.skipped_phases) {
            json skipped_json = json::array();
            for (const int phase : *e.skipped_phases) {
                const auto name = print_start_phase_name(static_cast<PrintStartPhase>(phase));
                if (!name.empty()) {
                    skipped_json.push_back(std::string(name));
                }
            }
            entry_json["skipped"] = std::move(skipped_json);
        }
        // Omitted without a plan, so an absent field and an empty plan differ.
        if (e.declared) {
            json declared_json = json::object();
            for (const auto& [phase, seconds] : *e.declared) {
                const auto name = print_start_phase_name(static_cast<PrintStartPhase>(phase));
                if (!name.empty()) {
                    declared_json[std::string(name)] = seconds;
                }
            }
            entry_json["declared"] = std::move(declared_json);
        }
        entries_json.push_back(std::move(entry_json));
    }
    return entries_json;
}

void PreprintPredictor::append_to_config(const PreprintEntry& entry) {
    auto entries = load_entries_from_config();
    entries.push_back(entry);

    const auto same_population = [&entry](const PreprintEntry& e) {
        return e.temp_bucket == entry.temp_bucket && e.window == entry.window;
    };
    auto population = std::count_if(entries.begin(), entries.end(), same_population);
    for (auto it = entries.begin(); population > MAX_ENTRIES && it != entries.end();) {
        if (same_population(*it)) {
            it = entries.erase(it);
            --population;
        } else {
            ++it;
        }
    }
    while (entries.size() > kMaxStoredEntries) {
        entries.erase(entries.begin());
    }
    Config::get_instance()->set<json>(kHistoryPath, entries_to_json(entries));
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
