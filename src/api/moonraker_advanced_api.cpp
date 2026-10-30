// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_advanced_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"
#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "accel_sensor_manager.h"
#include "app_globals.h"
#include "bed_mesh_probe_parser.h"
#include "gcode_unknown_command.h"
#include "helix_regex.h"
#include "json_utils.h"
#include "moonraker_api.h"
#include "moonraker_validation.h"
#include "observer_factory.h"
#include "operation_timeout_guard.h"
#include "printer_state.h"
#include "probe_preparation.h"
#include "resonance_console.h"
#include "screws_tilt_dialect.h"
#include "screws_tilt_parser.h"
#include "shaper_csv_parser.h"
#include "spdlog/spdlog.h"
#include "standard_macros.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

using namespace helix;

// ============================================================================
// MoonrakerAdvancedAPI Implementation
// ============================================================================

MoonrakerAdvancedAPI::MoonrakerAdvancedAPI(IMoonrakerClient& client, MoonrakerAPI& api)
    : client_(client), api_(api) {}

// ============================================================================
// Domain Service Operations - Bed Mesh
// ============================================================================

void MoonrakerAdvancedAPI::update_bed_mesh(const json& bed_mesh) {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    spdlog::debug("[MoonrakerAPI] update_bed_mesh called with keys: {}", [&]() {
        std::string keys;
        for (auto it = bed_mesh.begin(); it != bed_mesh.end(); ++it) {
            if (!keys.empty())
                keys += ", ";
            keys += it.key();
        }
        return keys;
    }());

    // Parse active profile name
    if (bed_mesh.contains("profile_name") && !bed_mesh["profile_name"].is_null()) {
        active_bed_mesh_.name = bed_mesh["profile_name"].template get<std::string>();
    }

    // Parse probed_matrix (2D array of Z heights)
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        for (const auto& row : bed_mesh["probed_matrix"]) {
            if (row.is_array()) {
                std::vector<float> row_vec;
                for (const auto& val : row) {
                    if (val.is_number()) {
                        row_vec.push_back(val.template get<float>());
                    }
                }
                if (!row_vec.empty()) {
                    active_bed_mesh_.probed_matrix.push_back(row_vec);
                }
            }
        }

        // Update dimensions
        active_bed_mesh_.y_count = static_cast<int>(active_bed_mesh_.probed_matrix.size());
        active_bed_mesh_.x_count = active_bed_mesh_.probed_matrix.empty()
                                       ? 0
                                       : static_cast<int>(active_bed_mesh_.probed_matrix[0].size());
    }

    // Parse mesh bounds (check that elements are numbers, not null)
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array() &&
        bed_mesh["mesh_min"].size() >= 2 && bed_mesh["mesh_min"][0].is_number() &&
        bed_mesh["mesh_min"][1].is_number()) {
        active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].template get<float>();
        active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].template get<float>();
    }

    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array() &&
        bed_mesh["mesh_max"].size() >= 2 && bed_mesh["mesh_max"][0].is_number() &&
        bed_mesh["mesh_max"][1].is_number()) {
        active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].template get<float>();
        active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].template get<float>();
    }

    // Parse available profiles and their mesh data
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        stored_bed_mesh_profiles_.clear();

        spdlog::debug("[MoonrakerAPI] Parsing {} bed mesh profiles", bed_mesh["profiles"].size());

        for (auto& [profile_name, profile_data] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(profile_name);

            // Parse and store mesh data for this profile (if available)
            if (profile_data.is_object()) {
                BedMeshProfile profile;
                profile.name = profile_name;

                // Parse points array (Moonraker calls it "points", not "probed_matrix")
                if (profile_data.contains("points") && profile_data["points"].is_array()) {
                    for (const auto& row : profile_data["points"]) {
                        if (row.is_array()) {
                            std::vector<float> row_vec;
                            for (const auto& val : row) {
                                if (val.is_number()) {
                                    row_vec.push_back(val.template get<float>());
                                }
                            }
                            if (!row_vec.empty()) {
                                profile.probed_matrix.push_back(row_vec);
                            }
                        }
                    }
                }

                // Parse mesh bounds
                if (profile_data.contains("mesh_params") &&
                    profile_data["mesh_params"].is_object()) {
                    const auto& params = profile_data["mesh_params"];
                    if (params.contains("min_x"))
                        profile.mesh_min[0] = params["min_x"].template get<float>();
                    if (params.contains("min_y"))
                        profile.mesh_min[1] = params["min_y"].template get<float>();
                    if (params.contains("max_x"))
                        profile.mesh_max[0] = params["max_x"].template get<float>();
                    if (params.contains("max_y"))
                        profile.mesh_max[1] = params["max_y"].template get<float>();
                    if (params.contains("x_count"))
                        profile.x_count = params["x_count"].template get<int>();
                    if (params.contains("y_count"))
                        profile.y_count = params["y_count"].template get<int>();
                }

                if (!profile.probed_matrix.empty()) {
                    stored_bed_mesh_profiles_[profile_name] = std::move(profile);
                }
            }
        }
    }

    // Parse algorithm from mesh_params (if available)
    if (bed_mesh.contains("mesh_params") && bed_mesh["mesh_params"].is_object()) {
        const json& params = bed_mesh["mesh_params"];
        if (params.contains("algo") && params["algo"].is_string()) {
            active_bed_mesh_.algo = params["algo"].template get<std::string>();
        }
    }

    if (active_bed_mesh_.probed_matrix.empty()) {
        spdlog::debug("[MoonrakerAPI] Bed mesh data cleared (no probed_matrix)");
    } else {
        spdlog::debug("[MoonrakerAPI] Bed mesh updated: profile='{}', size={}x{}, "
                      "profiles={}, algo='{}'",
                      active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count,
                      bed_mesh_profiles_.size(), active_bed_mesh_.algo);
    }
}

const BedMeshProfile* MoonrakerAdvancedAPI::get_active_bed_mesh() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    if (active_bed_mesh_.probed_matrix.empty()) {
        return nullptr;
    }
    return &active_bed_mesh_;
}

std::vector<std::string> MoonrakerAdvancedAPI::get_bed_mesh_profiles() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);
    return bed_mesh_profiles_;
}

bool MoonrakerAdvancedAPI::has_bed_mesh() const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);
    return !active_bed_mesh_.probed_matrix.empty();
}

const BedMeshProfile*
MoonrakerAdvancedAPI::get_bed_mesh_profile(const std::string& profile_name) const {
    std::lock_guard<std::mutex> lock(bed_mesh_mutex_);

    // Check stored profiles first
    auto it = stored_bed_mesh_profiles_.find(profile_name);
    if (it != stored_bed_mesh_profiles_.end()) {
        return &it->second;
    }

    // Fall back to active mesh if name matches
    if (active_bed_mesh_.name == profile_name && !active_bed_mesh_.probed_matrix.empty()) {
        return &active_bed_mesh_;
    }

    return nullptr;
}

void MoonrakerAdvancedAPI::get_excluded_objects(
    std::function<void(const std::set<std::string>&)> on_success, ErrorCallback on_error) {
    // Query exclude_object state from Klipper
    json params = {{"objects", json::object({{"exclude_object", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success](const json& response) {
            std::set<std::string> excluded;

            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].contains("exclude_object")) {
                const json& exclude_obj = response["result"]["status"]["exclude_object"];

                // excluded_objects is an array of object names
                if (exclude_obj.contains("excluded_objects") &&
                    exclude_obj["excluded_objects"].is_array()) {
                    for (const auto& obj : exclude_obj["excluded_objects"]) {
                        if (obj.is_string()) {
                            excluded.insert(obj.get<std::string>());
                        }
                    }
                }
            }

            spdlog::debug("[Moonraker API] get_excluded_objects() -> {} objects", excluded.size());
            if (on_success) {
                on_success(excluded);
            }
        },
        on_error);
}

void MoonrakerAdvancedAPI::get_available_objects(
    std::function<void(const std::vector<std::string>&)> on_success, ErrorCallback on_error) {
    // Query exclude_object state from Klipper
    json params = {{"objects", json::object({{"exclude_object", nullptr}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success](const json& response) {
            std::vector<std::string> objects;

            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].contains("exclude_object")) {
                const json& exclude_obj = response["result"]["status"]["exclude_object"];

                // objects is an array of {name, center, polygon} objects
                if (exclude_obj.contains("objects") && exclude_obj["objects"].is_array()) {
                    for (const auto& obj : exclude_obj["objects"]) {
                        if (obj.is_object() && obj.contains("name") && obj["name"].is_string()) {
                            objects.push_back(obj["name"].get<std::string>());
                        }
                    }
                }
            }

            spdlog::debug("[Moonraker API] get_available_objects() -> {} objects", objects.size());
            if (on_success) {
                on_success(objects);
            }
        },
        on_error);
}

// ============================================================================
// ADVANCED PANEL STUB IMPLEMENTATIONS
// ============================================================================
// These methods are placeholders for future implementation.

namespace helix {
/**
 * Shared lifecycle for the calibration collectors: notify_gcode_response
 * registration and teardown, the completion gate, and the classification
 * that decides whether an RPC error was the transport vanishing or the
 * printer's opinion of the macro.
 *
 * A dropped WebSocket, a timeout that a slow calibration simply outlived —
 * neither is Klipper telling us the macro failed, so a collector that treats
 * them as terminal unregisters mid-run and the UI reports failure, re-arms
 * Retry, or cools the heaters against a printer that is still executing
 * (prestonbrown/helixscreen#1543). Collectors therefore absorb TIMEOUT and
 * CONNECTION_LOST and keep listening for the result lines; anything carrying
 * Klipper's own complaint is still a real failure and stays terminal.
 */
class CalibrationCollectorCore {
  public:
    CalibrationCollectorCore(IMoonrakerClient& client, const char* handler_prefix)
        : client_(client), handler_name_(std::string(handler_prefix) + std::to_string(next_id())) {}

    ~CalibrationCollectorCore() {
        clear_armed_timers();
        unregister();
    }

    /// Register the forwarding handler. on_msg runs on the Websocket thread.
    /// @param owner The collector that holds this core. The client's handler
    ///        map is the collector's only strong owner, so every piece of
    ///        queued main-thread work takes a temporary reference through
    ///        @p owner: that keeps the collector alive until the work runs, and
    ///        pins its destruction — and with it the fallback's LVGL teardown —
    ///        to the main thread.
    void start(std::weak_ptr<void> owner, const helix::SharedCallback<void(const json&)>& on_msg) {
        owner_ = std::move(owner);
        client_.register_method_callback("notify_gcode_response", handler_name_, on_msg);
        registered_.store(true);
        spdlog::debug("[{}] Started (handler: {})", handler_name_, handler_name_);
    }

    void unregister() {
        if (registered_.exchange(false)) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[{}] Unregistered", handler_name_);
        }
    }

    void mark_completed() {
        completed_.store(true);
        defer_disarm();
    }

    /// Mark completed; false when completion was already recorded, so each
    /// collector's complete-once guard keeps its semantics.
    [[nodiscard]] bool try_complete() {
        const bool first = !completed_.exchange(true);
        if (first)
            defer_disarm();
        return first;
    }

    /// Completion callbacks run on the WebSocket thread, while the fallback's
    /// observer and timers are LVGL state. Tearing those down off the main
    /// thread races lv_timer_handler()/lv_subject_notify(), so completion only
    /// queues the disarm; the armed fallback's own (main-thread) callbacks
    /// still disarm directly, and disarm_idle_fallback() is idempotent.
    void defer_disarm() {
        auto owner = owner_.lock();
        if (!owner) {
            return; // the collector is already being destroyed
        }
        // The captured owner reference keeps the collector, and with it this core,
        // alive until the drain.
        helix::ui::queue_update(
            // QUEUE_RAW_THIS_OK: owner reference above.
            "CalibrationCollectorCore::defer_disarm", [this, owner]() { disarm_idle_fallback(); });
    }

    [[nodiscard]] bool completed() const {
        return completed_.load();
    }

    /// The owner's two terminal handlers for an absorbed transport error:
    /// on_idle runs when the printer's busy->idle edge proves the macro
    /// finished (the idle subject freezes while offline, so the edge can only
    /// arrive after a reconnect); on_unrecovered runs when the follow-up
    /// concludes the macro is not coming back with an answer. Bind them in the
    /// collector's start() capturing a raw `this`: they are non-owning, because
    /// a handler that held the collector would keep it — and the fallback's
    /// lv_timer — alive for the process. conclude_idle()/conclude_unrecovered()
    /// hold a strong owner reference across the call instead.
    /// @param edge_immediate True when the busy->idle edge is a definitive
    ///        completion signal by itself (screws tilt, bed mesh — their data
    ///        is complete the moment the macro finishes). False for
    ///        line-driven collectors: the result lines can trail the edge by a
    ///        moment, so the edge starts a short grace window instead.
    void set_idle_fallback(helix::SharedCallback<void()> on_idle,
                           helix::SharedCallback<void(const std::string&)> on_unrecovered,
                           bool edge_immediate = false) {
        fallback_.on_idle = std::move(on_idle);
        fallback_.on_unrecovered = std::move(on_unrecovered);
        fallback_.edge_immediate = edge_immediate;
    }

    /// Arm the busy->idle follow-up after an absorbed transport error. Runs on
    /// the main thread (observer attach and the backstop timer both require
    /// it); the driver's error callback may fire on the WebSocket thread, so
    /// the work is queued.
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        auto owner = owner_.lock();
        if (!owner) {
            return; // the collector is already being destroyed
        }
        // The captured owner reference keeps the collector, and with it this core,
        // alive until the drain.
        helix::ui::queue_update(
            // QUEUE_RAW_THIS_OK: owner reference above.
            "CalibrationCollectorCore::arm_idle_fallback", [this, owner, &state, backstop_ms]() {
                if (completed() || fallback_.armed) {
                    return;
                }
                fallback_.armed = true;
                std::shared_ptr<bool> subject_lifetime;
                lv_subject_t* idle_subject =
                    state.calibration_state().get_idle_timeout_printing_subject(subject_lifetime);
                if (idle_subject) {
                    fallback_.was_busy_at_arm = lv_subject_get_int(idle_subject) == 1;
                    // The subject is freed by PrinterCalibrationState's
                    // deinit_subjects() on a printer switch (and by test
                    // re-init); the lifetime token makes the observer's
                    // removal skip the freed node instead of chasing it.
                    fallback_.observer = helix::ui::observe<int>(
                        idle_subject, this,
                        [idle_subject](CalibrationCollectorCore* self, int printing) {
                            self->on_idle_report(idle_subject, printing == 1);
                        },
                        subject_lifetime);
                }
                fallback_.backstop.begin(backstop_ms, [this, idle_subject]() {
                    if (!fallback_.armed) {
                        return;
                    }
                    const bool still_busy = idle_subject && lv_subject_get_int(idle_subject) == 1;
                    if (still_busy) {
                        conclude_unrecovered("printer still busy after extended wait");
                    } else if (fallback_.edge_immediate && !fallback_.was_busy_at_arm) {
                        // The re-read has to give the same answer the idle
                        // reports give: a printer never seen busy proves nothing
                        // ran, and completion for these collectors means
                        // claiming data the printer may never have produced.
                        conclude_unrecovered(kMacroMayNotHaveRun);
                    } else {
                        // Idle after a run we saw start: the edge itself was
                        // missed, not the completion.
                        conclude_idle();
                    }
                });
                s_armed_backstop.store(&fallback_.backstop, std::memory_order_release);
            });
    }

    /// Idempotent: the fallback's own callbacks disarm directly and completion
    /// queues a disarm, so both can land for one collector.
    void disarm_idle_fallback() {
        fallback_.armed = false;
        fallback_.on_idle = nullptr;
        fallback_.on_unrecovered = nullptr;
        fallback_.observer.reset();
        fallback_.backstop.end();
        fallback_.grace.end();
        clear_armed_timers();
    }

    /// The backstop armed by the most recent absorbed transport error, or
    /// nullptr when none is armed. Test observability only: the collectors are
    /// file-local and these budgets run to seconds and minutes, so a test fires
    /// the timer the driver installed instead of waiting it out — which also
    /// keeps the wait from racing whatever else the elapsed time would set off.
    /// Written only from arm/begin_grace/disarm, which are main-thread.
    static OperationTimeoutGuard* armed_backstop() {
        return s_armed_backstop.load(std::memory_order_acquire);
    }

    /// @see armed_backstop()
    static OperationTimeoutGuard* armed_grace() {
        return s_armed_grace.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool registered() const {
        return registered_.load();
    }

  private:
    /// The armed fallback's view of the printer's busy flag, deferred out of
    /// the subject notification by observe<int>().
    void on_idle_report(lv_subject_t* subject, bool busy_now) {
        if (!fallback_.armed) {
            return;
        }
        // Edge_immediate with an idle printer at arm: the macro may never have
        // run (the socket was already down at send), or its busy status may
        // simply not have arrived before the drop. A busy report proves it is
        // running; failing outright on the first idle report would mis-fire on
        // a status frame that merely lags the macro's start, so the never-busy
        // case waits out the grace window like the line-driven collectors do.
        if (fallback_.edge_immediate && !fallback_.was_busy_at_arm) {
            if (busy_now) {
                fallback_.was_busy_at_arm = true;
                return;
            }
            begin_grace(subject, /*idle_completes=*/false);
            return;
        }
        if (busy_now) {
            return;
        }
        if (fallback_.edge_immediate) {
            conclude_idle();
            return;
        }
        // Line-driven: result lines can trail the edge. Give them a grace
        // window before concluding the results were lost; the long ceiling
        // backstop still covers the still-busy case above this.
        begin_grace(subject, /*idle_completes=*/true);
    }

    /// Wait out kEdgeGraceMs before acting on an idle report; a busy report
    /// inside the window means the macro is running after all, and cancels it.
    /// @param idle_completes True when a printer still idle at the end of the
    ///        window means the run finished and only its result lines were lost;
    ///        false when the printer was never seen busy, where a still-idle
    ///        printer means the macro may never have run at all.
    void begin_grace(lv_subject_t* subject, bool idle_completes) {
        fallback_.grace.begin(kEdgeGraceMs, [this, subject, idle_completes]() {
            if (!fallback_.armed || completed()) {
                fallback_.grace.end();
                return;
            }
            if (subject && lv_subject_get_int(subject) == 1) {
                fallback_.was_busy_at_arm = true;
                fallback_.grace.end();
                return;
            }
            if (idle_completes) {
                conclude_idle();
            } else {
                conclude_unrecovered(kMacroMayNotHaveRun);
            }
        });
        s_armed_grace.store(&fallback_.grace, std::memory_order_release);
    }

    /// Hand the owner its terminal answer on the next main-thread tick. The
    /// owner's handler unregisters the collector, and the collector owns this
    /// core along with the timer or observer context whose callback is running,
    /// so an inline call would free both mid-call. The queued lambda carries a
    /// strong owner reference, so the collector is alive to receive the call and
    /// is destroyed on the main thread.
    void conclude_idle() {
        auto owner = owner_.lock();
        auto on_idle = std::move(fallback_.on_idle);
        disarm_idle_fallback();
        if (!on_idle || !owner) {
            return;
        }
        helix::ui::queue_update("CalibrationCollectorCore::conclude_idle",
                                [owner, on_idle = std::move(on_idle)]() { on_idle(); });
    }

    /// @see conclude_idle()
    void conclude_unrecovered(const char* why) {
        auto owner = owner_.lock();
        auto on_lost = std::move(fallback_.on_unrecovered);
        disarm_idle_fallback();
        if (!on_lost || !owner) {
            return;
        }
        helix::ui::queue_update("CalibrationCollectorCore::conclude_unrecovered",
                                [owner, on_lost = std::move(on_lost), why]() { on_lost(why); });
    }

    static uint64_t next_id() {
        static std::atomic<uint64_t> s_collector_id{0};
        return ++s_collector_id;
    }

    IMoonrakerClient& client_;
    /// The collector that holds this core, weakly: see start(). Queued
    /// main-thread work locks it, so work that outlives the collector is
    /// dropped and work that does not run pins the destruction to this thread.
    std::weak_ptr<void> owner_;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};

    /// How long the line-driven collectors let result lines trail the
    /// busy->idle edge before concluding they were lost to the outage.
    static constexpr uint32_t kEdgeGraceMs = 3000;

    /// The answer for a printer that was never seen busy: nothing proves the
    /// macro reached it before the socket dropped.
    static constexpr const char* kMacroMayNotHaveRun =
        "the macro may not have run before the connection dropped";

    /// Atomic because the destructor may run on the WebSocket thread while a
    /// later calibration arms from the main thread.
    static std::atomic<OperationTimeoutGuard*> s_armed_backstop;
    static std::atomic<OperationTimeoutGuard*> s_armed_grace;

    void clear_armed_timers() {
        OperationTimeoutGuard* backstop = &fallback_.backstop;
        s_armed_backstop.compare_exchange_strong(backstop, nullptr);
        OperationTimeoutGuard* grace = &fallback_.grace;
        s_armed_grace.compare_exchange_strong(grace, nullptr);
    }

    struct IdleEdgeFallback {
        bool armed = false;
        bool edge_immediate = false;
        /// Subject value at arm time. For edge_immediate collectors an idle
        /// report while this is false means the macro may never have run at
        /// all (the socket was already down at send): completing as success
        /// would claim a mesh the printer never probed.
        bool was_busy_at_arm = false;
        ObserverGuard observer;
        OperationTimeoutGuard backstop;
        OperationTimeoutGuard grace;
        helix::SharedCallback<void()> on_idle;
        helix::SharedCallback<void(const std::string&)> on_unrecovered;
    } fallback_;
};
std::atomic<OperationTimeoutGuard*> CalibrationCollectorCore::s_armed_backstop{nullptr};
std::atomic<OperationTimeoutGuard*> CalibrationCollectorCore::s_armed_grace{nullptr};
} // namespace helix

namespace helix::calibration {
OperationTimeoutGuard* armed_idle_backstop() {
    return CalibrationCollectorCore::armed_backstop();
}
OperationTimeoutGuard* armed_idle_grace() {
    return CalibrationCollectorCore::armed_grace();
}
} // namespace helix::calibration

namespace helix {

/// The one RPC-error policy every calibration driver shares: absorb transport
/// losses (keep the collector listening — the macro may still be running),
/// terminate on the printer's own opinion. @p extra runs ahead of the absorb
/// return for drivers that record stall diagnostics.
template <typename Collector>
void report_collector_rpc_error(const char* cmd, PrinterState& state,
                                const std::shared_ptr<Collector>& collector,
                                const MoonrakerAdvancedAPI::ErrorCallback& on_error,
                                const MoonrakerError& err, uint32_t backstop_ms,
                                const helix::SharedCallback<void()>& extra = nullptr) {
    if (err.is_transport_loss()) {
        if (extra)
            extra();
        spdlog::warn("[MoonrakerAPI] {} RPC lost to the transport ({}); collector still "
                     "listening - calibration may still be running",
                     cmd, (err.type == MoonrakerErrorType::TIMEOUT ? "timeout" : "disconnect"));
        collector->arm_idle_fallback(state, backstop_ms);
        return;
    }
    spdlog::error("[MoonrakerAPI] Failed to send {}: {}", cmd, err.message);
    collector->mark_completed();
    collector->unregister();
    if (on_error)
        on_error(err);
}

} // namespace helix

// NOTE: start_bed_mesh_calibrate is implemented after BedMeshProgressCollector class below.

namespace helix {
/**
 * @brief Collector for PID_CALIBRATE gcode responses
 *
 * Klipper sends PID calibration results as console output via notify_gcode_response.
 * This class monitors for the result line containing pid_Kp, pid_Ki, pid_Kd values.
 *
 * Expected output format:
 *   PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178
 *
 * Error handling:
 *   - "Unknown command" with "PID_CALIBRATE" - command not recognized
 *   - "Error"/"error"/"!! " - Klipper error messages
 *
 * Note: No timeout is implemented. Caller should implement UI-level timeout if needed.
 */
class PIDCalibrateCollector : public std::enable_shared_from_this<PIDCalibrateCollector> {
  public:
    using PIDCallback = std::function<void(float kp, float ki, float kd)>;
    using PIDProgressCallback = std::function<void(int sample, float tolerance)>;

    PIDCalibrateCollector(IMoonrakerClient& client, PIDCallback on_success,
                          MoonrakerAdvancedAPI::ErrorCallback on_error,
                          PIDProgressCallback on_progress = nullptr)
        : core_(client, "pid_calibrate_collector_"), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)), on_progress_(std::move(on_progress)) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        core_.set_idle_fallback(
            [this]() {
                complete_error("PID calibration result unavailable - the printer finished before "
                               "the result arrived");
            },
            [this](const std::string& why) { complete_error("PID_CALIBRATE " + why); });
    }

    void unregister() {
        core_.unregister();
    }

    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    void on_gcode_response(const json& msg) {
        if (core_.completed())
            return;
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string())
            return;

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[PIDCalibrateCollector] Received: {}", line);

        // Check for progress: "sample:1 pwm:0.5 asymmetry:0.2 tolerance:n/a"
        static const helix::Regex sample_regex(
            R"(sample:(\d+)\s+pwm:[\d.]+\s+asymmetry:[\d.]+\s+tolerance:(\S+))");
        helix::RegexMatch progress_match;
        if (helix::regex_search(line, progress_match, sample_regex)) {
            int sample_num = text_io::parse_leading<int>(progress_match[1].str()).value_or(0);
            float tolerance_val = -1.0f;
            std::string tol_str = progress_match[2].str();
            if (tol_str != "n/a") {
                tolerance_val = text_io::parse_leading<float>(tol_str).value_or(tolerance_val);
            }
            spdlog::debug("[PIDCalibrateCollector] Progress: sample={} tolerance={}", sample_num,
                          tolerance_val);
            if (on_progress_)
                on_progress_(sample_num, tolerance_val);
            return;
        }

        // Check for PID result: "PID parameters: pid_Kp=22.865 pid_Ki=1.292 pid_Kd=101.178"
        static const helix::Regex pid_regex(
            R"(pid_Kp=([\d.]+)\s+pid_Ki=([\d.]+)\s+pid_Kd=([\d.]+))");
        helix::RegexMatch match;
        if (helix::regex_search(line, match, pid_regex) && match.size() == 4) {
            const auto kp = text_io::parse_leading<float>(match[1].str());
            const auto ki = text_io::parse_leading<float>(match[2].str());
            const auto kd = text_io::parse_leading<float>(match[3].str());
            if (kp && ki && kd) {
                complete_success(*kp, *ki, *kd);
            }
            return;
        }

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("PID_CALIBRATE") != std::string::npos) {
            complete_error("PID_CALIBRATE command not recognized. Check Klipper configuration.");
            return;
        }

        // Broader error detection
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos ||
            line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

  private:
    void complete_success(float kp, float ki, float kd) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete())
            return;
        spdlog::info("[PIDCalibrateCollector] PID result: Kp={:.3f} Ki={:.3f} Kd={:.3f}", kp, ki,
                     kd);
        unregister();
        if (on_success_)
            on_success_(kp, ki, kd);
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete())
            return;
        spdlog::error("[PIDCalibrateCollector] Error: {}", message);
        unregister();
        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("PID_CALIBRATE", message);
            on_error_(err);
        }
    }

    // Declared first so it tears down last: unregister() in the member dtor
    // runs while the client still outlives this collector's callbacks.
    CalibrationCollectorCore core_;
    PIDCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    PIDProgressCallback on_progress_;
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for collecting an automatic pressure-advance run
 *
 * The firmware measures pressure advance itself and reports one number. Which
 * firmware, what the command is called and what its output looks like all come
 * in on the Procedure (helix::pacal) - this collector only applies the patterns
 * it is handed, so a second firmware needs no change here.
 *
 * Expected output is whatever `proc.result_pattern` matches, in the shape
 * that provider's firmware prints its final value.
 *
 * Progress is best-effort. `proc.attempt_pattern` matches a per-candidate
 * measurement line and the collector COUNTS matches rather than reading a
 * number out of one, because no firmware publishes that line as a contract. A
 * pattern that never matches costs nothing: the panel simply shows no attempt
 * chips, and the result path is untouched.
 *
 * Error handling:
 *   - "Unknown command" naming proc.command_word: firmware cannot do this
 *   - a line starting "!! " or "Error:": a Klipper error
 *   - proc.failure_pattern: the firmware ending the run in its own words
 *
 * No timeout here; the caller owns the UI-level one.
 */
class PACalibrateCollector : public std::enable_shared_from_this<PACalibrateCollector> {
  public:
    using PACallback = IAdvancedAPI::PACalibrateCallback;
    using PAProgressCallback = IAdvancedAPI::PAProgressCallback;

    PACalibrateCollector(IMoonrakerClient& client, helix::pacal::Procedure proc,
                         PACallback on_success, MoonrakerAdvancedAPI::ErrorCallback on_error,
                         PAProgressCallback on_progress = nullptr)
        : client_(client), proc_(std::move(proc)), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)), on_progress_(std::move(on_progress)),
          result_re_(proc_.result_pattern) {
        if (!proc_.attempt_pattern.empty()) {
            attempt_re_ = helix::Regex(proc_.attempt_pattern);
            has_attempt_re_ = true;
        }
        if (!proc_.failure_pattern.empty()) {
            failure_re_ = helix::Regex(proc_.failure_pattern);
            has_failure_re_ = true;
        }
    }

    ~PACalibrateCollector() {
        unregister();
    }

    void start() {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "pa_calibrate_collector_" + std::to_string(++s_collector_id);
        auto self = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
        registered_.store(true);
        spdlog::debug("[PACalibrateCollector] Started (handler: {}, provider: {})", handler_name_,
                      proc_.provider);
    }

    void unregister() {
        bool was = registered_.exchange(false);
        if (was) {
            client_.unregister_method_callback("notify_gcode_response", handler_name_);
            spdlog::debug("[PACalibrateCollector] Unregistered");
        }
    }

    void mark_completed() {
        completed_.store(true);
    }

    void on_gcode_response(const json& msg) {
        if (completed_.load())
            return;
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string())
            return;

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[PACalibrateCollector] Received: {}", line);

        // Result first: on firmwares that apply the value through
        // SET_PRESSURE_ADVANCE the winning line can also satisfy a loose
        // attempt pattern, and reading it as progress would drop the result.
        helix::RegexMatch match;
        if (helix::regex_search(line, match, result_re_) && match.size() >= 2) {
            const auto k = text_io::parse_leading<float>(match[1].str());
            if (k) {
                complete_success(*k);
            } else {
                spdlog::warn("[PACalibrateCollector] Unparseable K in '{}'", line);
            }
            return;
        }

        helix::RegexMatch attempt_match;
        if (has_attempt_re_ && helix::regex_search(line, attempt_match, attempt_re_)) {
            const int attempt = ++attempts_seen_;
            // The candidate K this attempt tried, when the pattern captures it.
            // Without it the panel's "K so far" readout has nothing to show and
            // a repeating extrusion looks like a stuck machine.
            float k_so_far = -1.0f;
            if (attempt_match.size() >= 2) {
                k_so_far = text_io::parse_leading<float>(attempt_match[1].str()).value_or(-1.0f);
            }
            spdlog::debug("[PACalibrateCollector] Attempt {} of ~{} (k={:.4f})", attempt,
                          proc_.expected_attempts, k_so_far);
            if (on_progress_)
                on_progress_(attempt, proc_.expected_attempts, k_so_far);
            return;
        }

        // The command is not installed on this firmware. Worth its own message:
        // it is a capability problem, not a run that went wrong.
        if (line.find("Unknown command") != std::string::npos &&
            line.find(proc_.command_word) != std::string::npos) {
            complete_error(proc_.command_word +
                           " is not available on this printer, so pressure advance"
                           " cannot be measured automatically.");
            return;
        }

        // Only a line Klipper marks as an error: progress lines can carry words
        // like "fitting error" and must not end the run.
        if (line.rfind("!! ", 0) == 0 || line.rfind("Error:", 0) == 0 ||
            (has_failure_re_ && helix::regex_search(line, failure_re_))) {
            complete_error(line);
            return;
        }
    }

  private:
    void complete_success(float k) {
        if (completed_.exchange(true))
            return;
        spdlog::info("[PACalibrateCollector] Pressure advance measured: {:.4f} ({})", k,
                     proc_.provider);
        unregister();
        if (on_success_)
            on_success_(k);
    }

    void complete_error(const std::string& message) {
        if (completed_.exchange(true))
            return;
        spdlog::error("[PACalibrateCollector] Error: {}", message);
        unregister();
        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error(proc_.command_word, message);
            on_error_(err);
        }
    }

    IMoonrakerClient& client_;
    helix::pacal::Procedure proc_;
    PACallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    PAProgressCallback on_progress_;
    helix::Regex result_re_;
    helix::Regex attempt_re_;
    bool has_attempt_re_ = false;
    helix::Regex failure_re_;
    bool has_failure_re_ = false;
    int attempts_seen_ = 0;
    std::string handler_name_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> completed_{false};
};

/**
 * @brief State machine for collecting MPC_CALIBRATE gcode responses
 *
 * Kalico sends MPC calibration output as multiple gcode_response lines.
 * The collector tracks calibration phases for progress reporting and accumulates
 * result values from multiple lines after "Finished MPC calibration".
 *
 * Result lines arrive separately:
 *   block_heat_capacity=18.5432 [J/K]
 *   sensor_responsiveness=0.123456 [K/s/K]
 *   ambient_transfer=0.078901 [W/K]
 *   fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]
 *
 * The collector fires the success callback once the minimum required parameters
 * (block_heat_capacity, sensor_responsiveness, ambient_transfer) have been parsed,
 * with a short accumulation window to capture fan_ambient_transfer if present.
 */
class MPCCalibrateCollector : public std::enable_shared_from_this<MPCCalibrateCollector> {
  public:
    using MPCCallback = MoonrakerAdvancedAPI::MPCCalibrateCallback;
    using MPCProgressCB = MoonrakerAdvancedAPI::MPCProgressCallback;
    using MPCResult = MoonrakerAdvancedAPI::MPCResult;

    MPCCalibrateCollector(IMoonrakerClient& client, MPCCallback on_success,
                          MoonrakerAdvancedAPI::ErrorCallback on_error,
                          MPCProgressCB on_progress = nullptr, bool expect_fan_data = false)
        : core_(client, "mpc_calibrate_collector_"), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)), on_progress_(std::move(on_progress)) {
        expect_fan_data_ = expect_fan_data;
    }

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        core_.set_idle_fallback(
            [this]() {
                complete_error("MPC calibration result unavailable - the printer finished before "
                               "the result arrived");
            },
            [this](const std::string& why) { complete_error("MPC_CALIBRATE " + why); });
    }

    void unregister() {
        core_.unregister();
    }

    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    void on_gcode_response(const json& msg) {
        if (core_.completed())
            return;
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string())
            return;

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[MPCCalibrateCollector] Received: {}", line);

        // If we're accumulating result lines after "Finished MPC calibration"
        if (accumulating_results_) {
            parse_result_line(line);
            return;
        }

        // Check for "Finished MPC calibration" — begin result accumulation
        if (line.find("Finished MPC calibration") != std::string::npos) {
            spdlog::debug("[MPCCalibrateCollector] Calibration finished, accumulating results");
            accumulating_results_ = true;
            return;
        }

        // Progress: phase 1 — ambient settling
        if (line.find("Waiting for heater to settle") != std::string::npos) {
            report_progress(1, line);
            return;
        }

        // Progress: phase 2 — heatup test
        if (line.find("Performing heatup test") != std::string::npos) {
            report_progress(2, line);
            return;
        }

        // Progress: phase 3 — fan breakpoint measurements
        if (line.find("measuring power usage with") != std::string::npos) {
            report_progress(3, line);
            return;
        }

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("MPC_CALIBRATE") != std::string::npos) {
            complete_error("MPC_CALIBRATE command not recognized. Requires Kalico firmware.");
            return;
        }

        // Broader error detection
        if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos ||
            line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

  private:
    void report_progress(int phase, const std::string& description) {
        // Total phases: 1=settle, 2=heatup, 3=fan measurements
        static constexpr int kTotalPhases = 3;
        spdlog::debug("[MPCCalibrateCollector] Progress: phase={} desc={}", phase, description);
        if (on_progress_)
            on_progress_(phase, kTotalPhases, description);
    }

    void parse_result_line(const std::string& line) {
        // Parse: fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]
        // Must check BEFORE ambient_transfer since both contain "ambient_transfer"
        static const helix::Regex fat_regex(R"(fan_ambient_transfer=([\d., ]+)\s*\[W/K\])");
        // Parse: block_heat_capacity=18.5432 [J/K]
        static const helix::Regex bhc_regex(R"(block_heat_capacity=([\d.]+))");
        // Parse: sensor_responsiveness=0.123456 [K/s/K]
        static const helix::Regex sr_regex(R"(sensor_responsiveness=([\d.]+))");
        // Parse: ambient_transfer=0.078901 [W/K]
        static const helix::Regex at_regex(R"(ambient_transfer=([\d.]+)\s+\[W/K\])");

        helix::RegexMatch match;

        if (helix::regex_search(line, match, fat_regex)) {
            result_.fan_ambient_transfer = match[1].str();
            // Trim trailing whitespace
            auto end = result_.fan_ambient_transfer.find_last_not_of(' ');
            if (end != std::string::npos)
                result_.fan_ambient_transfer.resize(end + 1);
            spdlog::debug("[MPCCalibrateCollector] fan_ambient_transfer={}",
                          result_.fan_ambient_transfer);
            parsed_fan_ambient_ = true;
            // fan_ambient_transfer is always the last result line — complete now
            if (has_required_params())
                complete_success();
            return;
        }

        if (helix::regex_search(line, match, bhc_regex)) {
            const auto bhc = text_io::parse_leading<float>(match[1].str());
            if (!bhc) {
                return;
            }
            result_.block_heat_capacity = *bhc;
            spdlog::debug("[MPCCalibrateCollector] block_heat_capacity={}",
                          result_.block_heat_capacity);
            parsed_bhc_ = true;
            return;
        }

        if (helix::regex_search(line, match, sr_regex)) {
            const auto sr = text_io::parse_leading<float>(match[1].str());
            if (!sr) {
                return;
            }
            result_.sensor_responsiveness = *sr;
            spdlog::debug("[MPCCalibrateCollector] sensor_responsiveness={}",
                          result_.sensor_responsiveness);
            parsed_sr_ = true;
            return;
        }

        if (helix::regex_search(line, match, at_regex)) {
            const auto at = text_io::parse_leading<float>(match[1].str());
            if (!at) {
                return;
            }
            result_.ambient_transfer = *at;
            spdlog::debug("[MPCCalibrateCollector] ambient_transfer={}", result_.ambient_transfer);
            parsed_at_ = true;
            // ambient_transfer is the last required param. Complete now unless we're
            // expecting fan_ambient_transfer data to follow.
            if (has_required_params() && !expect_fan_data_)
                complete_success();
            return;
        }

        // Unrecognized line during accumulation — if we have all required params,
        // complete (fan_ambient_transfer was not sent). Otherwise check for errors.
        if (has_required_params()) {
            complete_success();
            return;
        }

        if (line.find("Error") != std::string::npos || line.rfind("!! ", 0) == 0) {
            complete_error(line);
            return;
        }
    }

    bool has_required_params() const {
        return parsed_bhc_ && parsed_sr_ && parsed_at_;
    }

    void complete_success() {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete())
            return;
        spdlog::info("[MPCCalibrateCollector] MPC result: bhc={:.4f} sr={:.6f} at={:.6f} fat={}",
                     result_.block_heat_capacity, result_.sensor_responsiveness,
                     result_.ambient_transfer, result_.fan_ambient_transfer);
        unregister();
        if (on_success_)
            on_success_(result_);
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete())
            return;
        spdlog::error("[MPCCalibrateCollector] Error: {}", message);
        unregister();
        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("MPC_CALIBRATE", message);
            on_error_(err);
        }
    }

    CalibrationCollectorCore core_;
    MPCCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    MPCProgressCB on_progress_;

    // Result accumulation state
    bool expect_fan_data_ = false;
    bool accumulating_results_ = false;
    MPCResult result_;
    bool parsed_bhc_ = false;
    bool parsed_sr_ = false;
    bool parsed_at_ = false;
    bool parsed_fan_ambient_ = false;
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for collecting SCREWS_TILT_CALCULATE responses
 *
 * Klipper sends screw tilt results as console output lines via notify_gcode_response.
 * This class collects and parses those lines until the sequence completes.
 *
 * Expected output format:
 *   // front_left (base) : x=-5.0, y=30.0, z=2.48750
 *   // front_right : x=155.0, y=30.0, z=2.36000 : adjust CW 01:15
 *   // rear_right : x=155.0, y=180.0, z=2.42500 : adjust CCW 00:30
 *   // rear_left : x=155.0, y=180.0, z=2.42500 : adjust CW 00:18
 *
 * Error handling:
 *   - "Unknown command" - screws_tilt_adjust not configured
 *   - "!! " prefix - Klipper emergency/critical errors
 *
 * Completion is signaled by the execute_gcode success callback (JSON-RPC response),
 * NOT by an "ok" line in notify_gcode_response. Klipper may send intermediate "ok"
 * lines (e.g., from sub-commands during probing) before the actual screw results
 * arrive via "//" prefixed lines. The execute_gcode call to printer.gcode.script
 * only returns after the entire command finishes, so its success callback is the
 * reliable completion signal.
 */
class ScrewsTiltCollector : public std::enable_shared_from_this<ScrewsTiltCollector> {
  public:
    /// @param command The macro actually being run. The ScrewsTilt slot makes this
    ///        configurable (BED_LEVEL_SCREWS_TUNE wraps SCREWS_TILT_CALCULATE), so
    ///        every place that names the command must use the RESOLVED name -
    ///        otherwise the unknown-command diagnostic and the error context both
    ///        go stale and start naming a macro the user never ran.
    ScrewsTiltCollector(IMoonrakerClient& client, ScrewTiltCallback on_success,
                        MoonrakerAdvancedAPI::ErrorCallback on_error, std::string command)
        : core_(client, "screws_tilt_collector_"), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)), command_(std::move(command)) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        core_.set_idle_fallback(
            [this]() { on_command_finished(); },
            [this](const std::string& why) { complete_error(command_ + " " + why); },
            /*edge_immediate=*/true);
    }

    void unregister() {
        core_.unregister();
    }

    /**
     * @brief Mark as completed without invoking callbacks
     *
     * Used when the execute_gcode error path handles the error callback directly.
     */
    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    /**
     * @brief Signal that execute_gcode completed successfully (JSON-RPC returned)
     *
     * This is the reliable completion signal. By the time the JSON-RPC call for
     * printer.gcode.script returns, Klipper has finished executing the command
     * and all notify_gcode_response lines (including screw results) have been sent.
     */
    void on_command_finished() {
        if (core_.completed()) {
            return;
        }

        if (!results_.empty()) {
            spdlog::info("[ScrewsTiltCollector] Command finished, {} results collected",
                         results_.size());
            complete_success();
        } else {
            spdlog::warn("[ScrewsTiltCollector] Command finished but no screw data received");
            complete_error(command_ + " completed but no screw data received");
        }
    }

    void on_gcode_response(const json& msg) {
        // Check if already completed (prevent double-invocation)
        if (core_.completed()) {
            return;
        }

        // notify_gcode_response format: {"method": "notify_gcode_response", "params": ["line"]}
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[ScrewsTiltCollector] Received: {}", line);

        // Check for unknown command error (screws_tilt_adjust not configured)
        if (line.find("Unknown command") != std::string::npos &&
            line.find(command_) != std::string::npos) {
            complete_error(command_ + " requires [screws_tilt_adjust] in printer.cfg");
            return;
        }

        // Parse screw result lines that start with "//"
        if (line.rfind("//", 0) == 0) {
            parse_screw_line(line);
        }

        // Klipper emergency/critical errors start with "!! "
        if (line.rfind("!! ", 0) == 0) {
            complete_error(line);
        }
    }

  private:
    void parse_screw_line(const std::string& line) {
        // Shared with the mock printer and the unit tests — see
        // screws_tilt_parser.h. Keep the parsing itself out of this collector so
        // there is exactly one implementation of Klipper's sign convention.
        ScrewTiltResult result;
        if (parse_screws_tilt_line(line, result)) {
            results_.push_back(std::move(result));
        }
    }

    void complete_success() {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::info("[ScrewsTiltCollector] Complete with {} screws", results_.size());
        unregister();

        if (on_success_) {
            on_success_(results_);
        }
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::error("[ScrewsTiltCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error(command_, message);
            on_error_(err);
        }
    }

    CalibrationCollectorCore core_;
    ScrewTiltCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::string command_; ///< Resolved macro name, NOT a literal
    std::vector<ScrewTiltResult> results_;
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for collecting SHAPER_CALIBRATE responses
 *
 * Klipper sends input shaper results as console output lines via notify_gcode_response.
 * This class collects and parses those lines until the sequence completes.
 *
 * Expected output format (verbatim from a Klipper/Kalico run; the sweep ends at
 * [resonance_tester] max_freq, which defaults to 133-135 Hz, NOT 100 Hz):
 *   Testing frequency 5 Hz
 *   ...
 *   Testing frequency 134 Hz
 *   Calculating the best input shaper parameters for x axis
 *   Fitted shaper 'zv' frequency = 35.8 Hz (vibrations = 22.7%, smoothing ~= 0.100)
 *   To avoid too much smoothing with 'zv', suggested max_accel <= 4000 mm/sec^2
 *   Fitted shaper 'mzv' frequency = 36.7 Hz (vibrations = 7.2%, smoothing ~= 0.140)
 *   To avoid too much smoothing with 'mzv', suggested max_accel <= 5400 mm/sec^2
 *   ...
 *   Recommended shaper_type_x = mzv, shaper_freq_x = 36.7 Hz
 *   Shaper calibration data written to /tmp/calibration_data_x_*.csv file
 */
class InputShaperCollector : public std::enable_shared_from_this<InputShaperCollector> {
  public:
    /// Fallback sweep bounds, used until the printer's own [resonance_tester]
    /// range answers. Klipper defaults to 133.33 Hz and Kalico to 135 Hz; the
    /// higher value is the safer guess because underestimating the ceiling
    /// pins the sweep bar at its maximum for the remainder of the test.
    static constexpr float kDefaultMinFreq = 5.0f;
    static constexpr float kDefaultMaxFreq = 135.0f;

    /// Sweep progress spans the whole 0..100 bar. The analysis phase that
    /// follows has no percent at all - the UI swaps the bar for an indeterminate
    /// spinner plus an elapsed-seconds label, so the phase alone carries the
    /// handover. A sweep that runs past its expected ceiling clamps at 100
    /// while the phase stays Sweeping, so it can never masquerade as analysis.

    /// How close to max_freq a sweep line must land to count as the last one.
    /// Klipper stops one step short of the configured ceiling (a 135 Hz
    /// max_freq ends at 134 Hz), so an equality test would never fire.
    static constexpr float kSweepEndToleranceHz = 1.5f;

    /// Gaps in Klipper's output that are worth a log line. Measured on a CB1
    /// running a 5-135 Hz sweep: lines arrive ~1/sec while sweeping, and the
    /// longest legitimate silence is the FFT between the final sweep line and
    /// the "Calculating the best" marker, at ~11s. Analysis time scales with
    /// host CPU, so its threshold is deliberately loose — a memory-constrained
    /// board can take far longer than a Pi. These only log; nothing aborts on
    /// them, so erring generous costs nothing.
    static constexpr int64_t kSweepStallWarnMs = 30000;
    static constexpr int64_t kAnalyzeStallWarnMs = 120000;

    InputShaperCollector(IMoonrakerClient& client, char axis, ShaperProgressCallback on_progress,
                         InputShaperCallback on_success,
                         MoonrakerAdvancedAPI::ErrorCallback on_error)
        : core_(client, "input_shaper_collector_"), axis_(axis),
          on_progress_(std::move(on_progress)), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)), last_activity_ms_(steady_now_ms()) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        spdlog::debug("[InputShaperCollector] Started collecting responses for axis {}", axis_);
        core_.set_idle_fallback(
            [this]() {
                complete_error("Resonance test result unavailable - the printer finished before "
                               "the result arrived");
            },
            [this](const std::string& why) { complete_error("SHAPER_CALIBRATE " + why); });
    }

    void unregister() {
        core_.unregister();
    }

    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    /**
     * @brief Adopt this printer's actual [resonance_tester] sweep bounds
     *
     * Called from the configfile query issued alongside SHAPER_CALIBRATE. Safe
     * to arrive after the sweep has begun — it only rescales later progress
     * reports. Implausible ranges are ignored so a malformed config cannot make
     * progress divide by zero or run backwards.
     */
    void set_sweep_range(float min_freq, float max_freq) {
        if (!(max_freq > min_freq) || min_freq < 0.0f) {
            spdlog::warn("[InputShaperCollector] Ignoring implausible sweep range {}-{} Hz",
                         min_freq, max_freq);
            return;
        }
        min_freq_.store(min_freq);
        max_freq_.store(max_freq);
        range_from_config_.store(true);
        spdlog::debug("[InputShaperCollector] Sweep range set to {:.1f}-{:.1f} Hz", min_freq,
                      max_freq);
    }

    /**
     * @brief Log where the run had got to, for a stall we cannot explain
     *
     * Called from the SHAPER_CALIBRATE timeout path. Klipper gives no "I am
     * stuck" line, so the only evidence a stalled calibration leaves is the
     * shape of its silence: which phase we were in, the last frequency the
     * sweep reached, and how long ago the last line arrived.
     */
    void log_stall_diagnostics(const char* reason) const {
        spdlog::warn("[InputShaperCollector] {} on axis {} — phase={}, last sweep freq={:.1f} Hz "
                     "of {:.1f} Hz, {} shapers fitted, {:.1f}s since last response",
                     reason, axis_, phase_name(collector_state_), last_sweep_freq_.load(),
                     max_freq_.load(), shaper_fits_.size(),
                     static_cast<double>(ms_since_last_activity()) / 1000.0);
    }

    void on_gcode_response(const json& msg) {
        // Ordering assumption: everything this collector needs (fits,
        // recommendation, the copy_TestAxis_y_to_x marker, and the CSV-write
        // line that completes the run) arrives BEFORE the CSV line. The
        // captured K1C transcript (2026-08-19) shows the marker preceding the
        // "calibration data written to" line, so completing on the CSV line
        // cannot race the marker away. A fork emitting the marker after the
        // CSV line would lose it here.
        if (core_.completed()) {
            return;
        }

        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[InputShaperCollector] Received: {}", line);

        // Activity watchdog. There is no line to match on for "the calibration
        // wedged" — the only evidence is silence — so report the gap we just
        // came out of. A gap noticed here is retrospective and harmless; a run
        // that goes quiet permanently is reported instead from the timeout path
        // via log_stall_diagnostics().
        note_activity();

        // Check for unknown command error
        if (line.find("Unknown command") != std::string::npos &&
            line.find("SHAPER_CALIBRATE") != std::string::npos) {
            complete_error(
                "SHAPER_CALIBRATE requires [resonance_tester] and ADXL345 in printer.cfg");
            return;
        }

        // Parse frequency sweep lines: "Testing frequency 62.00 Hz"
        if (line.find("Testing frequency") != std::string::npos) {
            parse_sweep_line(line);
            return;
        }

        // End of sweep, start of the offline fit. Klipper and Kalico both emit
        // "Calculating the best input shaper parameters for <axis> axis";
        // "Wait for calculations.." is the older wording some forks still use -
        // and some (e.g. Creality's K1C build) repeat it every few seconds as
        // a heartbeat through the whole analysis. Only the first occurrence
        // reports; repeats fall through enter_analyzing()'s idempotence guard
        // and do nothing but stamp the activity watchdog above.
        if (line.find("Calculating the best") != std::string::npos ||
            line.find("Wait for calculations") != std::string::npos) {
            enter_analyzing();
            return;
        }

        // Parse shaper/smoother fit lines (Kalico uses "Fitted smoother" for smooth shapers)
        if (line.find("Fitted shaper") != std::string::npos ||
            line.find("Fitted smoother") != std::string::npos) {
            parse_shaper_line(line);
            return;
        }

        // Parse max_accel lines: "suggested max_accel <= 4000 mm/sec^2"
        if (line.find("suggested max_accel") != std::string::npos) {
            parse_max_accel_line(line);
            return;
        }

        // Firmware copy-marker: at the end of a Y-axis run some klippy forks
        // (Creality's K1C build) overwrite the staged X result with Y's values,
        // discarding the measured X recommendation, and announce it with a line
        // starting "copy_TestAxis_y_to_x Recommended shaper_type_x = ...".
        // Must be matched BEFORE the recommendation parser: the marker embeds
        // a real "Recommended shaper_type_x" wording that would otherwise be
        // parsed as this axis's recommendation and clobber it.
        if (line.find("copy_TestAxis_y_to_x") != std::string::npos) {
            x_overwritten_by_firmware_ = true;
            spdlog::warn("[InputShaperCollector] Firmware overwrote the staged X result with Y's "
                         "values ({} axis run)",
                         axis_);
            return;
        }

        // Parse recommendation line (try new format first, then old)
        // Don't complete yet — CSV path line follows immediately after
        if (line.find("Recommended shaper") != std::string::npos ||
            line.find("Recommended smoother") != std::string::npos) {
            parse_recommendation(line);
            collector_state_ = CollectorState::COMPLETE;
            return;
        }

        // Parse CSV path: "calibration data written to /tmp/calibration_data_x_*.csv"
        if (line.find("calibration data written to") != std::string::npos) {
            parse_csv_path(line);
            complete_success();
            return;
        }

        // If we have the recommendation, keep waiting for the CSV path line.
        // Don't complete early on unrelated G-code responses (e.g., temperature
        // reports) — that would discard the CSV data needed for frequency charts.
        if (collector_state_ == CollectorState::COMPLETE) {
            return;
        }

        // Error detection
        if (line.rfind("!! ", 0) == 0 || line.rfind("Error: ", 0) == 0 ||
            line.find("error:") != std::string::npos) {
            complete_error(line);
        }
    }

  private:
    enum class CollectorState { WAITING_FOR_OUTPUT, SWEEPING, CALCULATING, COMPLETE };

    static const char* phase_name(CollectorState s) {
        switch (s) {
        case CollectorState::WAITING_FOR_OUTPUT:
            return "waiting";
        case CollectorState::SWEEPING:
            return "sweeping";
        case CollectorState::CALCULATING:
            return "analyzing";
        case CollectorState::COMPLETE:
            return "complete";
        }
        return "unknown";
    }

    static int64_t steady_now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] int64_t ms_since_last_activity() const {
        return steady_now_ms() - last_activity_ms_.load();
    }

    /// Stamp this response and, if the preceding silence was long enough to be
    /// worth knowing about, say so. Thresholds differ by phase because the
    /// sweep emits roughly one line per second while the analysis is a single
    /// host-CPU-bound computation with no output at all.
    void note_activity() {
        const int64_t gap = ms_since_last_activity();
        const int64_t threshold = (collector_state_ == CollectorState::CALCULATING)
                                      ? kAnalyzeStallWarnMs
                                      : kSweepStallWarnMs;
        if (gap > threshold) {
            spdlog::warn("[InputShaperCollector] {:.1f}s gap in SHAPER_CALIBRATE output on axis {} "
                         "(phase={}, last sweep freq={:.1f} Hz)",
                         static_cast<double>(gap) / 1000.0, axis_, phase_name(collector_state_),
                         last_sweep_freq_.load());
        }
        last_activity_ms_.store(steady_now_ms());
    }

    void enter_analyzing() {
        if (collector_state_ == CollectorState::CALCULATING ||
            collector_state_ == CollectorState::COMPLETE) {
            return;
        }
        collector_state_ = CollectorState::CALCULATING;
        // The percent is meaningless in this phase (the UI shows a spinner plus
        // elapsed time); the report exists to carry the phase change itself.
        emit_progress(0, ShaperCalibrationPhase::Analyzing, 0.0f, "Calculating results...");
    }

    void parse_sweep_line(const std::string& line) {
        const auto parsed_freq = calibration::parse_testing_frequency(line);
        if (parsed_freq) {
            float freq = *parsed_freq;
            last_sweep_freq_.store(freq);

            // A sweep line arriving after the analysis phase began means the
            // range we were given is not the range this run used. Report it
            // — no string identifies this, only the ordering does — but do
            // not drag the phase backwards: a label flickering between
            // "measuring" and "analyzing" is worse than one that is early.
            if (collector_state_ == CollectorState::CALCULATING ||
                collector_state_ == CollectorState::COMPLETE) {
                spdlog::warn("[InputShaperCollector] Sweep line at {:.1f} Hz arrived after the "
                             "sweep was believed finished (ceiling {:.1f} Hz) — the reported "
                             "[resonance_tester] range does not match this run",
                             freq, max_freq_.load());
                return;
            }

            collector_state_ = CollectorState::SWEEPING;

            // Progress: 0..100 mapped across min_freq..max_freq.
            // A sweep that runs past the expected ceiling (configfile query
            // missed, or TEST_RESONANCES was given explicit bounds) sits at
            // 100 until it ends. That is still honest — the
            // phase stays Sweeping, so the UI keeps saying "measuring"
            // rather than claiming the analysis has started.
            calibration::ResonanceTesterConfig range;
            range.min_freq = min_freq_.load();
            range.max_freq = max_freq_.load();
            const int percent = calibration::sweep_percent(freq, range);

            char status[64];
            snprintf(status, sizeof(status), "Testing frequency %.0f Hz", freq);
            emit_progress(percent, ShaperCalibrationPhase::Sweeping, freq, status);

            // Structural end-of-sweep: reaching the configured ceiling means
            // the toolhead is done regardless of what the firmware prints
            // next, so a fork that reworded both the marker line and its
            // "Fitted shaper" lines still leaves the Sweeping phase.
            //
            // Gated on range_from_config_ on purpose. Against a defaulted
            // ceiling this check would re-create the very bug it backs up —
            // a printer sweeping past our guess would trip it mid-sweep and
            // claim to be analyzing while still moving.
            if (range_from_config_.load() && freq >= max_freq_.load() - kSweepEndToleranceHz) {
                enter_analyzing();
            }
        }
    }

    void parse_shaper_line(const std::string& line) {
        // Kalico bleeding-edge format (both smoothers and discrete shapers):
        // Fitted smoother 'smooth_mzv' frequency = 42.6 Hz (vibration score = 1.23%, smoothing ~=
        // 0.085, combined score = 1.234e-02) Fitted shaper 'mzv' frequency = 36.7 Hz (vibration
        // score = 1.23%, smoothing ~= 0.140, combined score = 2.345e-02)
        static const helix::Regex kalico_regex(
            R"(Fitted (?:shaper|smoother) '([\w]+)' frequency = ([\d.]+) Hz \(vibration score = ([\d.]+)%, smoothing ~= ([\d.]+))");

        // Standard Klipper format:
        // Fitted shaper 'mzv' frequency = 36.7 Hz (vibrations = 7.2%, smoothing ~= 0.140)
        static const helix::Regex klipper_regex(
            R"(Fitted shaper '(\w+)' frequency = ([\d.]+) Hz \(vibrations = ([\d.]+)%, smoothing ~= ([\d.]+)\))");

        helix::RegexMatch match;
        bool matched = helix::regex_search(line, match, kalico_regex) ||
                       helix::regex_search(line, match, klipper_regex);

        if (matched && match.size() >= 5) {
            ShaperFitData fit;
            fit.type = match[1].str();
            const auto frequency = text_io::parse_leading<float>(match[2].str());
            const auto vibrations = text_io::parse_leading<float>(match[3].str());
            const auto smoothing = text_io::parse_leading<float>(match[4].str());
            if (!frequency || !vibrations || !smoothing) {
                spdlog::warn("[InputShaperCollector] Failed to parse shaper fit values");
                return;
            }
            fit.frequency = *frequency;
            fit.vibrations = *vibrations;
            fit.smoothing = *smoothing;

            spdlog::debug("[InputShaperCollector] Parsed: {} @ {:.1f} Hz (vib: {:.1f}%)", fit.type,
                          fit.frequency, fit.vibrations);
            shaper_fits_.push_back(fit);

            // A "Fitted shaper" line means the sweep is over even if the
            // phase marker line was absent or reworded by a fork. When this is
            // the first analysis signal, enter_analyzing() emits the phase
            // change; when a marker line got there first, it no-ops and the
            // fit only accumulates data - the analysis phase reports no
            // percent, so there is nothing to emit per fit.
            enter_analyzing();
        }
    }

    void parse_max_accel_line(const std::string& line) {
        static const helix::Regex accel_regex(R"(suggested max_accel <= (\d+))");
        helix::RegexMatch match;
        if (helix::regex_search(line, match, accel_regex) && match.size() == 2) {
            const auto max_accel = text_io::parse_leading<float>(match[1].str());
            if (!max_accel) {
                return;
            }
            // Attach to the most recently parsed shaper fit
            if (!shaper_fits_.empty()) {
                shaper_fits_.back().max_accel = *max_accel;
                spdlog::debug("[InputShaperCollector] {} max_accel: {:.0f}",
                              shaper_fits_.back().type, *max_accel);
            }
        }
    }

    void parse_recommendation(const std::string& line) {
        // Try new Klipper format first: "Recommended shaper_type_x = mzv, shaper_freq_x = 53.8 Hz"
        static const helix::Regex rec_new(
            R"(Recommended shaper_type_\w+ = (\w+), shaper_freq_\w+ = ([\d.]+) Hz)");
        // Kalico smoother format: "Recommended smoother_type_x = smooth_mzv, smoother_freq_x = 42.6
        // Hz"
        static const helix::Regex rec_smoother(
            R"(Recommended smoother_type_\w+ = (\w+), smoother_freq_\w+ = ([\d.]+) Hz)");
        // Legacy format: "Recommended shaper is mzv @ 36.7 Hz"
        static const helix::Regex rec_old(R"(Recommended shaper is (\w+) @ ([\d.]+) Hz)");

        helix::RegexMatch match;
        bool matched = helix::regex_search(line, match, rec_new) ||
                       helix::regex_search(line, match, rec_smoother) ||
                       helix::regex_search(line, match, rec_old);

        if (matched && match.size() == 3) {
            recommended_type_ = match[1].str();
            recommended_freq_ = text_io::parse_leading<float>(match[2].str()).value_or(0.0f);
            spdlog::info("[InputShaperCollector] Recommendation: {} @ {:.1f} Hz", recommended_type_,
                         recommended_freq_);
        }
    }

    void parse_csv_path(const std::string& line) {
        if (const auto path = calibration::parse_written_csv_path(line)) {
            csv_path_ = *path;
            spdlog::info("[InputShaperCollector] CSV path: {}", csv_path_);
        }
    }

    void emit_progress(int percent, ShaperCalibrationPhase phase, float freq_hz,
                       const std::string& status) {
        if (on_progress_) {
            on_progress_(percent, phase, freq_hz);
        }
        spdlog::trace("[InputShaperCollector] Progress: {}% - {}", percent, status);
    }

    void complete_success() {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::info("[InputShaperCollector] Complete with {} shaper options", shaper_fits_.size());
        unregister();

        // Emit 100% progress
        emit_progress(100, ShaperCalibrationPhase::Complete, 0.0f, "Complete");

        if (on_success_) {
            InputShaperResult result;
            result.axis = axis_;
            result.shaper_type = recommended_type_;
            result.shaper_freq = recommended_freq_;
            result.csv_path = csv_path_;
            result.x_overwritten_by_firmware = x_overwritten_by_firmware_;

            // Find recommended shaper's details and populate all_shapers
            for (const auto& fit : shaper_fits_) {
                if (fit.type == recommended_type_) {
                    result.smoothing = fit.smoothing;
                    result.vibrations = fit.vibrations;
                    result.max_accel = fit.max_accel;
                }

                ShaperOption option;
                option.type = fit.type;
                option.frequency = fit.frequency;
                option.vibrations = fit.vibrations;
                option.smoothing = fit.smoothing;
                option.max_accel = fit.max_accel;
                result.all_shapers.push_back(option);
            }

            // Parse frequency response data from calibration CSV
            if (!result.csv_path.empty()) {
                auto csv_data = helix::calibration::parse_shaper_csv(result.csv_path, axis_);
                if (!csv_data.frequencies.empty()) {
                    result.freq_response.reserve(csv_data.frequencies.size());
                    for (size_t i = 0; i < csv_data.frequencies.size(); ++i) {
                        result.freq_response.emplace_back(
                            csv_data.frequencies[i],
                            i < csv_data.raw_psd.size() ? csv_data.raw_psd[i] : 0.0f);
                    }
                    result.shaper_curves = std::move(csv_data.shaper_curves);
                    spdlog::debug(
                        "[InputShaperCollector] parsed {} freq bins, {} shaper curves from CSV",
                        result.freq_response.size(), result.shaper_curves.size());
                } else {
                    // Klipper reported a CSV path but we couldn't read any data
                    // from it (missing/unreadable file or malformed CSV). Flag it
                    // so the UI can tell the user the chart is unavailable instead
                    // of silently showing a blank graph.
                    result.chart_data_unavailable = true;
                    spdlog::warn("[InputShaperCollector] CSV reported at {} but no frequency data "
                                 "could be read — chart unavailable",
                                 result.csv_path);
                }
            }

            on_success_(result);
        }
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::error("[InputShaperCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("SHAPER_CALIBRATE", message);
            on_error_(err);
        }
    }

    // Internal struct for collecting fits before building final result
    struct ShaperFitData {
        std::string type;
        float frequency = 0.0f;
        float vibrations = 0.0f;
        float smoothing = 0.0f;
        float max_accel = 0.0f;
    };

    CalibrationCollectorCore core_;
    char axis_;
    ShaperProgressCallback on_progress_;
    InputShaperCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;

    CollectorState collector_state_ = CollectorState::WAITING_FOR_OUTPUT;
    // Atomic because the configfile query answers on the RPC path while the
    // sweep lines arrive on the notification path.
    std::atomic<float> min_freq_{kDefaultMinFreq};
    std::atomic<float> max_freq_{kDefaultMaxFreq};
    /// True once the printer's own [resonance_tester] range has been read.
    /// The sweep-end fallback is only trustworthy when this is set.
    std::atomic<bool> range_from_config_{false};
    std::atomic<float> last_sweep_freq_{0.0f};
    std::string csv_path_;
    /// steady_clock milliseconds of the last gcode response. Atomic because the
    /// timeout path reads it from a different callback than the one writing it.
    std::atomic<int64_t> last_activity_ms_;

    std::vector<ShaperFitData> shaper_fits_;
    std::string recommended_type_;
    float recommended_freq_ = 0.0f;
    /// Set by the copy_TestAxis_y_to_x marker line (see on_gcode_response).
    bool x_overwritten_by_firmware_ = false;
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for one TEST_RESONANCES OUTPUT=resonances run
 *
 * Follows Klipper's console through a belt-path sweep: "Testing frequency"
 * lines carry progress, and the terminal "Resonances data written to <path>
 * file" line names the CSV this collector reads the curve from. The file is
 * only ever read from a path announced to THIS collector, so a stale file
 * left by an earlier run cannot be mistaken for this run's result.
 *
 * There is no overall deadline: a sweep takes minutes, and the caller owns
 * stall detection and cancels through the returned handle.
 */
class BeltResonanceCollector : public std::enable_shared_from_this<BeltResonanceCollector> {
  public:
    BeltResonanceCollector(IMoonrakerClient& client,
                           MoonrakerAdvancedAPI::BeltSweepProgressCallback on_progress,
                           MoonrakerAdvancedAPI::BeltResponseCallback on_success,
                           MoonrakerAdvancedAPI::ErrorCallback on_error,
                           std::optional<calibration::BeltSweepWindow> window = std::nullopt)
        : core_(client, "belt_resonance_collector_"), on_progress_(std::move(on_progress)),
          on_success_(std::move(on_success)), on_error_(std::move(on_error)), window_(window) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        spdlog::debug("[BeltResonanceCollector] Started collecting responses");
    }

    /// Adopt this printer's [resonance_tester] range, from the configfile query
    /// issued alongside the sweep. Progress is held until this has run: the
    /// query reply can land after the first sweep lines, and a percent computed
    /// against the default range would misreport the sweep's start.
    void set_config(const calibration::ResonanceTesterConfig& cfg) {
        cfg_ = cfg;
        if (window_) {
            cfg_.min_freq = window_->freq_start_hz;
            cfg_.max_freq = window_->freq_end_hz;
        }
        config_received_.store(true);
    }

    /// Stop listening and suppress every later callback. Idempotent.
    void cancel() {
        core_.mark_completed();
        core_.unregister();
    }

    /// Terminal path for RPC errors the transport did not cause.
    void complete_error(const std::string& message) {
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }
        spdlog::error("[BeltResonanceCollector] Error: {}", message);
        core_.unregister();
        if (on_error_) {
            on_error_(MoonrakerError::json_rpc_error("TEST_RESONANCES", message));
        }
    }

    void on_gcode_response(const json& msg) {
        if (core_.completed()) {
            return;
        }
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[BeltResonanceCollector] Received: {}", line);

        if (line.rfind("!! ", 0) == 0 || line.rfind("Error: ", 0) == 0) {
            complete_error(line);
            return;
        }

        if (line.find("Unknown command") != std::string::npos &&
            line.find("TEST_RESONANCES") != std::string::npos) {
            complete_error("TEST_RESONANCES requires [resonance_tester] and an accelerometer in "
                           "printer.cfg");
            return;
        }

        if (const auto freq = calibration::parse_testing_frequency(line)) {
            // Lines before the config reply are dropped rather than reported
            // against a guessed range; set_config() always runs, so the hold
            // is finite.
            if (config_received_.load() && on_progress_) {
                on_progress_(calibration::sweep_percent(*freq, cfg_), *freq);
            }
            return;
        }

        if (const auto path = calibration::parse_written_csv_path(line)) {
            read_result(*path);
        }
    }

  private:
    /// Read the file this run announced and complete the run on what it holds.
    void read_result(const std::string& path) {
        const calibration::ResonanceCsvData data = calibration::parse_resonance_csv(path);
        switch (data.error) {
        case calibration::ResonanceCsvError::NONE:
            complete_success(calibration::BeltResponse{data.curve, data.in_plane});
            return;
        case calibration::ResonanceCsvError::MISSING:
            complete_error(fmt::format(
                "Klipper reported {} but it cannot be read here. HelixScreen must run on the "
                "printer's own computer.",
                path));
            return;
        case calibration::ResonanceCsvError::MULTI_CHIP:
            complete_error("More than one accelerometer reported. Belt Tension supports one.");
            return;
        case calibration::ResonanceCsvError::EMPTY:
        case calibration::ResonanceCsvError::NO_PSD_COLUMN:
            complete_error(fmt::format("Klipper's results file {} has no usable data.", path));
            return;
        }
    }

    void complete_success(const calibration::BeltResponse& response) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner. Pin the object for the rest of the
        // callback chain (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }
        spdlog::info("[BeltResonanceCollector] Complete with {} curve points ({} in-plane)",
                     response.xyz.size(), response.in_plane.size());
        core_.unregister();
        if (on_success_) {
            on_success_(response);
        }
    }

    CalibrationCollectorCore core_;
    MoonrakerAdvancedAPI::BeltSweepProgressCallback on_progress_;
    MoonrakerAdvancedAPI::BeltResponseCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    /// Progress scales against this instead of the printer's range when set.
    std::optional<calibration::BeltSweepWindow> window_;

    /// Written once by set_config() on the RPC path, read on the notification
    /// path; the seq-cst flag below orders the two.
    calibration::ResonanceTesterConfig cfg_{};
    std::atomic<bool> config_received_{false};
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for collecting MEASURE_AXES_NOISE responses
 *
 * Klipper sends noise measurement results as console output lines via notify_gcode_response.
 * This class collects and parses those lines to extract the noise level.
 *
 * Expected output format:
 *   "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396 (z)"
 *
 * Error handling:
 *   - "Unknown command" - MEASURE_AXES_NOISE not available (no accelerometer)
 *   - "Error"/"error"/"!! " - Klipper error messages
 */
class NoiseCheckCollector : public std::enable_shared_from_this<NoiseCheckCollector> {
  public:
    NoiseCheckCollector(IMoonrakerClient& client,
                        MoonrakerAdvancedAPI::NoiseCheckCallback on_success,
                        MoonrakerAdvancedAPI::ErrorCallback on_error)
        : core_(client, "noise_check_collector_"), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        spdlog::debug("[NoiseCheckCollector] Started collecting responses");
        core_.set_idle_fallback(
            [this]() {
                complete_error("Noise measurement result unavailable - the printer finished "
                               "before the result arrived");
            },
            [this](const std::string& why) { complete_error("MEASURE_AXES_NOISE " + why); });
    }

    void unregister() {
        core_.unregister();
    }

    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    void on_gcode_response(const json& msg) {
        if (core_.completed()) {
            return;
        }

        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[NoiseCheckCollector] Received: {}", line);

        // Check for unknown command error (no accelerometer configured)
        if (line.find("Unknown command") != std::string::npos &&
            line.find("MEASURE_AXES_NOISE") != std::string::npos) {
            complete_error("MEASURE_AXES_NOISE requires [adxl345] accelerometer in printer.cfg");
            return;
        }

        // Parse noise level line: "Axes noise for xy-axis accelerometer: 57.956 (x), ..."
        if (line.find("Axes noise") != std::string::npos) {
            parse_noise_line(line);
            return;
        }

        // Error detection
        if (line.rfind("!! ", 0) == 0 ||                // Emergency errors
            line.rfind("Error:", 0) == 0 ||             // Standard errors
            line.find("error:") != std::string::npos) { // Python traceback
            complete_error(line);
        }
    }

  private:
    void parse_noise_line(const std::string& line) {
        // Klipper output format:
        // "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396 (z)"
        static const helix::Regex noise_regex(
            R"(Axes noise.*:\s*([\d.]+)\s*\(x\),\s*([\d.]+)\s*\(y\),\s*([\d.]+)\s*\(z\))");

        helix::RegexMatch match;
        if (helix::regex_search(line, match, noise_regex) && match.size() == 4) {
            const auto noise_x = text_io::parse_leading<float>(match[1].str());
            const auto noise_y = text_io::parse_leading<float>(match[2].str());
            const auto noise_z = text_io::parse_leading<float>(match[3].str());
            if (!noise_x || !noise_y || !noise_z) {
                spdlog::warn("[NoiseCheckCollector] Failed to parse noise value");
                complete_error("Failed to parse noise measurement");
                return;
            }

            spdlog::info("[NoiseCheckCollector] Noise: x={:.2f}, y={:.2f}, z={:.2f}", *noise_x,
                         *noise_y, *noise_z);

            // Zero reading on X or Y means accelerometer isn't working on that axis
            constexpr float kMinNoise = 0.001f;
            if (*noise_x < kMinNoise || *noise_y < kMinNoise) {
                std::string dead_axes;
                if (*noise_x < kMinNoise)
                    dead_axes += "X";
                if (*noise_y < kMinNoise) {
                    if (!dead_axes.empty())
                        dead_axes += " and ";
                    dead_axes += "Y";
                }
                complete_error("Accelerometer reading zero on " + dead_axes +
                               " axis — check wiring and axes_map configuration");
                return;
            }

            // Report max of x,y as the overall noise level
            float noise = std::max(*noise_x, *noise_y);
            complete_success(noise);
        }
    }

    void complete_success(float noise_level) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::info("[NoiseCheckCollector] Complete with noise level: {:.6f}", noise_level);
        unregister();

        if (on_success_) {
            on_success_(noise_level);
        }
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::error("[NoiseCheckCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("MEASURE_AXES_NOISE", message);
            on_error_(err);
        }
    }

    CalibrationCollectorCore core_;
    MoonrakerAdvancedAPI::NoiseCheckCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
};
} // namespace helix

namespace helix {
/**
 * @brief State machine for collecting BED_MESH_CALIBRATE progress
 *
 * Klipper sends probing progress as console output lines via notify_gcode_response.
 * This class collects and parses those lines to provide real-time progress updates.
 *
 * Expected output formats:
 *   Probing point 5/25
 *   Probe point 5 of 25
 *
 * Completion markers:
 *   "Mesh Bed Leveling Complete"
 *   "Mesh bed leveling complete"
 *
 * Error handling:
 *   - "!! " prefix - Klipper emergency/critical errors
 *   - "Error:" prefix - Standard Klipper errors
 *   - "error:" in line - Python traceback errors
 */
class BedMeshProgressCollector : public std::enable_shared_from_this<BedMeshProgressCollector> {
  public:
    using ProgressCallback = std::function<void(int current, int total)>;

    BedMeshProgressCollector(IMoonrakerClient& client, ProgressCallback on_progress,
                             MoonrakerAdvancedAPI::SuccessCallback on_complete,
                             MoonrakerAdvancedAPI::ErrorCallback on_error, int expected_probes = 0,
                             int probe_samples = 1, bool unknown_command_fails = false)
        : core_(client, "bed_mesh_collector_"), on_progress_(std::move(on_progress)),
          on_complete_(std::move(on_complete)), on_error_(std::move(on_error)),
          expected_probes_(expected_probes), unknown_command_fails_(unknown_command_fails),
          point_counter_(probe_samples) {}

    void start() {
        auto self = shared_from_this();
        core_.start(self, [self](const json& msg) { self->on_gcode_response(msg); });
        spdlog::debug("[BedMeshProgressCollector] Started collecting responses");
        core_.set_idle_fallback(
            [this]() { on_command_finished(); },
            [this](const std::string& why) { complete_error("BED_MESH_CALIBRATE " + why); },
            /*edge_immediate=*/true);
    }

    void unregister() {
        core_.unregister();
    }

    void mark_completed() {
        core_.mark_completed();
    }
    void arm_idle_fallback(PrinterState& state, uint32_t backstop_ms) {
        core_.arm_idle_fallback(state, backstop_ms);
    }

    /// Called when the JSON-RPC response returns successfully — the command
    /// finished on the Klipper side.  Acts as a fallback completion signal in
    /// case the gcode_response stream didn't contain a recognised completion
    /// marker (e.g. "Mesh Bed Leveling Complete").
    void on_command_finished() {
        complete_success();
    }

    void on_gcode_response(const json& msg) {
        if (core_.completed()) {
            return;
        }

        // notify_gcode_response format: {"method": "notify_gcode_response", "params": ["line"]}
        if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty() ||
            !msg["params"][0].is_string()) {
            return;
        }

        const std::string& line = msg["params"][0].get_ref<const std::string&>();
        spdlog::trace("[BedMeshProgressCollector] Received: {}", line);

        // Check for errors first
        if (line.rfind("!! ", 0) == 0 ||                // Emergency errors
            line.rfind("Error:", 0) == 0 ||             // Standard errors
            line.find("error:") != std::string::npos) { // Python traceback
            complete_error(line);
            return;
        }

        // A command the firmware does not define. Klipper answers it with a console
        // line rather than an error and carries on with the script, so this line is
        // the only sign that a step of the calibration never ran.
        if (auto missing = helix::parse_unknown_command(line)) {
            if (helix::is_template_residue(*missing)) {
                spdlog::warn("[BedMeshProgressCollector] A macro rendered stray template text "
                             "'{}'; Klipper skipped that line and carried on",
                             *missing);
                return;
            }
            const bool calibrate_missing = missing->rfind("BED_MESH_CALIBRATE", 0) == 0;
            if (calibrate_missing || unknown_command_fails_) {
                complete_error(
                    calibrate_missing
                        ? std::string("BED_MESH_CALIBRATE requires [bed_mesh] in printer.cfg")
                        : "Unknown command: " + *missing);
                return;
            }
            // A user's or a detected macro may call something optional; what the
            // calibration stores decides whether it worked.
            spdlog::warn("[BedMeshProgressCollector] Calibration called undefined '{}'; still "
                         "waiting for it to finish",
                         *missing);
            return;
        }

        // Try to parse probe progress
        parse_probe_line(line);

        // Check for completion markers
        if (line.find("Mesh Bed Leveling Complete") != std::string::npos ||
            line.find("Mesh bed leveling complete") != std::string::npos) {
            complete_success();
            return;
        }
    }

  private:
    void parse_probe_line(const std::string& line) {
        if (auto pp = helix::parse_probe_progress(line)) {
            current_probe_ = pp->current;
            total_probes_ = pp->total;
            spdlog::debug("[BedMeshProgressCollector] Progress: {}/{}", pp->current, pp->total);
            if (on_progress_) {
                on_progress_(pp->current, pp->total);
            }
            return;
        }

        // Fallback: count "probe at X,Y is z=Z" lines (standard Klipper probe
        // output).  Some firmware variants don't emit the "Probing point X/Y"
        // progress line but do emit per-probe result lines.
        //
        // These lines are per SAMPLE, not per mesh point, so they are deduped by
        // position rather than divided by the configured sample count — the
        // divisor only works when we recognise the printer's probe section name,
        // and the Qidi Q2's is not one we enumerate, so a 36-point mesh counted
        // to 72 (#1224). ProbePointCounter keeps the divisor as a fallback for
        // lines whose coordinates do not parse.
        if (auto mesh_point = point_counter_.feed(line)) {
            spdlog::debug("[BedMeshProgressCollector] Probe sample line #{} → point {}/{}",
                          point_counter_.sample_lines(), *mesh_point, expected_probes_);
            if (on_progress_) {
                on_progress_(*mesh_point, expected_probes_);
            }
        }
    }

    void complete_success() {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return; // Already completed
        }

        // current_probe_/total_probes_ are only populated by the "Probing point
        // X/Y" branch. Firmware that emits bare "probe at X,Y" lines instead
        // (Creality K2, Qidi Q2) drives point_counter_ and leaves both at zero,
        // which logged a completed 81-point mesh as "Complete (0/0 probes)".
        const int done = current_probe_ > 0 ? current_probe_ : point_counter_.points();
        const int total = total_probes_ > 0 ? total_probes_ : expected_probes_;
        spdlog::info("[BedMeshProgressCollector] Complete ({}/{} probes)", done, total);
        unregister();

        if (on_complete_) {
            on_complete_();
        }
    }

    void complete_error(const std::string& message) {
        // unregister() below drops the client handler map's reference, which is
        // the collector's only strong owner, and the fallback handlers hold a
        // bare `this`. Pin the object for the rest of the callback chain
        // (prestonbrown/helixscreen#1543).
        auto keepalive = shared_from_this();
        if (!core_.try_complete()) {
            return;
        }

        spdlog::error("[BedMeshProgressCollector] Error: {}", message);
        unregister();

        if (on_error_) {
            MoonrakerError err = MoonrakerError::json_rpc_error("BED_MESH_CALIBRATE", message);
            on_error_(err);
        }
    }

    CalibrationCollectorCore core_;
    ProgressCallback on_progress_;
    MoonrakerAdvancedAPI::SuccessCallback on_complete_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;

    int current_probe_ = 0;
    int total_probes_ = 0;
    int expected_probes_ = 0; // hint from configfile (0 = unknown)
    // Every command the script calls is meant to exist (a shipped sequence).
    bool unknown_command_fails_ = false;
    // Dedupes "probe at X,Y is z=Z" samples into mesh points. Seeded with the
    // configured sample count, which it uses only when a line's coordinates
    // cannot be parsed.
    helix::ProbePointCounter point_counter_;
};
} // namespace helix

namespace {

/// Build "<preparation>\n<command>" for a probing operation, reporting the extra
/// time budget the preparation costs. One script on purpose: a failed
/// preparation aborts before the probe runs.
std::string with_probe_preparation(const std::string& command, helix::probe_prep::Operation op,
                                   uint32_t& extra_timeout_ms) {
    std::string script;
    extra_timeout_ms = helix::probe_prep::append_preparation(script, op, command);
    script += command;
    return script;
}

} // namespace

void MoonrakerAdvancedAPI::start_bed_mesh_calibrate(const BedMeshCommand& command,
                                                    BedMeshProgressCallback on_progress,
                                                    SuccessCallback on_complete,
                                                    ErrorCallback on_error, int expected_probes,
                                                    int probe_samples) {
    spdlog::info("[MoonrakerAPI] Starting bed mesh calibration with progress tracking "
                 "(expected_probes={}, probe_samples={})",
                 expected_probes, probe_samples);

    // Create collector to track progress
    auto collector = std::make_shared<BedMeshProgressCollector>(
        client_, std::move(on_progress), std::move(on_complete), on_error, expected_probes,
        probe_samples, command.shipped);

    collector->start();

    // command.script already names the profile the finished mesh is stored in.
    uint32_t prep_timeout_ms = 0;
    const std::string script =
        command.self_prepares
            ? command.script
            : with_probe_preparation(command.script, helix::probe_prep::Operation::BedMesh,
                                     prep_timeout_ms);

    api_.execute_gcode(
        script,
        [collector]() {
            // JSON-RPC returned — command finished on Klipper side.
            // Some firmware variants don't emit "Mesh Bed Leveling Complete"
            // in gcode_response, so use the RPC return as a fallback signal.
            spdlog::debug("[MoonrakerAPI] BED_MESH_CALIBRATE command finished");
            collector->on_command_finished();
        },
        [collector, on_error, command, prep_timeout_ms](const MoonrakerError& err) {
            report_collector_rpc_error(
                "BED_MESH_CALIBRATE", get_printer_state(), collector, on_error, err,
                command.self_prepares ? kSelfPreparedCalibrationTimeoutMs
                                      : kCalibrationTimeoutMs + prep_timeout_ms);
        },
        command.self_prepares ? kSelfPreparedCalibrationTimeoutMs
                              : kCalibrationTimeoutMs + prep_timeout_ms);
}

void MoonrakerAdvancedAPI::calculate_screws_tilt(ScrewTiltCallback on_success,
                                                 ErrorCallback on_error) {
    // The U1's [auto_screws_tilt_adjust] is the only screws-tilt module, so
    // SCREWS_TILT_CALCULATE does not exist there. The five-command sequence
    // feeds the SAME ScrewTiltCallback the standard path uses; the panel
    // cannot tell which dialect ran. The macro slot is meaningless without
    // upstream's command, so the auto path does not resolve it.
    if (screws_tilt::start_dialect_sequence(client_, api_, client_.hardware(), on_success,
                                            on_error)) {
        return;
    }

    // Resolved, not hardcoded: the ScrewsTilt slot lets the user pick
    // BED_LEVEL_SCREWS_TUNE. Moving the literal into a helper argument would still
    // make the slot a silent no-op. Falls back to the stock command when the slot
    // is empty, which is every printer that predates this setting.
    const StandardMacroInfo& slot = StandardMacros::instance().get(StandardMacroSlot::ScrewsTilt);
    helix::MacroScriptValues values;
    values.screws_tilt_builtin = client_.hardware().screws_tilt_builtin();
    const ResolvedMacroScript resolved = resolve_macro_script(slot, values);
    const std::string command = resolved.script.empty() ? "SCREWS_TILT_CALCULATE" : resolved.script;
    spdlog::info("[Moonraker API] Starting {}", command);

    // A shipped sequence spans several lines; the collector's diagnostics name the
    // one command in it that probes.
    const std::string& probing_command = resolved.shipped && !values.screws_tilt_builtin.empty()
                                             ? values.screws_tilt_builtin
                                             : command;

    // Create a collector to handle async response parsing
    // The collector will self-destruct when complete via shared_ptr ref counting
    auto collector =
        std::make_shared<ScrewsTiltCollector>(client_, on_success, on_error, probing_command);
    collector->start();

    // Send the G-code command
    // printer.gcode.script blocks until the command finishes, so the success callback
    // fires after all probing is done and all notify_gcode_response lines have been sent.
    // Honours self_prepares for the same reason the mesh path does: a shipped
    // sequence opens with its own tare, and prepending another would tare twice.
    uint32_t prep_timeout_ms = 0;
    const std::string script =
        resolved.self_prepares
            ? command
            : with_probe_preparation(command, helix::probe_prep::Operation::ScrewsTilt,
                                     prep_timeout_ms);

    api_.execute_gcode(
        script,
        [collector]() {
            // JSON-RPC returned — command fully executed, all results should be collected
            spdlog::debug("[Moonraker API] SCREWS_TILT_CALCULATE command finished");
            collector->on_command_finished();
        },
        [collector, on_error, prep_timeout_ms](const MoonrakerError& err) {
            report_collector_rpc_error("SCREWS_TILT_CALCULATE", get_printer_state(), collector,
                                       on_error, err, kCalibrationTimeoutMs + prep_timeout_ms);
        },
        kCalibrationTimeoutMs + prep_timeout_ms);
}

void MoonrakerAdvancedAPI::run_qgl(SuccessCallback /*on_success*/, ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] run_qgl() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("QGL not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::run_z_tilt_adjust(SuccessCallback /*on_success*/,
                                             ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] run_z_tilt_adjust() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("Z-tilt adjust not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::start_resonance_test(char axis, ShaperProgressCallback on_progress,
                                                InputShaperCallback on_complete,
                                                ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Starting SHAPER_CALIBRATE AXIS={}", axis);

    // Create collector to handle async response parsing
    auto collector =
        std::make_shared<InputShaperCollector>(client_, axis, on_progress, on_complete, on_error);
    collector->start();

    // Ask the printer what range it will actually sweep. Fire-and-forget: the
    // reply lands in milliseconds while SHAPER_CALIBRATE still has to home
    // and travel to the probe point, and if it never lands the collector
    // keeps its defaults.
    calibration::query_resonance_tester_config(
        client_, [collector](calibration::ResonanceTesterConfig cfg) {
            if (cfg.from_printer) {
                collector->set_sweep_range(cfg.min_freq, cfg.max_freq);
            }
        });

    // Send the G-code command
    // SHAPER_CALIBRATE sweeps the configured range (~2 min at the 5-135 Hz
    // default) then calculates best shapers (~30-60s)
    std::string cmd = "SHAPER_CALIBRATE AXIS=";
    cmd += axis;

    api_.execute_gcode(
        cmd, []() { spdlog::debug("[Moonraker API] SHAPER_CALIBRATE command accepted"); },
        [collector, on_error](const MoonrakerError& err) {
            report_collector_rpc_error(
                "SHAPER_CALIBRATE", get_printer_state(), collector, on_error, err, kShaperTimeoutMs,
                [collector]() { collector->log_stall_diagnostics("SHAPER_CALIBRATE RPC lost"); });
        },
        kShaperTimeoutMs);
}

void MoonrakerAdvancedAPI::start_klippain_shaper_calibration(const std::string& /*axis*/,
                                                             SuccessCallback /*on_success*/,
                                                             ErrorCallback on_error) {
    spdlog::warn("[Moonraker API] start_klippain_shaper_calibration() not yet implemented");
    if (on_error) {
        MoonrakerError err = MoonrakerError::unknown("Klippain Shake&Tune not yet implemented");
        on_error(err);
    }
}

void MoonrakerAdvancedAPI::set_input_shaper(char axis, const std::string& shaper_type,
                                            double frequency, SuccessCallback on_success,
                                            ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Setting input shaper: {}={} @ {:.1f} Hz", axis, shaper_type,
                 frequency);

    // Build SET_INPUT_SHAPER command
    std::string cmd = fmt::format("SET_INPUT_SHAPER SHAPER_FREQ_{}={:g} SHAPER_TYPE_{}={}", axis,
                                  frequency, axis, shaper_type);

    api_.execute_gcode(cmd, on_success, on_error);
}

void MoonrakerAdvancedAPI::measure_axes_noise(NoiseCheckCallback on_complete,
                                              ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Starting MEASURE_AXES_NOISE");

    // Create collector to handle async response parsing
    auto collector = std::make_shared<NoiseCheckCollector>(client_, on_complete, on_error);
    collector->start();

    // Send the G-code command
    api_.execute_gcode(
        "MEASURE_AXES_NOISE",
        []() { spdlog::debug("[Moonraker API] MEASURE_AXES_NOISE command accepted"); },
        [collector, on_error](const MoonrakerError& err) {
            report_collector_rpc_error("MEASURE_AXES_NOISE", get_printer_state(), collector,
                                       on_error, err, kShaperTimeoutMs);
        },
        kShaperTimeoutMs);
}

void MoonrakerAdvancedAPI::get_input_shaper_config(InputShaperConfigCallback on_success,
                                                   ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying input shaper configuration");

    // Query configfile to get saved input_shaper settings from printer.cfg
    // (the input_shaper runtime object is empty — config lives in configfile)
    json params = {{"objects", json::object({{"configfile", json::array({"config"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success, on_error](const json& response) {
            InputShaperConfig config;
            // A field present with a type we cannot read fails the whole read,
            // like a parse error does.
            bool parse_ok = true;
            auto read_string = [&parse_ok](const json& shaper, const char* key) -> std::string {
                const json* v = json_util::find_member(shaper, key);
                if (!v) {
                    return "";
                }
                if (!v->is_string()) {
                    parse_ok = false;
                    return "";
                }
                return v->get<std::string>();
            };
            auto read_float = [&parse_ok](const json& shaper, const char* key, float& out) {
                const json* v = json_util::find_member(shaper, key);
                if (!v) {
                    return;
                }
                if (v->is_number()) {
                    out = v->get<float>();
                } else if (v->is_string()) {
                    // configfile returns frequencies as strings
                    const auto parsed = text_io::parse_leading<float>(v->get<std::string>());
                    if (parsed) {
                        out = *parsed;
                    } else {
                        parse_ok = false;
                    }
                } else {
                    parse_ok = false;
                }
            };

            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].contains("configfile") &&
                response["result"]["status"]["configfile"].contains("config") &&
                response["result"]["status"]["configfile"]["config"].contains("input_shaper")) {
                const auto& shaper =
                    response["result"]["status"]["configfile"]["config"]["input_shaper"];

                config.shaper_type_x = read_string(shaper, "shaper_type_x");
                config.shaper_type_y = read_string(shaper, "shaper_type_y");
                read_float(shaper, "shaper_freq_x", config.shaper_freq_x);
                read_float(shaper, "shaper_freq_y", config.shaper_freq_y);
                read_float(shaper, "damping_ratio_x", config.damping_ratio_x);
                read_float(shaper, "damping_ratio_y", config.damping_ratio_y);

                if (!parse_ok) {
                    spdlog::error(
                        "[Moonraker API] Failed to parse input shaper config: malformed field");
                    if (on_error) {
                        MoonrakerError err =
                            MoonrakerError::unknown("Failed to parse input shaper config");
                        on_error(err);
                    }
                    return;
                }

                // Input shaper is configured if at least one axis has a type set
                config.is_configured =
                    !config.shaper_type_x.empty() || !config.shaper_type_y.empty();

                spdlog::info("[Moonraker API] Input shaper config: X={}@{:.1f}Hz, Y={}@{:.1f}Hz",
                             config.shaper_type_x, config.shaper_freq_x, config.shaper_type_y,
                             config.shaper_freq_y);
            } else {
                spdlog::debug("[Moonraker API] Input shaper section not found in printer config");
                config.is_configured = false;
            }

            if (on_success) {
                on_success(config);
            }
        },
        on_error);
}

void MoonrakerAdvancedAPI::get_machine_limits(MachineLimitsCallback on_success,
                                              ErrorCallback on_error) {
    spdlog::debug("[Moonraker API] Querying machine limits from toolhead");

    // Query toolhead object for current velocity/acceleration limits
    json params = {{"objects", {{"toolhead", nullptr}}}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [on_success, on_error](const json& response) {
            if (!response.contains("result") || !response["result"].contains("status") ||
                !response["result"]["status"].contains("toolhead")) {
                spdlog::warn("[Moonraker API] Toolhead object not available in response");
                if (on_error) {
                    MoonrakerError err = MoonrakerError::unknown("Toolhead object not available");
                    on_error(err);
                }
                return;
            }

            const auto& toolhead = response["result"]["status"]["toolhead"];
            MachineLimits limits;

            // Extract limits with safe defaults
            limits.max_velocity = json_util::safe_double(toolhead, "max_velocity", 0.0);
            limits.max_accel = json_util::safe_double(toolhead, "max_accel", 0.0);
            limits.max_accel_to_decel = json_util::safe_double(toolhead, "max_accel_to_decel", 0.0);
            limits.square_corner_velocity =
                json_util::safe_double(toolhead, "square_corner_velocity", 0.0);
            limits.max_z_velocity = json_util::safe_double(toolhead, "max_z_velocity", 0.0);
            limits.max_z_accel = json_util::safe_double(toolhead, "max_z_accel", 0.0);

            spdlog::info("[Moonraker API] Machine limits: vel={:.0f} accel={:.0f} "
                         "accel_to_decel={:.0f} scv={:.1f} z_vel={:.0f} z_accel={:.0f}",
                         limits.max_velocity, limits.max_accel, limits.max_accel_to_decel,
                         limits.square_corner_velocity, limits.max_z_velocity, limits.max_z_accel);

            if (on_success) {
                on_success(limits);
            }
        },
        on_error);
}

void MoonrakerAdvancedAPI::set_machine_limits(const MachineLimits& limits,
                                              SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[Moonraker API] Setting machine limits");

    // Warn about Z limits that cannot be set at runtime
    if (limits.max_z_velocity > 0 || limits.max_z_accel > 0) {
        spdlog::warn("[Moonraker API] max_z_velocity and max_z_accel cannot be set "
                     "via SET_VELOCITY_LIMIT - they require config changes");
    }

    // Build SET_VELOCITY_LIMIT command with only non-zero parameters
    // Use fixed precision to avoid floating point representation issues
    std::string cmd = "SET_VELOCITY_LIMIT";

    bool has_params = false;

    if (limits.max_velocity > 0) {
        cmd += fmt::format(" VELOCITY={:.1f}", limits.max_velocity);
        has_params = true;
    }
    if (limits.max_accel > 0) {
        cmd += fmt::format(" ACCEL={:.1f}", limits.max_accel);
        has_params = true;
    }
    if (limits.max_accel_to_decel > 0) {
        cmd += fmt::format(" ACCEL_TO_DECEL={:.1f}", limits.max_accel_to_decel);
        has_params = true;
    }
    if (limits.square_corner_velocity > 0) {
        cmd += fmt::format(" SQUARE_CORNER_VELOCITY={:.1f}", limits.square_corner_velocity);
        has_params = true;
    }

    if (!has_params) {
        helix::report_validation_error(on_error, "set_machine_limits",
                                       "No valid machine limit parameters provided",
                                       "No machine limits to apply.");
        return;
    }

    spdlog::debug("[Moonraker API] Executing: {}", cmd);
    api_.execute_gcode(cmd, on_success, on_error);
}

void MoonrakerAdvancedAPI::save_config(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Sending SAVE_CONFIG");
    api_.execute_gcode("SAVE_CONFIG", std::move(on_success), std::move(on_error));
}

void MoonrakerAdvancedAPI::execute_macro(const std::string& name,
                                         const std::map<std::string, std::string>& params,
                                         SuccessCallback on_success, ErrorCallback on_error,
                                         uint32_t timeout_ms, bool suppress_auto_toast) {
    // Validate macro name - only allow alphanumeric, underscore (standard Klipper macro names)
    if (name.empty()) {
        helix::report_validation_error(on_error, "execute_macro", "Macro name cannot be empty",
                                       "Cannot run a macro without a name.");
        return;
    }

    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            helix::report_validation_error(
                on_error, "execute_macro", "Macro name contains illegal characters",
                fmt::format("Invalid macro name '{}'. Contains unsafe characters.", name));
            return;
        }
    }

    // Build G-code: MACRO_NAME KEY1=value1 KEY2=value2
    std::string gcode = name;

    for (const auto& [key, value] : params) {
        // Validate param key - only alphanumeric and underscore
        bool key_valid = !key.empty();
        for (char c : key) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                key_valid = false;
                break;
            }
        }
        if (!key_valid) {
            spdlog::warn("[Moonraker API] Skipping invalid param key '{}'", key);
            continue;
        }

        // Validate param value - reject dangerous characters that could enable G-code injection
        // Allow: alphanumeric, underscore, hyphen, dot, space (for human-readable values)
        bool value_valid = true;
        for (char c : value) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' &&
                c != ' ') {
                value_valid = false;
                break;
            }
        }
        if (!value_valid) {
            spdlog::warn("[Moonraker API] Skipping param with unsafe value: {}={}", key, value);
            continue;
        }

        // Safe to include - quote if it has spaces
        if (value.find(' ') != std::string::npos) {
            gcode += " " + key + "=\"" + value + "\"";
        } else {
            gcode += " " + key + "=" + value;
        }
    }

    std::string gcode_str = std::move(gcode);
    spdlog::debug("[Moonraker API] Executing macro: {}", gcode_str);

    // Default to kMacroTimeoutMs (5 min) — user macros can do anything
    uint32_t effective_timeout = timeout_ms > 0 ? timeout_ms : MoonrakerAPI::kMacroTimeoutMs;
    // suppress_auto_toast is CallerIntent::silent: it opts out of the tracker's
    // generic fallback toast and nothing else. Whether the `!!` broadcast dedups
    // follows from on_error being a real user-facing report, which is the
    // default for macro callers (see rpc_error_policy.h).
    api_.execute_gcode(gcode_str, std::move(on_success), std::move(on_error), effective_timeout,
                       /*silent=*/suppress_auto_toast, /*on_queued=*/nullptr,
                       /*caller_surfaces_errors=*/true);
}

std::vector<MacroInfo> MoonrakerAdvancedAPI::get_user_macros(bool /*include_system*/) const {
    spdlog::warn("[Moonraker API] get_user_macros() not yet implemented");
    return {};
}

// ============================================================================
// Advanced Panel Operations - PID Calibration
// ============================================================================

void MoonrakerAdvancedAPI::get_heater_pid_values(
    const std::string& heater, MoonrakerAdvancedAPI::PIDCalibrateCallback on_complete,
    MoonrakerAdvancedAPI::ErrorCallback on_error) {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [heater, on_complete, on_error](const json& response) {
            if (!response.contains("result") || !response["result"].contains("status") ||
                !response["result"]["status"].contains("configfile") ||
                !response["result"]["status"]["configfile"].contains("settings")) {
                spdlog::debug("[Moonraker API] configfile.settings not available in response");
                if (on_error) {
                    on_error(MoonrakerError::unknown("configfile.settings not available",
                                                     "get_pid_values"));
                }
                return;
            }

            const json& settings = response["result"]["status"]["configfile"]["settings"];

            if (!settings.contains(heater)) {
                if (on_error) {
                    on_error(MoonrakerError::unknown("Heater '" + heater + "' not in config",
                                                     "get_pid_values"));
                }
                return;
            }

            const json& h = settings[heater];
            if (h.contains("pid_kp") && h.contains("pid_ki") && h.contains("pid_kd")) {
                const json& kp_v = h["pid_kp"];
                const json& ki_v = h["pid_ki"];
                const json& kd_v = h["pid_kd"];
                if (!(kp_v.is_number() || kp_v.is_boolean()) ||
                    !(ki_v.is_number() || ki_v.is_boolean()) ||
                    !(kd_v.is_number() || kd_v.is_boolean())) {
                    spdlog::warn("[Moonraker API] Error parsing PID values: non-numeric value");
                    if (on_error) {
                        on_error(MoonrakerError::unknown("Parse error: non-numeric PID value",
                                                         "get_pid_values"));
                    }
                    return;
                }
                float kp = kp_v.get<float>();
                float ki = ki_v.get<float>();
                float kd = kd_v.get<float>();
                spdlog::debug(
                    "[Moonraker API] Fetched PID values for {}: Kp={:.3f} Ki={:.3f} Kd={:.3f}",
                    heater, kp, ki, kd);
                if (on_complete) {
                    on_complete(kp, ki, kd);
                }
            } else {
                if (on_error) {
                    on_error(MoonrakerError::unknown("No PID values for heater '" + heater + "'",
                                                     "get_pid_values"));
                }
            }
        },
        [on_error](const MoonrakerError& err) {
            spdlog::debug("[Moonraker API] Failed to fetch PID values: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        });
}

void MoonrakerAdvancedAPI::get_heater_control_type(
    const std::string& heater, MoonrakerAdvancedAPI::HeaterControlTypeCallback on_complete,
    MoonrakerAdvancedAPI::ErrorCallback on_error) {
    json params = {{"objects", json::object({{"configfile", json::array({"settings"})}})}};

    client_.send_jsonrpc(
        "printer.objects.query", params,
        [heater, on_complete, on_error](const json& response) {
            if (!response.contains("result") || !response["result"].contains("status") ||
                !response["result"]["status"].contains("configfile") ||
                !response["result"]["status"]["configfile"].contains("settings")) {
                spdlog::debug(
                    "[Moonraker API] configfile.settings not available for control type query");
                if (on_error) {
                    on_error(MoonrakerError::unknown("configfile.settings not available",
                                                     "get_heater_control_type"));
                }
                return;
            }

            const json& settings = response["result"]["status"]["configfile"]["settings"];

            if (!settings.contains(heater)) {
                if (on_error) {
                    on_error(MoonrakerError::unknown("Heater '" + heater + "' not in config",
                                                     "get_heater_control_type"));
                }
                return;
            }

            const json& h = settings[heater];
            std::string control = json_util::safe_string(h, "control", "pid");
            spdlog::debug("[Moonraker API] Heater '{}' control type: {}", heater, control);
            if (on_complete) {
                on_complete(control);
            }
        },
        [on_error](const MoonrakerError& err) {
            spdlog::debug("[Moonraker API] Failed to fetch heater control type: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        });
}

void MoonrakerAdvancedAPI::start_pid_calibrate(
    const std::string& heater, int target_temp,
    MoonrakerAdvancedAPI::PIDCalibrateCallback on_complete, ErrorCallback on_error,
    PIDProgressCallback on_progress) {
    spdlog::info("[MoonrakerAPI] Starting PID calibration for {} at {}°C", heater, target_temp);

    auto collector = std::make_shared<PIDCalibrateCollector>(client_, std::move(on_complete),
                                                             on_error, std::move(on_progress));
    collector->start();

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "PID_CALIBRATE HEATER=%s TARGET=%d", heater.c_str(), target_temp);

    // silent=true: PID errors are handled by the collector and UI panel, not global toast
    api_.execute_gcode(
        cmd, nullptr,
        [collector, on_error](const MoonrakerError& err) {
            // The notify_gcode_response collector — not this RPC — is the authority for
            // PID_CALIBRATE completion. Slow-cooling beds can run longer than the RPC
            // timeout (#988), and a dropped socket is the transport vanishing, not the
            // printer's opinion of the macro (#1543): on either, the collector keeps
            // listening for the eventual "PID parameters:" result line, with the
            // busy->idle follow-up as the terminal backstop. The UI panel keeps its
            // own user-facing "taking longer than expected" backstop alongside it.
            // A genuine RPC
            // error (heater misconfigured) is still terminal.
            report_collector_rpc_error("PID_CALIBRATE", get_printer_state(), collector, on_error,
                                       err, kPidTimeoutMs);
        },
        kPidTimeoutMs, true);
}

helix::SharedCallback<void()>
MoonrakerAdvancedAPI::start_pa_calibrate(const helix::pacal::Procedure& proc,
                                         MoonrakerAdvancedAPI::PACalibrateCallback on_complete,
                                         ErrorCallback on_error, PAProgressCallback on_progress) {
    spdlog::info("[MoonrakerAPI] Starting pressure advance calibration: {} ({})", proc.start_gcode,
                 proc.provider);

    auto collector = std::make_shared<PACalibrateCollector>(client_, proc, std::move(on_complete),
                                                            on_error, std::move(on_progress));
    collector->start();

    const std::string command_word = proc.command_word;

    // silent=true: a refusal is rendered by the panel as the whole stage, in the
    // machine's own words. A global toast on top of that says it twice.
    api_.execute_gcode(
        proc.start_gcode, nullptr,
        [collector, on_error, command_word](const MoonrakerError& err) {
            // Same contract as PID_CALIBRATE: the console result line, not this
            // RPC, is the authority. A calibration that spends two minutes
            // heating can outlive the RPC timeout and still be running, so on
            // timeout we leave the collector listening.
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[MoonrakerAPI] {} RPC timed out; collector still listening for"
                             " result (calibration may still be running)",
                             command_word);
                return;
            }
            spdlog::error("[MoonrakerAPI] Failed to send {}: {}", command_word, err.message);
            collector->mark_completed();
            collector->unregister();
            if (on_error)
                on_error(err);
        },
        proc.timeout_ms, true);

    // Klipper cannot interrupt a running command, so cancelling only stops
    // listening: the firmware finishes the measurement it is in.
    return [collector]() {
        collector->mark_completed();
        collector->unregister();
    };
}
void MoonrakerAdvancedAPI::start_mpc_calibrate(
    const std::string& heater, int target_temp, int fan_breakpoints,
    MoonrakerAdvancedAPI::MPCCalibrateCallback on_complete, ErrorCallback on_error,
    MPCProgressCallback on_progress) {
    spdlog::info("[MoonrakerAPI] Starting MPC calibration for {} at {}°C (fan_breakpoints={})",
                 heater, target_temp, fan_breakpoints);

    auto collector = std::make_shared<MPCCalibrateCollector>(
        client_, std::move(on_complete), on_error, std::move(on_progress), fan_breakpoints > 0);
    collector->start();

    std::string cmd = fmt::format("MPC_CALIBRATE HEATER={} TARGET={}", heater, target_temp);
    if (fan_breakpoints > 0) {
        cmd += fmt::format(" FAN_BREAKPOINTS={}", fan_breakpoints);
    }

    // silent=true: MPC errors are handled by the collector and UI panel, not global toast
    api_.execute_gcode(
        cmd, nullptr,
        [collector, on_error](const MoonrakerError& err) {
            // The notify_gcode_response collector — not this RPC — is the authority
            // for MPC_CALIBRATE completion. The calibration keeps running past the RPC
            // ceiling (prestonbrown/helixscreen#1544), and a dropped socket is the
            // transport vanishing, not the printer's opinion of the macro (#1543): on
            // either, the collector keeps listening for the eventual result block, with
            // the busy->idle follow-up as the terminal backstop. A genuine RPC error
            // is still terminal.
            report_collector_rpc_error("MPC_CALIBRATE", get_printer_state(), collector, on_error,
                                       err, kMpcTimeoutMs);
        },
        kMpcTimeoutMs, true);
}

// ============================================================================
// Belt Tension Operations
// ============================================================================

void MoonrakerAdvancedAPI::detect_belt_hardware(BeltHardwareCallback on_complete,
                                                ErrorCallback on_error) {
    spdlog::info("[MoonrakerAPI] Detecting belt tension hardware capabilities");

    // Step 1: Query printer.objects.list. The response is unused - accelerometer
    // presence comes from PrinterDiscovery below, not from parsing this list
    // (Klipper's objects/list omits on-demand calibration tools like adxl345).
    json params = json::object();
    client_.send_jsonrpc(
        "printer.objects.list", params,
        [this, on_complete, on_error](const json& /*response*/) {
            helix::calibration::BeltTensionHardware hw;

            // AccelSensorManager is the documented single source of truth for
            // accelerometer presence, and the discovery sequence seeds it from
            // configfile.config alongside the probe manager
            // (moonraker_discovery_sequence.cpp). Accelerometer modules have no
            // get_status(), so configfile is the only place they appear at all.
            hw.has_adxl = helix::sensors::AccelSensorManager::instance().sensor_count() > 0;

            spdlog::info("[MoonrakerAPI] Belt HW scan: adxl={}", hw.has_adxl);

            // Step 2: Query kinematics type
            json query_params;
            query_params["objects"]["configfile"] = json::array({"settings"});
            client_.send_jsonrpc(
                "printer.objects.query", query_params,
                [hw, on_complete, on_error](const json& config_response) mutable {
                    if (config_response.contains("result") &&
                        config_response["result"].contains("status") &&
                        config_response["result"]["status"].contains("configfile") &&
                        config_response["result"]["status"]["configfile"].contains("settings")) {
                        const auto& settings =
                            config_response["result"]["status"]["configfile"]["settings"];

                        if (settings.contains("printer") &&
                            settings["printer"].contains("kinematics")) {
                            const auto& kinematics = settings["printer"]["kinematics"];
                            if (!kinematics.is_string()) {
                                spdlog::error(
                                    "[MoonrakerAPI] Failed to parse kinematics: not a string");
                                if (on_error)
                                    on_error(MoonrakerError::json_rpc_error(
                                        "", "Failed to detect kinematics: not a string"));
                                return;
                            }
                            hw.kinematics_name = kinematics.get<std::string>();

                            // COREXY only where the two diagonals are the two
                            // belt paths the comparison sweeps.
                            if (belt_path_kinematics(hw.kinematics_name)) {
                                hw.kinematics = helix::calibration::KinematicsType::COREXY;
                            } else if (hw.kinematics_name == "cartesian" ||
                                       hw.kinematics_name == "limited_cartesian") {
                                hw.kinematics = helix::calibration::KinematicsType::CARTESIAN;
                            } else {
                                hw.kinematics = helix::calibration::KinematicsType::UNKNOWN;
                            }
                        }
                    }

                    spdlog::info("[MoonrakerAPI] Belt HW kinematics: {} (type={})",
                                 hw.kinematics_name, static_cast<int>(hw.kinematics));

                    if (on_complete)
                        on_complete(hw);
                },
                [on_error](const MoonrakerError& err) {
                    spdlog::error("[MoonrakerAPI] Kinematics query failed: {}", err.message);
                    if (on_error)
                        on_error(err);
                });
        },
        [on_error](const MoonrakerError& err) {
            spdlog::error("[MoonrakerAPI] Object list query failed: {}", err.message);
            if (on_error)
                on_error(err);
        });
}

MoonrakerAdvancedAPI::BeltRunCancel MoonrakerAdvancedAPI::test_belt_resonance(
    const std::string& axis_param, const std::string& output_name,
    BeltSweepProgressCallback on_progress, BeltResponseCallback on_complete, ErrorCallback on_error,
    std::optional<calibration::BeltSweepWindow> window) {
    spdlog::info("[MoonrakerAPI] Starting belt resonance test: axis={}, name={}", axis_param,
                 output_name);

    auto collector = std::make_shared<BeltResonanceCollector>(
        client_, std::move(on_progress), std::move(on_complete), std::move(on_error), window);
    collector->start();

    // Ask the printer what range it will sweep, so progress is scaled by the
    // printer's own [resonance_tester] rather than a guess. The reply always
    // arrives (defaults on error), so the collector's progress hold is finite.
    calibration::query_resonance_tester_config(
        client_,
        [collector](calibration::ResonanceTesterConfig cfg) { collector->set_config(cfg); });

    // SWEEPING_PERIOD=0 forces the pulse-only excitation on firmware whose
    // [resonance_tester] defaults to sweeping: the slow sweep smooths over the
    // mechanical faults a belt comparison looks for. Firmware without the
    // parameter ignores it.
    std::string freq_range;
    if (window) {
        // Inward to whole Hz: Klipper takes any FREQ_END up to 300 Hz, so rounding
        // the end up could sweep past the printer's max_freq.
        freq_range = fmt::format(" FREQ_START={:.0f} FREQ_END={:.0f}",
                                 std::ceil(window->freq_start_hz), std::floor(window->freq_end_hz));
    }
    const std::string gcode =
        fmt::format("TEST_RESONANCES AXIS={}{} OUTPUT=resonances NAME={} SWEEPING_PERIOD=0",
                    axis_param, freq_range, output_name);
    // A sweep takes minutes, so the request rides the long calibration timeout
    // rather than the tracker default. A transport loss only means the RPC
    // reply vanished - Klipper keeps sweeping and the console lines keep
    // arriving, so the collector stays registered and the run finishes on its
    // terminal line. Anything else is Moonraker refusing the script.
    api_.execute_gcode(
        gcode, []() {},
        [collector](const MoonrakerError& err) {
            if (err.is_transport_loss()) {
                spdlog::debug("[MoonrakerAPI] TEST_RESONANCES RPC lost to the transport ({}); "
                              "collector still listening - the sweep may still be running",
                              err.message);
                return;
            }
            collector->complete_error(err.message);
        },
        IAdvancedAPI::kShaperTimeoutMs, /*silent=*/false, /*on_queued=*/nullptr,
        /*caller_surfaces_errors=*/true);

    return [weak = std::weak_ptr(collector)]() {
        if (auto c = weak.lock()) {
            c->cancel();
        }
    };
}

// ============================================================================
