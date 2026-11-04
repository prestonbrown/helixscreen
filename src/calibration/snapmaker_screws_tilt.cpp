// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_screws_tilt.h"

#include "i_moonraker_client.h"
#include "moonraker_advanced_api.h"
#include "snapmaker_exceptions.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <optional>

namespace helix {

namespace {

// Klipper emits JSON null for keys a printer does not define; reading one as
// 0 would fake a screw sitting exactly at the target height.
[[nodiscard]] bool read_number(const nlohmann::json& status, const std::string& field, float& out) {
    if (!status.is_object()) {
        return false;
    }
    const auto it = status.find(field);
    if (it == status.end() || it->is_null() || !it->is_number()) {
        return false;
    }
    out = it->get<float>();
    return true;
}

} // namespace

AutoScrewsTiltResults parse_auto_screws_tilt(const nlohmann::json& status,
                                             const nlohmann::json& config_section, float pitch_mm) {
    AutoScrewsTiltResults results;

    float target_z = 0.0f;
    if (!read_number(status, "target_z", target_z)) {
        results.error = "auto_screws_tilt_adjust: missing or null target_z";
        return results;
    }

    std::vector<ScrewTiltResult> screws;
    screws.reserve(4);
    for (int i = 1; i <= 4; i++) {
        const std::string base_key = "base_point" + std::to_string(i);

        float base_z = 0.0f;
        if (!read_number(status, base_key, base_z)) {
            results.error = "auto_screws_tilt_adjust: missing or null " + base_key;
            return results;
        }

        ScrewTiltResult screw;
        screw.z_height = base_z;
        // Measured against target_z, not against a base screw: every screw
        // can carry a real adjustment.
        screw.is_reference = false;

        // Coordinates and display names come from the configfile section.
        if (config_section.is_object()) {
            const auto coords = config_section.find("screw" + std::to_string(i));
            if (coords != config_section.end() && coords->is_string()) {
                std::sscanf(coords->get_ref<const std::string&>().c_str(), "%f,%f", &screw.x_pos,
                            &screw.y_pos);
            }
            const auto name = config_section.find("screw" + std::to_string(i) + "_name");
            if (name != config_section.end() && name->is_string()) {
                screw.screw_name = name->get_ref<const std::string&>();
            }
        }

        // Signed minutes carry the direction (CW positive, CCW negative), the
        // same convention signed_adjustment_minutes() parses back out.
        const int signed_minutes = screw_minutes_for_mm(target_z - base_z, pitch_mm);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s %02d:%02d", signed_minutes >= 0 ? "CW" : "CCW",
                      std::abs(signed_minutes) / 60, std::abs(signed_minutes) % 60);
        screw.adjustment = buf;

        screws.push_back(std::move(screw));
    }

    results.screws = std::move(screws);
    return results;
}

namespace snapmaker {
namespace screws_tilt {

bool stale_calibration_state(int main_state, const std::string& probe_step) {
    if (main_state != kMainStateScrewsTiltAdjust) {
        return false;
    }
    static const std::array<const char*, 6> kNotRunning = {
        "adjust_idle",
        "adjust_complete",
        "adjust_failed",
        "adjust_homing_failed",
        "adjust_plate_detection_error",
        "adjust_aborted",
    };
    return std::find(kNotRunning.begin(), kNotRunning.end(), probe_step) != kNotRunning.end();
}

std::optional<int> main_state_from_status(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    const auto manager = status.find("machine_state_manager");
    if (manager == status.end() || !manager->is_object()) {
        return std::nullopt;
    }
    const auto state = manager->find("main_state");
    if (state == manager->end() || !state->is_number()) {
        return std::nullopt;
    }
    return state->get<int>();
}

namespace {

// result.status out of an objects.query response; the response can instead
// be an error object, which carries no status.
const nlohmann::json* query_status(const nlohmann::json& response) {
    if (!response.is_object()) {
        return nullptr;
    }
    const auto result = response.find("result");
    if (result == response.end() || !result->is_object()) {
        return nullptr;
    }
    const auto status = result->find("status");
    if (status == result->end() || !status->is_object()) {
        return nullptr;
    }
    return &*status;
}

} // namespace

bool plate_still_on_bed(const std::string& error_message) {
    const auto code = snapmaker::decode_exception_code(error_message);
    return code && code->id == 530 && code->index == 0 && code->code == 11;
}

AutoScrewsTiltResults results_from_query(const nlohmann::json& response) {
    const nlohmann::json* status = query_status(response);
    const nlohmann::json* module_status = nullptr;
    if (status) {
        const auto module = status->find(kModuleName);
        if (module != status->end() && module->is_object()) {
            module_status = &*module;
        }
    }
    if (!module_status) {
        AutoScrewsTiltResults none;
        none.error = std::string(kModuleName) + ": no status object in query response";
        return none;
    }

    nlohmann::json config_section = nlohmann::json::object();
    const auto configfile = status->find("configfile");
    if (configfile != status->end() && configfile->is_object()) {
        const auto settings = configfile->find("settings");
        if (settings != configfile->end() && settings->is_object()) {
            const auto section = settings->find(kModuleName);
            if (section != settings->end() && section->is_object()) {
                config_section = *section;
            }
        }
    }
    return parse_auto_screws_tilt(*module_status, config_section);
}

void request_exit(IMoonrakerClient& client) {
    json params = {
        {"objects", json::object({{"machine_state_manager", json::array({"main_state"})}})}};
    client.send_jsonrpc("printer.objects.query", params, [&client](const json& response) {
        const json* status = query_status(response);
        const std::optional<int> state = status ? main_state_from_status(*status) : std::nullopt;
        if (!state || *state != kMainStateScrewsTiltAdjust) {
            return;
        }
        spdlog::info("[AutoScrewsTilt] Leaving the firmware screws-tilt state");
        client.gcode_script(kCmdExit);
    });
}

void reconcile_on_connect(IMoonrakerClient& client, const nlohmann::json& initial_status) {
    const std::optional<int> held = main_state_from_status(initial_status);
    if (!held || *held != kMainStateScrewsTiltAdjust) {
        // The discovery subscription carries machine_state_manager only under
        // mmu_type() == SNAPMAKER, which does not track the screws dialect:
        // a U1 without SnapSwap never subscribes, and this is where that
        // coupling shows up as a missing object.
        if (!initial_status.contains("machine_state_manager")) {
            spdlog::debug("[AutoScrewsTilt] No machine_state_manager in the discovery snapshot "
                          "- connect-time reconcile has nothing to read");
        }
        return;
    }
    // probe_step is not part of the discovery subscription, so read it (and a
    // fresh main_state) now, once, at connect time.
    json params = {{"objects", json::object({{"machine_state_manager", json::array({"main_state"})},
                                             {kModuleName, json::array({"probe_step"})}})}};
    client.send_jsonrpc("printer.objects.query", params, [&client](const json& response) {
        const json* status = query_status(response);
        if (!status) {
            return;
        }
        const std::optional<int> state = main_state_from_status(*status);

        std::string probe_step;
        const auto module = status->find(kModuleName);
        if (module != status->end() && module->is_object()) {
            const auto step = module->find("probe_step");
            if (step != module->end() && step->is_string()) {
                probe_step = step->get_ref<const std::string&>();
            }
        }

        if (!state || !stale_calibration_state(*state, probe_step)) {
            spdlog::info("[AutoScrewsTilt] Screws-tilt state held with work in flight "
                         "(probe_step='{}') - leaving it to its driver",
                         probe_step.empty() ? "<unknown>" : probe_step);
            return;
        }
        spdlog::info("[AutoScrewsTilt] Clearing a stale screws-tilt state (probe_step={})",
                     probe_step);
        // kCmdExit needs exactly reconcile's own precondition (main_state ==
        // SCREWS_TILT_ADJUST), just re-read above. If the state changed
        // between that read and this send, the macro throws - a benign race
        // the fire-and-forget RPC layer logs; there is nothing to retry
        // against a state we no longer hold.
        client.gcode_script(kCmdExit);
    });
}

/**
 * @brief State machine driving the U1's AUTO_SCREWS_TILT_ADJUST sequence
 *
 * Five firmware commands replace SCREWS_TILT_CALCULATE on printers whose
 * screws_tilt_dialect() is SnapmakerAuto; the result arrives as a status
 * object, not console lines. Each command blocks until Klipper finishes it,
 * so the sequence chains on execute_gcode completion and every terminal path
 * funnels into complete_success()/complete_error() - both of which release
 * the firmware calibration state. A leaked SCREWS_TILT_ADJUST makes
 * unrelated filament operations refuse forever, so the gated exit on every
 * path is mandatory, not best-effort.
 *
 * The plate gate between homing and probing is equally mandatory: probing
 * through the PEI sheet shifts per-corner tilt far beyond the probe's own
 * noise floor, so a detected sheet fails the run with instructions instead
 * of producing wrong numbers.
 */
class AutoScrewsTiltCollector : public std::enable_shared_from_this<AutoScrewsTiltCollector> {
  public:
    AutoScrewsTiltCollector(IMoonrakerClient& client, IMoonrakerAPI& api,
                            ScrewTiltCallback on_success,
                            MoonrakerAdvancedAPI::ErrorCallback on_error)
        : client_(client), api_(api), on_success_(std::move(on_success)),
          on_error_(std::move(on_error)) {}

    void start() {
        spdlog::info("[AutoScrewsTiltCollector] Starting the U1 screws-tilt sequence");
        send_step(kCmdEntry, &AutoScrewsTiltCollector::on_entry_done);
    }

  private:
    using StepFn = void (AutoScrewsTiltCollector::*)();

    /// One firmware command of the sequence. Success runs @p next; any
    /// failure ends the run. Each command gets its own full
    /// kCalibrationTimeoutMs, so the sequence spans five budgets, not one
    /// shared one.
    ///
    /// Every failure is terminal here, including TIMEOUT and
    /// CONNECTION_LOST, which report_collector_rpc_error() absorbs for the
    /// line-driven collectors (prestonbrown/helixscreen#1543). Those wait on
    /// console lines from ONE long macro, so losing the transport and
    /// continuing to listen is safe. This is a chain of five dependent RPCs:
    /// after a loss there is no knowing which step the firmware is actually
    /// on, and issuing the next command blind could collide with one still
    /// running. Failing terminally with the state-gated exit cleans up
    /// deterministically, and connect-time reconciliation is the backstop.
    /// The cost: a step that outlives its timeout surfaces to the user as a
    /// failure while the firmware is still working.
    void send_step(const char* command, StepFn next) {
        auto self = shared_from_this();
        api_.execute_gcode(
            command, [self, next]() { (self.get()->*next)(); },
            [self, command](const MoonrakerError& err) {
                self->complete_error(std::string(command) + " failed: " + err.message);
            },
            MoonrakerAdvancedAPI::kCalibrationTimeoutMs);
    }

    void on_entry_done() {
        send_step(kCmdHoming, &AutoScrewsTiltCollector::on_homing_done);
    }

    void on_homing_done() {
        detect_plate();
    }

    /// The plate gate. DETECT_BED_PLATE PRESENCE=0 is an assertion, not a
    /// query, so the command's own outcome is the verdict: success means the
    /// sheet is off and the run proceeds to probing; the not-removed error
    /// means the sheet is on and the run refuses with instructions; any other
    /// error is a genuine detection failure and refuses too - an outcome we
    /// cannot classify is not evidence the sheet is off, and probing through
    /// it skews per-corner tilt by more than the adjustment tolerance.
    void detect_plate() {
        auto self = shared_from_this();
        api_.execute_gcode(
            kCmdDetectBedPlate,
            [self]() {
                self->send_step(kCmdProbeReferencePoints, &AutoScrewsTiltCollector::on_probe_done);
            },
            [self](const MoonrakerError& err) {
                if (plate_still_on_bed(err.message)) {
                    self->complete_error(
                        "Remove the PEI sheet from the bed, then start again: probing through "
                        "the sheet gives wrong results");
                    return;
                }
                self->complete_error(std::string(kCmdDetectBedPlate) + " failed: " + err.message);
            },
            MoonrakerAdvancedAPI::kCalibrationTimeoutMs);
    }

    void on_probe_done() {
        collect_results();
    }

    void collect_results() {
        auto self = shared_from_this();
        json params = {{"objects", json::object({{kModuleName, nullptr},
                                                 {"configfile", json::array({"settings"})}})}};
        client_.send_jsonrpc(
            "printer.objects.query", params,
            [self](const json& response) {
                AutoScrewsTiltResults results = results_from_query(response);
                if (!results.ok()) {
                    self->complete_error(results.error);
                    return;
                }
                self->complete_success(std::move(results.screws));
            },
            [self](const MoonrakerError& err) {
                self->complete_error(std::string("reading screw results failed: ") + err.message);
            });
    }

    void complete_success(std::vector<ScrewTiltResult> screws) {
        if (finished_.exchange(true)) {
            return;
        }
        spdlog::info("[AutoScrewsTiltCollector] Complete with {} screws", screws.size());
        // Release the firmware state (restores IDLE, lifts Z). The exit is
        // only ISSUED here: its state query resolves a round trip after
        // on_success_ renders, so the panel never waits on it. What this
        // guarantees is that the state is released, not that it is released
        // before the results appear.
        request_exit(client_);
        if (on_success_) {
            on_success_(screws);
        }
    }

    void complete_error(const std::string& message) {
        if (finished_.exchange(true)) {
            return;
        }
        spdlog::error("[AutoScrewsTiltCollector] Error: {}", message);
        request_exit(client_);
        if (on_error_) {
            on_error_(MoonrakerError::json_rpc_error(kModuleName, message));
        }
    }

    IMoonrakerClient& client_;
    IMoonrakerAPI& api_;
    ScrewTiltCallback on_success_;
    MoonrakerAdvancedAPI::ErrorCallback on_error_;
    std::atomic<bool> finished_{false};
};

void start_sequence(IMoonrakerClient& client, IMoonrakerAPI& api, ScrewTiltCallback on_success,
                    helix::SharedCallback<void(const MoonrakerError&)> on_error) {
    std::make_shared<AutoScrewsTiltCollector>(client, api, std::move(on_success),
                                              std::move(on_error))
        ->start();
}

} // namespace screws_tilt
} // namespace snapmaker

} // namespace helix
