// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_controller.h"

#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "system/crash_handler.h"
#include "temperature_history_manager.h"
#include "temperature_sensor_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace helix::temp_graph_internal {

// Compress a time-ordered (ascending) list of sample timestamps down to at most
// `max_points` by bucketing the span into `max_points` equal time slices and
// keeping the last sample in each non-empty slice. The most recent sample is
// always retained; the result is strictly increasing. Lets a dense 1 Hz history
// window fill a coarse chart buffer without dropping the older end (#979).
std::vector<int> decimate_indices(const std::vector<int64_t>& timestamps_ms, int max_points) {
    std::vector<int> keep;
    const int n = static_cast<int>(timestamps_ms.size());
    if (n <= 0 || max_points <= 0)
        return keep;

    if (n <= max_points) {
        keep.reserve(n);
        for (int i = 0; i < n; i++)
            keep.push_back(i);
        return keep;
    }

    const int64_t span = timestamps_ms[n - 1] - timestamps_ms[0];
    if (span <= 0) {
        // Degenerate (all samples share a timestamp): keep just the endpoints.
        keep.push_back(0);
        if (n - 1 != 0)
            keep.push_back(n - 1);
        return keep;
    }

    auto bucket = [&](int i) -> int64_t {
        int64_t b = (timestamps_ms[i] - timestamps_ms[0]) * max_points / span;
        if (b >= max_points)
            b = max_points - 1;
        if (b < 0)
            b = 0;
        return b;
    };

    keep.reserve(max_points);
    for (int i = 0; i < n; i++) {
        // Last sample of its bucket (and always the very last sample).
        if (i == n - 1 || bucket(i) != bucket(i + 1))
            keep.push_back(i);
    }
    return keep;
}

} // namespace helix::temp_graph_internal

namespace helix {

using helix::ui::observe_int_sync;
using helix::ui::temperature::deci_to_degrees_f;

// ============================================================================
// Shared color palette
// ============================================================================

const lv_color_t TEMP_GRAPH_SERIES_COLORS[TEMP_GRAPH_PALETTE_SIZE] = {
    lv_color_hex(0xFF4444), // Nozzle (red)
    lv_color_hex(0x88C0D0), // Bed (cyan / nord8)
    lv_color_hex(0xA3BE8C), // Chamber (green / nord14)
    lv_color_hex(0xEBCB8B), // Yellow / nord13
    lv_color_hex(0xB48EAD), // Purple / nord15
    lv_color_hex(0xD08770), // Orange / nord12
    lv_color_hex(0x5E81AC), // Blue / nord10
    lv_color_hex(0xBF616A), // Dark red / nord11
};

uint32_t temp_graph_series_hex(int index) {
    const int slot =
        ((index % TEMP_GRAPH_PALETTE_SIZE) + TEMP_GRAPH_PALETTE_SIZE) % TEMP_GRAPH_PALETTE_SIZE;
    const lv_color_t c = TEMP_GRAPH_SERIES_COLORS[slot];
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) |
           static_cast<uint32_t>(c.blue);
}

// ============================================================================
// Construction / Destruction
// ============================================================================

namespace {
// Registry of live controllers so refresh_all_from_history() can re-backfill
// every persistent graph after the history manager is (re)seeded. Main-thread
// only: controllers are created, destroyed, and refreshed on the UI thread, so
// no lock is needed. Function-local static keeps init order well-defined vs the
// controllers that register into it (#1124).
std::vector<TempGraphController*>& live_controllers() {
    static std::vector<TempGraphController*> instances;
    return instances;
}
} // namespace

TempGraphController::TempGraphController(lv_obj_t* container,
                                         const TempGraphControllerConfig& config)
    : config_(config), container_(container) {
    live_controllers().push_back(this);

    create_graph();
    if (!graph_) {
        spdlog::error("[TempGraphController] Failed to create graph");
        return;
    }

    setup_series();
    setup_observers();
    backfill_history();
    apply_auto_range();

    spdlog::debug("[TempGraphController] Initialized with {} series", series_.size());
}

void TempGraphController::detach() {
    tearing_down_ = true;
    lifetime_.invalidate();
    // The deferred clear queued by reattach_observers() is generation-guarded,
    // so invalidate() above just cancelled it. Clear here or a later rebuild()
    // would come up with the suppression still latched and never sample again.
    suppress_attach_fire_ = false;

    {
        auto freeze =
            helix::ui::UpdateQueue::instance().scoped_freeze("TempGraphController::detach");
        helix::ui::UpdateQueue::instance().drain();

        connection_observer_.reset();
        discovery_observer_.reset();
        sensor_discovery_observer_.reset();
        for (auto& s : series_) {
            s.temp_obs.reset();
            s.target_obs.reset();
        }
    }

    spdlog::debug("[TempGraphController] Detached observers");
}

TempGraphController::~TempGraphController() {
    auto& reg = live_controllers();
    reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());

    // Safe to call multiple times — idempotent (invalidate + reset are no-ops
    // if detach() was already called before deferred deletion)
    detach();

    series_.clear();

    if (graph_) {
        ui_temp_graph_destroy(graph_);
        graph_ = nullptr;
    }

    spdlog::debug("[TempGraphController] Destroyed");
}

// ============================================================================
// Public interface
// ============================================================================

void TempGraphController::set_features(uint32_t features) {
    if (graph_) {
        ui_temp_graph_set_features(graph_, features);
    }
}

void TempGraphController::pause() {
    paused_ = true;
}

void TempGraphController::resume() {
    paused_ = false;
    refresh_from_history();
}

void TempGraphController::refresh_from_history() {
    backfill_history();
}

void TempGraphController::refresh_all_from_history() {
    auto snapshot = live_controllers();
    spdlog::debug("[TempGraphController] refresh_all_from_history: {} live controllers",
                  snapshot.size());
    for (auto* c : snapshot) {
        if (c) {
            c->refresh_from_history();
        }
    }
}

void TempGraphController::rebuild() {
    // Guard against stale container — if the parent widget was deleted while a
    // deferred observer callback was queued, container_ is dangling.
    // lv_obj_is_valid() is O(n) but acceptable here: rebuild() is the full-reset
    // path, not something any hot loop reaches.
    if (tearing_down_ || !container_ || !lv_obj_is_valid(container_)) {
        spdlog::warn("[TempGraphController] Rebuild skipped — tearing down or container freed");
        container_ = nullptr;
        return;
    }

    crash_handler::breadcrumb::note("tgc", "rebuild", static_cast<long>(generation_ + 1));

    detach();
    // detach() set tearing_down_ to guard the old generation's deferred
    // callbacks. We're building a NEW generation now — clear the flag so
    // setup_observers() proceeds. Without this, every reconnect leaves the
    // graph with zero observers and temps freeze permanently (#1245).
    tearing_down_ = false;

    series_.clear();

    if (graph_) {
        ui_temp_graph_destroy(graph_);
        graph_ = nullptr;
    }

    ++generation_;
    y_axis_max_ = 100.0f;

    create_graph();
    if (!graph_) {
        spdlog::error("[TempGraphController] Failed to recreate graph on rebuild");
        return;
    }

    setup_series();
    setup_observers();
    backfill_history();
    apply_auto_range();

    spdlog::debug("[TempGraphController] Rebuilt with {} series", series_.size());
}

void TempGraphController::reattach_observers() {
    if (tearing_down_ || !graph_)
        return;

    spdlog::debug("[TempGraphController] Re-attaching observers (preserving chart data)");

    // Expire all old tokens so any handler already queued by the old observers
    // returns instead of touching the series we are about to rebind.
    //
    // No freeze/drain pair here, unlike detach(): reattach_observers() is only
    // reachable from the connection observer, which runs inside
    // UpdateQueue::process_pending(). process_pending() has already swapped
    // pending_ into a local queue, so a nested drain() cannot see (let alone
    // flush) the stale callbacks — it would only re-enter the queue for
    // unrelated work, which is exactly the re-entrancy #696 warns about.
    // ObserverGuard::reset() below frees each observer context, expiring the
    // weak_alive token that the already-queued handlers check, so they drop
    // themselves without any drain.
    lifetime_.invalidate();
    connection_observer_.reset();
    discovery_observer_.reset();
    sensor_discovery_observer_.reset();
    for (auto& s : series_) {
        s.temp_obs.reset();
        s.target_obs.reset();
    }

    // New generation — new tokens for the fresh observers.
    ++generation_;

    // Reset each series' SubjectLifetime so subject lookups rebind to the
    // CURRENT subjects. Extruder/sensor subjects may have been recreated
    // during reconnect discovery — without this, the lookup returns the
    // old (dead) subject and the observer silently never fires (#1245).
    for (auto& s : series_) {
        s.lifetime = SubjectLifetime{};
    }

    // Suppress the attach-time fire from observe_int_sync. Those fires push the
    // subject's PRE-reconnect value, and the live handler stamps it with a
    // fresh `now` — a phantom spike bridging the whole disconnect gap (#1245).
    //
    // The suppression cannot be scoped to this function: observe_int_sync's
    // LVGL callback only *queues* the handler (observer_factory.h:344), so the
    // attach-time fires have not run yet when setup_observers() returns.
    // Restoring the flag inline here would restore it before a single one of
    // them executed — the no-op this replaces. Instead, clear it from a
    // callback queued right after them. UpdateQueue's pending_ is a FIFO, and
    // process_pending() drains it in order, so the clear is guaranteed to run
    // after every attach-time fire has been dropped and before any sample that
    // arrives on a later tick.
    suppress_attach_fire_ = true;
    setup_observers();
    lifetime_.defer("TempGraphController::clear_attach_suppression",
                    [this]() { suppress_attach_fire_ = false; });
}

int TempGraphController::series_id_for(const std::string& klipper_name) const {
    for (const auto& s : series_) {
        if (s.klipper_name == klipper_name) {
            return s.series_id;
        }
    }
    return -1;
}

// ============================================================================
// Graph creation
// ============================================================================

void TempGraphController::create_graph() {
    if (!container_ || !lv_obj_is_valid(container_)) {
        spdlog::error("[TempGraphController] Container is null or freed, cannot create graph");
        container_ = nullptr;
        return;
    }

    graph_ = ui_temp_graph_create(container_);
    if (!graph_)
        return;

    // Size chart to fill container
    lv_obj_t* chart = ui_temp_graph_get_chart(graph_);
    if (chart) {
        lv_obj_set_size(chart, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(chart, theme_manager_get_color("card_bg"), 0);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
        // Let clicks pass through chart to parent container
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    ui_temp_graph_set_axis_size(graph_, config_.axis_size);
    ui_temp_graph_set_features(graph_, config_.initial_features);
    ui_temp_graph_set_point_count(graph_, config_.point_count);
}

// ============================================================================
// Series setup
// ============================================================================

void TempGraphController::setup_series() {
    if (!graph_)
        return;

    auto& ps = get_printer_state();

    for (const auto& spec : config_.series) {
        const char* label =
            spec.display_name.empty() ? spec.klipper_name.c_str() : spec.display_name.c_str();
        int series_id = ui_temp_graph_add_series(graph_, label, spec.color);
        if (series_id < 0) {
            spdlog::warn("[TempGraphController] Failed to add series '{}'", spec.klipper_name);
            continue;
        }

        SeriesState state;
        state.klipper_name = spec.klipper_name;
        state.series_id = series_id;
        state.show_target = spec.show_target;

        // Determine if this uses a dynamic subject
        if (spec.klipper_name.find("extruder") == 0 && ps.extruder_count() > 1) {
            state.is_dynamic = true;
        } else if (spec.klipper_name.find("temperature_sensor") == 0 ||
                   spec.klipper_name.find("temperature_fan") == 0) {
            state.is_dynamic = true;
        }

        series_.push_back(std::move(state));
    }

    spdlog::trace("[TempGraphController] Setup {} series", series_.size());
}

// ============================================================================
// Observer setup
// ============================================================================

void TempGraphController::setup_observers() {
    // Belt-and-suspenders against deferred-delete races (#1117): a queued
    // connection-state observer can fire rebuild() → setup_observers() on a
    // controller whose detach() has already started but whose memory hasn't
    // been freed yet. lifetime_.invalidate() ran first, so the per-series
    // observer tokens would early-return, but rebuild() pushes a new
    // generation before reaching here — the tearing_down_ flag is the hard
    // gate that survives across generations.
    if (tearing_down_) {
        spdlog::warn("[TempGraphController] setup_observers skipped — tearing down");
        return;
    }

    // Track the two discovery sources separately. Watching a source we have no
    // series for is not just wasted work: reaching for
    // TemperatureSensorManager::instance() constructs that singleton (and
    // registers its subjects) on first touch, which shows up as a subject leak
    // to any test measuring the registry around an unrelated graph.
    int unresolved_extruder = 0;
    int unresolved_sensor = 0;

    // Auxiliary sensors are whatever does not route to the bed, chamber, or
    // extruder subjects in attach_series_observers().
    auto is_sensor_series = [](const std::string& n) {
        return n != "heater_bed" && n.rfind("heater_generic", 0) != 0 &&
               n.rfind("temperature_fan", 0) != 0 && n.rfind("extruder", 0) != 0;
    };

    for (size_t i = 0; i < series_.size(); ++i) {
        // A provisional binding counts as unresolved: it is producing data, but
        // from a stand-in subject that discovery will replace.
        if (!attach_series_observers(i) || series_[i].provisional) {
            if (is_sensor_series(series_[i].klipper_name)) {
                ++unresolved_sensor;
            } else {
                ++unresolved_extruder;
            }
        }
    }
    const int unresolved = unresolved_extruder + unresolved_sensor;

    // A series whose subject did not exist yet has no observer and would sit
    // frozen on backfilled history for the whole session — the home temp_graph
    // widget is built at app startup, before discovery creates the per-extruder
    // and per-sensor subjects. Watch the discovery version subjects so those
    // series can attach late. Only when something is actually unresolved: a
    // fully-wired graph must not re-resolve on every discovery bump.
    if (unresolved > 0) {
        auto& ps = get_printer_state();
        spdlog::debug("[TempGraphController] {} of {} series unresolved — watching discovery",
                      unresolved, series_.size());

        if (unresolved_extruder > 0) {
            if (auto* extruder_version = ps.get_extruder_version_subject()) {
                discovery_observer_ = observe_int_sync<TempGraphController>(
                    extruder_version, this,
                    [token = lifetime_.token(), gen = generation_](TempGraphController* self, int) {
                        if (token.expired() || gen != self->generation_)
                            return;
                        self->resolve_pending_series();
                    });
            }
        }

        if (unresolved_sensor > 0) {
            auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
            if (auto* sensor_count = sensor_mgr.get_sensor_count_subject()) {
                sensor_discovery_observer_ = observe_int_sync<TempGraphController>(
                    sensor_count, this,
                    [token = lifetime_.token(), gen = generation_](TempGraphController* self, int) {
                        if (token.expired() || gen != self->generation_)
                            return;
                        self->resolve_pending_series();
                    });
            }
        }
    }

    setup_connection_observer();
}

void TempGraphController::resolve_pending_series() {
    if (tearing_down_ || !graph_)
        return;

    int resolved = 0;
    for (size_t i = 0; i < series_.size(); ++i) {
        auto& s = series_[i];
        if (s.temp_obs && !s.provisional)
            continue; // already bound to its real subject
        if (s.provisional) {
            // Drop the stand-in before re-resolving, or the series ends up with
            // two observers pushing into the same chart slot.
            s.temp_obs.reset();
            s.target_obs.reset();
        }
        if (attach_series_observers(i) && !series_[i].provisional)
            ++resolved;
    }

    if (resolved == 0)
        return;

    // The history for these objects only became reachable now too (it is keyed
    // by the same Klipper names), so pull it in rather than waiting for the
    // trace to redraw itself one live sample at a time.
    spdlog::debug("[TempGraphController] Resolved {} series after discovery", resolved);
    backfill_history();
    apply_auto_range();
}

bool TempGraphController::attach_series_observers(size_t i) {
    auto& ps = get_printer_state();
    auto token = lifetime_.token();
    uint32_t gen = generation_;
    bool resolved = false;

    {
        auto& s = series_[i];
        s.provisional = false;

        lv_subject_t* temp_subj = nullptr;
        lv_subject_t* target_subj = nullptr;

        if (s.klipper_name == "heater_bed") {
            temp_subj = ps.get_bed_temp_subject();
            target_subj = ps.get_bed_target_subject();
        } else if (s.klipper_name.find("heater_generic") == 0 ||
                   s.klipper_name.find("temperature_fan") == 0) {
            // Chamber (or other heater/fan-based heaters)
            temp_subj = ps.get_chamber_temp_subject();
            target_subj = ps.get_chamber_target_subject();
        } else if (s.klipper_name.find("extruder") == 0) {
            // Always prefer this extruder's OWN subject — update_from_status
            // publishes one per discovered head, single-tool printers included.
            // The active-extruder subject carries whichever tool is mounted, so
            // binding a named series to it plots the active tool's trace under
            // another tool's label (a changer printing on T4 showed 230°C under
            // "Nozzle 1").
            temp_subj = ps.get_extruder_temp_subject(s.klipper_name, s.lifetime);
            target_subj = ps.get_extruder_target_subject(s.klipper_name, s.lifetime);

            // Nothing discovered yet: the generic "extruder" series can ride the
            // active subject until init_extruders() runs. A numbered head cannot
            // — it stays unresolved so the discovery watcher retries it.
            if (!temp_subj && s.klipper_name == "extruder" && ps.extruder_count() == 0) {
                temp_subj = ps.get_active_extruder_temp_subject();
                target_subj = ps.get_active_extruder_target_subject();
                s.provisional = (temp_subj != nullptr);
            }
        } else {
            // Auxiliary sensor from TemperatureSensorManager
            auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
            temp_subj = sensor_mgr.get_temp_subject(s.klipper_name, s.lifetime);
        }

        resolved = (temp_subj != nullptr);

        if (temp_subj) {
            size_t idx = i;
            s.temp_obs = observe_int_sync<TempGraphController>(
                temp_subj, this,
                [token, gen, idx](TempGraphController* self, int temp_deci) {
                    if (token.expired() || gen != self->generation_)
                        return;
                    if (self->paused_ || self->suppress_attach_fire_ || !self->graph_)
                        return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0)
                        return;

                    // Filter garbage / "no data" readings at the source, mirroring
                    // TemperatureService::on_temp_changed. A heater/sensor subject
                    // momentarily reads 0 on disconnect, partial status updates, or
                    // for an inactive extruder on a multi-tool printer. Pushing a 0
                    // appends a literal 0 sample, so the series line draws a solid
                    // vertical drop to the 0°C floor at the live edge (the reported
                    // U1 artifact). Drop these BEFORE the throttle timestamp update so
                    // a spurious 0 never consumes the per-series sample slot and block
                    // the next real reading. Upper bound rejects obviously-bogus spikes
                    // (deci-degrees: 4000 = 400°C covers any nozzle).
                    constexpr int MAX_VALID_TEMP_DECI = 4000;
                    if (temp_deci <= 0 || temp_deci > MAX_VALID_TEMP_DECI)
                        return;

                    // Throttle chart updates to one sample per SAMPLE_INTERVAL_SEC
                    // per series — Klipper pushes status at ~4Hz, and the chart
                    // only holds one point per interval, so faster pushes just
                    // burn LVGL redraws (the K2 Plus freeze, #979).
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                    if (now_ms - si.last_update_ms < UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000)
                        return;
                    si.last_update_ms = now_ms;

                    float temp_deg = deci_to_degrees_f(temp_deci);
                    // trace, not debug: one line per series per sample interval with
                    // no decision content — the value is already in the subject and on
                    // the chart. The bundle's ring buffer captures DEBUG by default
                    // (ring_captures_debug()), so at debug this single line evicts
                    // every other line: bundle ED2YC336 was 2000/2000 of these.
                    spdlog::trace("[TempGraphController] live push series_id={} '{}' {:.1f}°C",
                                  si.series_id, si.klipper_name, temp_deg);
                    ui_temp_graph_update_series_with_time(self->graph_, si.series_id, temp_deg,
                                                          now_ms);
                    self->apply_auto_range();
                },
                s.lifetime);
        }

        if (target_subj && s.show_target) {
            size_t idx = i;
            s.target_obs = observe_int_sync<TempGraphController>(
                target_subj, this,
                [token, gen, idx](TempGraphController* self, int target_deci) {
                    if (token.expired() || gen != self->generation_)
                        return;
                    if (!self->graph_)
                        return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0)
                        return;

                    float target_deg = deci_to_degrees_f(target_deci);
                    // Stage the new setpoint — the buffer push happens on the
                    // next actuals sample, so multiple target updates between
                    // samples collapse to "latest target wins" naturally.
                    //
                    // Always pass show=true: the series has a target capability
                    // (otherwise this observer wouldn't be registered). The buffer's
                    // 0-sentinel handles "off period" gaps via the segmenter, so we
                    // never want to flip show_target off here — that would erase the
                    // whole historical trace.
                    ui_temp_graph_set_current_target(self->graph_, si.series_id, target_deg, true);
                    self->apply_auto_range();
                },
                s.lifetime);
        }
    }

    return resolved;
}

void TempGraphController::setup_connection_observer() {
    auto& ps = get_printer_state();

    // On reconnect, re-attach observers WITHOUT rebuilding the chart.
    // This refreshes observer bindings to current subjects (in case the
    // printer's sensor configuration changed) while preserving existing
    // chart data, chip visibility, and X-axis timestamps.
    //
    // Transition detection: observe_int_sync fires once synchronously with the
    // current value when attached. Without tracking the previous state, every
    // re-attach would create a new observer that sync-fires CONNECTED and
    // triggers another re-attach — an infinite loop. By tracking prev_state,
    // only ACTUAL state changes (disconnect → reconnect) trigger re-attach.
    // This ensures ALL controllers re-attach, not just the first one (#1245).
    auto* conn_subj = ps.get_printer_connection_state_subject();
    if (conn_subj) {
        auto conn_token = lifetime_.token();
        uint32_t conn_gen = generation_;
        auto prev_state = std::make_shared<int>(lv_subject_get_int(conn_subj));
        connection_observer_ = observe_int_sync<TempGraphController>(
            conn_subj, this,
            [conn_token, conn_gen, prev_state](TempGraphController* self, int state) {
                if (conn_token.expired())
                    return;
                if (conn_gen != self->generation_)
                    return;
                if (state == *prev_state)
                    return;
                *prev_state = state;
                if (state == 0) {
                    spdlog::debug("[TempGraphController] Connection lost — graph paused");
                } else if (state == 2) {
                    spdlog::debug(
                        "[TempGraphController] Connection restored — re-attaching observers");
                    self->reattach_observers();
                }
            });
    }
}

// ============================================================================
// History backfill
// ============================================================================

void TempGraphController::backfill_history() {
    auto* history_mgr = get_temperature_history_manager();
    if (!graph_ || !history_mgr)
        return;

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    int64_t cutoff_ms = now_ms - static_cast<int64_t>(config_.point_count) *
                                     UI_TEMP_GRAPH_SAMPLE_INTERVAL_SEC * 1000;

    for (auto& s : series_) {
        if (s.series_id < 0)
            continue;

        auto samples = history_mgr->get_samples_since(s.klipper_name, cutoff_ms);
        if (samples.empty())
            continue;

        // Decimate to <= point_count by time-bucketing (keeps the newest sample).
        std::vector<int64_t> sample_ts;
        sample_ts.reserve(samples.size());
        for (const auto& sample : samples)
            sample_ts.push_back(sample.timestamp_ms);
        auto kept = temp_graph_internal::decimate_indices(sample_ts, config_.point_count);
        if (kept.empty())
            continue;

        // Build parallel temp / target arrays for one-call replay.
        std::vector<float> temps;
        std::vector<float> targets;
        temps.reserve(kept.size());
        targets.reserve(kept.size());
        for (int idx : kept) {
            const auto& sample = samples[idx];
            temps.push_back(deci_to_degrees_f(sample.temp_deci));
            targets.push_back(s.show_target ? deci_to_degrees_f(sample.target_deci) : 0.0f);
        }

        // Replay both buffers via the dedicated parallel-array API. This also
        // refreshes the chart and updates max_visible_temp. set_series_data marks
        // first_value_received=true so the next live update_series_with_time does
        // not wipe the just-populated buffer via lv_chart_set_all_values.
        ui_temp_graph_set_series_data_with_targets(graph_, s.series_id, temps.data(),
                                                   targets.data(), static_cast<int>(kept.size()));

        const auto& first = samples[kept.front()];
        const auto& last = samples[kept.back()];

        spdlog::trace("[TempGraphController] backfill '{}': {} → {} points, "
                      "{:.1f}C → {:.1f}C, {:.1f} min",
                      s.klipper_name, samples.size(), kept.size(),
                      deci_to_degrees_f(first.temp_deci), deci_to_degrees_f(last.temp_deci),
                      (last.timestamp_ms - first.timestamp_ms) / 60000.0f);

        ui_temp_graph_set_axis_timestamps(graph_, first.timestamp_ms, now_ms,
                                          static_cast<int>(kept.size()));

        // Stage the latest target for the accent tick + next-sample push.
        if (s.show_target) {
            float target_deg = deci_to_degrees_f(last.target_deci);
            // Pass show=true unconditionally — see live-observer comment above for
            // why we don't gate on (target_deg > 0): the buffer's 0-sentinel handles
            // the off-period gap via the segmenter.
            ui_temp_graph_set_current_target(graph_, s.series_id, target_deg, true);
        }
    }
}

// ============================================================================
// Auto-range
// ============================================================================

void TempGraphController::apply_auto_range() {
    if (!graph_)
        return;

    // Find max relevant temperature (from data and targets)
    float max_temp = graph_->max_visible_temp;
    for (const auto& s : series_) {
        if (s.show_target && s.series_id >= 0) {
            for (int j = 0; j < graph_->series_count; j++) {
                auto& meta = graph_->series_meta[j];
                if (meta.id == s.series_id && meta.show_target && meta.target_temp > max_temp) {
                    max_temp = meta.target_temp;
                }
            }
        }
    }

    float new_max = calculate_temp_graph_y_max(y_axis_max_, max_temp, graph_->max_visible_temp,
                                               config_.scale_params);

    bool changed = (new_max != y_axis_max_);
    y_axis_max_ = new_max;

    ui_temp_graph_set_temp_range(graph_, 0.0f, y_axis_max_);

    float y_increment = (y_axis_max_ <= 150.0f) ? 25.0f : 50.0f;
    bool show_y = (ui_temp_graph_get_features(graph_) & TEMP_GRAPH_FEATURE_Y_AXIS) != 0;
    ui_temp_graph_set_y_axis(graph_, y_increment, show_y);

    if (changed) {
        spdlog::trace("[TempGraphController] Y-axis range: 0-{}°C (increment={}, show_y={})",
                      y_axis_max_, y_increment, show_y);
    }
}

} // namespace helix
