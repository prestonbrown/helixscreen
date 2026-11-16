// SPDX-License-Identifier: GPL-3.0-or-later

// MedusaHC mock hardware (HELIX_MOCK_AMS=medusahc and its -ktc and -tn variants):
// the hotend changer's status, macro state and pin_watch docks, with a swap
// advanced phase by phase from the simulation thread.

#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

using namespace helix;
using mock_internal::effective_mock_ams_env;

MoonrakerClientMock::MedusaVariant MoonrakerClientMock::mock_medusa_variant() {
    return helix::mock::medusa_variant(effective_mock_ams_env());
}

bool MoonrakerClientMock::is_mock_medusahc() const {
    return mock_medusa_variant() != MedusaVariant::NONE;
}

nlohmann::json MoonrakerClientMock::medusa_config_sections() const {
    json sections = json::object();
    if (!is_mock_medusahc()) {
        return sections;
    }
    sections["gcode_macro resume"] = {
        {"rename_existing", "BASE_RESUME"},
        {"gcode", "{% if printer.pause_resume.is_paused %}\n"
                  "    {% set tgt = printer[\"gcode_macro _GLOBAL_STATE\"].target_tool|int %}\n"
                  "    MHC_RESUME T={tgt}\n"
                  "{% else %}\n"
                  "    RESPOND MSG=\"RESUME (no print, passthrough)\"\n"
                  "{% endif %}"}};
    return sections;
}

namespace {
// Notifications a single swap phase lasts. One notification is
// kNotificationIntervalTicks * kSimulationIntervalMs = ~1s, so a full
// drop-then-pick swap runs ~7s: slow enough to watch the step bar move,
// fast enough not to be a wait.
constexpr int kMedusaPhaseNotifications = 3;
// The homing and Z hop MHC_SET runs before it reads the docks.
constexpr int kMedusaChangingNotifications = 1;

/// pin_watch.py's _compute_current_tool(): -1 when the toolhead switch is
/// released and every dock is seated, the one vacant dock's index when the
/// toolhead switch is pressed, and -2 for anything else.
int pin_watch_current_tool(bool head,
                           const std::array<bool, MoonrakerClientMock::kMedusaMaxTools>& docks,
                           int count) {
    int seated = 0;
    int vacant = -1;
    for (int i = 0; i < count; ++i) {
        if (docks[static_cast<size_t>(i)]) {
            ++seated;
        } else {
            vacant = i;
        }
    }
    if (!head && seated == count) {
        return -1;
    }
    if (head && seated == count - 1) {
        return vacant;
    }
    return -2;
}
} // namespace

const char* MoonrakerClientMock::medusa_operation_word(MedusaPhase phase) {
    switch (phase) {
    case MedusaPhase::Changing:
        return "changing";
    case MedusaPhase::Dropping:
        return "dropping";
    case MedusaPhase::Picking:
        return "picking";
    case MedusaPhase::Idle:
        break;
    }
    return "idle";
}

int MoonrakerClientMock::medusa_tool_count() {
    return mock_medusa_variant() == MedusaVariant::CONTROLLER_TN ? kMedusaMaxTools : 4;
}

int MoonrakerClientMock::medusa_pin_tool_locked() const {
    const int count = medusa_tool_count();
    std::array<bool, kMedusaMaxTools> docks{};
    for (int i = 0; i < count; ++i) {
        docks[static_cast<size_t>(i)] = i != medusa_.carriage_tool || i == medusa_.stuck_dock;
    }
    return pin_watch_current_tool(medusa_.carriage_tool >= 0, docks, count);
}

int MoonrakerClientMock::sync_medusa_toolchanger_tool() {
    const MockPrintPhase phase = print_phase_.load();
    const bool printing = phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT;
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    const int reading = medusa_pin_tool_locked();
    // pin_watch syncs when the pins change, so a print paused after they did
    // does not reach a reading taken before it.
    if (medusa_.toolchanger_synced && reading == medusa_.toolchanger_reading) {
        return medusa_.toolchanger_tool;
    }
    medusa_.toolchanger_synced = true;
    medusa_.toolchanger_reading = reading;
    // INITIALIZE_TOOLCHANGER T=<n> for a named tool, UNSELECT_TOOL otherwise,
    // and never an unselect mid-print.
    if (reading >= 0) {
        medusa_.toolchanger_tool = reading;
    } else if (!printing) {
        medusa_.toolchanger_tool = -1;
    }
    return medusa_.toolchanger_tool;
}

bool MoonrakerClientMock::take_medusa_swap_armed() {
    if (!is_mock_medusahc()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    return std::exchange(medusa_.swap_armed, false);
}

nlohmann::json MoonrakerClientMock::medusa_status_json() const {
    const MedusaVariant variant = mock_medusa_variant();
    if (variant == MedusaVariant::NONE) {
        return nlohmann::json::object();
    }

    std::lock_guard<std::mutex> lock(medusa_mutex_);
    const MedusaSim& m = medusa_;
    nlohmann::json obj;
    const int count = medusa_tool_count();
    obj["tool_count"] = count;
    obj["target_tool"] = m.target_tool;
    obj["feeder_open"] = m.feeder_open;
    obj["layer"] = m.layer;

    // Irbis3D medusahc.py get_status(). current_tool is pin_watch's, live
    // from the pins; sensor_error is the operation latch, a separate signal.
    obj["operation"] = medusa_operation_word(m.phase);
    obj["current_tool"] = medusa_pin_tool_locked();
    obj["last_error"] = m.last_error;
    obj["sensor_error"] = m.op_failed;
    // pin_watch's raw pins: "e" the toolhead, "t<n>" each dock.
    nlohmann::json sensors;
    sensors["e"] = (m.carriage_tool >= 0) ? 1 : 0;
    for (int i = 0; i < count; ++i) {
        sensors["t" + std::to_string(i)] = (i != m.carriage_tool || i == m.stuck_dock) ? 1 : 0;
    }
    obj["sensors"] = sensors;
    return obj;
}

nlohmann::json MoonrakerClientMock::medusa_macro_status_json() const {
    if (mock_medusa_variant() == MedusaVariant::NONE) {
        return nlohmann::json::object();
    }
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    const MedusaSim& m = medusa_;
    const int count = medusa_tool_count();
    nlohmann::json offsets = nlohmann::json::object();
    for (int t = 0; t < count; ++t) {
        for (int a = 0; a < 3; ++a) {
            offsets["t" + std::to_string(t) + "_off_" + "xyz"[a]] =
                m.offsets[static_cast<size_t>(t)][static_cast<size_t>(a)];
        }
    }
    return nlohmann::json{
        {"gcode_macro _TOOL_OFFSET", offsets},
        {"gcode_macro _GLOBAL_STATE",
         {{"max_tool", count},
          {"layer", m.layer},
          {"target_tool", m.global_target_tool},
          {"error_state", m.error_state},
          {"feeder_open", m.feeder_open ? 1 : 0}}},
        {"save_variables", {{"variables", m.saved_variables}}},
    };
}

double MoonrakerClientMock::medusa_nozzle_temp(int tool) const {
    return tool == 0 ? extruder_temp_.load() : secondary_extruder_temp(tool);
}

void MoonrakerClientMock::start_medusa_swap(int tool, bool close_after) {
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    MedusaSim& m = medusa_;
    // The controller's _begin(): a new operation clears the last one's latch.
    m.op_failed = false;
    m.last_error.clear();
    m.redock_ticks = -1;
    m.pick_tool = tool;
    m.close_after_drop = close_after;
    if (tool >= 0) {
        // MHC_SET homes and hops Z as "changing" before it reads the docks.
        m.target_tool = tool;
        m.global_target_tool = tool;
        m.error_state = 0;
        m.phase = MedusaPhase::Changing;
        m.phase_ticks = kMedusaChangingNotifications;
        m.swap_armed = true;
    } else {
        // MHC_DROP targets the tool it parks, and an empty head has none.
        const int reading = medusa_pin_tool_locked();
        m.target_tool = reading;
        if (reading >= 0) {
            m.global_target_tool = reading;
        }
        if (reading == -1) {
            if (close_after) {
                m.feeder_open = false;
            }
            spdlog::info("[MoonrakerClientMock] MedusaHC unmount: nothing installed");
            return;
        }
        // The gripper releases at the dock, once the docks have named the tool.
        m.feeder_open = m.feeder_open || reading >= 0;
        m.phase = MedusaPhase::Dropping;
        m.phase_ticks = kMedusaPhaseNotifications;
        m.swap_armed = true;
    }
    spdlog::info("[MoonrakerClientMock] MedusaHC {} armed: carriage T{} -> T{}",
                 tool >= 0 ? "swap" : "unmount", m.carriage_tool, tool);
}

void MoonrakerClientMock::medusa_fail_locked(std::string message) {
    spdlog::info("[MoonrakerClientMock] MedusaHC operation failed: {}", message);
    medusa_.op_failed = true;
    medusa_.last_error = std::move(message);
    medusa_.phase = MedusaPhase::Idle;
    medusa_.phase_ticks = 0;
    medusa_.redock_ticks = medusa_redock_notifications_;
}

void MoonrakerClientMock::medusa_place_tools_by_hand(int tool) {
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    medusa_.carriage_tool = tool;
    medusa_.stuck_dock = -1;
    medusa_.redock_ticks = -1;
    spdlog::info("[MoonrakerClientMock] MedusaHC tools placed by hand: carriage T{}", tool);
}

void MoonrakerClientMock::advance_medusa_swap() {
    bool failed = false;
    std::optional<MedusaReply> reply;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(medusa_mutex_);
        MedusaSim& m = medusa_;
        if (m.phase == MedusaPhase::Idle) {
            if (m.redock_ticks > 0 && --m.redock_ticks == 0) {
                // The user lifts the hot end off the carriage into its dock.
                m.carriage_tool = -1;
                m.stuck_dock = -1;
                m.redock_ticks = -1;
                spdlog::info("[MoonrakerClientMock] MedusaHC re-docked by hand");
            }
            return;
        }
        if (--m.phase_ticks > 0) {
            return; // still inside this phase
        }

        // What the controller reads from the docks once the phase's motion ends.
        const int reading = medusa_pin_tool_locked();
        switch (m.phase) {
        case MedusaPhase::Changing:
            if (reading == -2) {
                medusa_fail_locked("MHC_SET: ambiguous sensor state");
                failed = true;
            } else if (reading == m.pick_tool) {
                m.phase = MedusaPhase::Idle;
                spdlog::info("[MoonrakerClientMock] MedusaHC T{} already installed", reading);
            } else {
                // The gripper releases at the dock, before the latch moves.
                m.feeder_open = true;
                m.phase = reading >= 0 ? MedusaPhase::Dropping : MedusaPhase::Picking;
                m.phase_ticks = kMedusaPhaseNotifications;
            }
            break;
        case MedusaPhase::Dropping:
            if (reading == -2) {
                medusa_fail_locked("MHC_DROP: ambiguous sensor state");
                failed = true;
                break;
            }
            m.feeder_open = true;
            m.carriage_tool = -1;
            if (m.pick_tool >= 0) {
                m.phase = MedusaPhase::Picking;
                m.phase_ticks = kMedusaPhaseNotifications;
                spdlog::debug("[MoonrakerClientMock] MedusaHC drop complete, picking T{}",
                              m.pick_tool);
                break;
            }
            // MHC_DROP leaves the gripper open; DROP_TOOL closes it after.
            m.feeder_open = !m.close_after_drop;
            m.phase = MedusaPhase::Idle;
            spdlog::info("[MoonrakerClientMock] MedusaHC unmount complete");
            break;
        case MedusaPhase::Picking:
            m.carriage_tool = m.pick_tool;
            if (m.pick_tool == medusa_swap_fail_tool_) {
                // The hot end is on the carriage but its dock switch still reads
                // seated, so the sensors never confirm it; _fail() leaves the
                // gripper open.
                m.stuck_dock = m.pick_tool;
                medusa_fail_locked(
                    fmt::format("MHC_SET: sensors did not confirm T{}", m.pick_tool));
                failed = true;
                break;
            }
            m.feeder_open = false;
            m.phase = MedusaPhase::Idle;
            spdlog::info("[MoonrakerClientMock] MedusaHC swap complete: T{} on head", m.pick_tool);
            break;
        case MedusaPhase::Idle:
            break;
        }
        if (m.phase == MedusaPhase::Idle) {
            reply = std::exchange(medusa_reply_, std::nullopt);
            message = m.last_error;
        }
    }
    if (mock_medusa_variant() == MedusaVariant::CONTROLLER_KTC) {
        sync_medusa_toolchanger_tool();
    }
    // MHC_ERROR flags the failed change while a print is active and pauses a
    // running one; MHC_RESUME's swap continues the print once it succeeds.
    const MockPrintPhase phase = print_phase_.load();
    const bool print_running =
        phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT;
    bool resume_now = false;
    {
        std::lock_guard<std::mutex> lock(medusa_mutex_);
        if (failed && (print_running || phase == MockPrintPhase::PAUSED)) {
            medusa_.error_state = 1;
        }
        if (medusa_.phase == MedusaPhase::Idle && medusa_.resume_after_swap) {
            medusa_.resume_after_swap = false;
            resume_now = !failed;
        }
    }
    if (failed && print_running) {
        pause_print_internal();
    }
    if (resume_now) {
        resume_print_internal();
    }
    if (reply) {
        // _fail() raises _OperationPaused while print_stats reads printing or
        // paused, and the command handler swallows that one.
        const bool print_active = phase == MockPrintPhase::PRINTING ||
                                  phase == MockPrintPhase::PREHEAT ||
                                  phase == MockPrintPhase::PAUSED;
        medusa_reply_lifetime_.token().defer(
            "MoonrakerClientMock::medusa_reply",
            [this, reply = std::move(*reply), failed, print_active, message]() mutable {
                complete_medusa_reply(std::move(reply), failed, print_active, message);
            });
    }
}

void MoonrakerClientMock::hold_medusa_reply(
    std::string script, size_t resume_at, helix::rpc_error_policy::CallerIntent intent,
    helix::SharedCallback<void(const nlohmann::json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    MedusaReply reply{std::move(script), resume_at, intent, std::move(success_cb),
                      std::move(error_cb)};
    bool failed = false;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(medusa_mutex_);
        if (medusa_.phase != MedusaPhase::Idle) {
            medusa_reply_ = std::move(reply);
            return;
        }
        // The simulation thread finished the swap before its reply was held.
        failed = medusa_.op_failed;
        message = medusa_.last_error;
    }
    const MockPrintPhase phase = print_phase_.load();
    complete_medusa_reply(std::move(reply), failed,
                          phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT ||
                              phase == MockPrintPhase::PAUSED,
                          message);
}

void MoonrakerClientMock::complete_medusa_reply(MedusaReply reply, bool failed, bool print_active,
                                                const std::string& message) {
    spdlog::debug("[MoonrakerClientMock] MedusaHC swap reply: {}",
                  !failed        ? "ok"
                  : print_active ? "ok, print paused: " + message
                                 : message);
    if (failed && !print_active) {
        // printer.command_error: the script ends at the swap.
        GcodeScriptRun run;
        run.result = 1;
        run.error = message;
        answer_gcode_script(reply.script, run, reply.intent, reply.success_cb, reply.error_cb);
        return;
    }
    if (failed) {
        dispatch_gcode_response("// MedusaHC paused: " + message);
    }
    GcodeScriptRun run = run_gcode_script_lines(reply.script, reply.resume_at);
    if (run.result == 0 && run.resume_at != std::string::npos) {
        hold_medusa_reply(std::move(reply.script), run.resume_at, reply.intent,
                          std::move(reply.success_cb), std::move(reply.error_cb));
        return;
    }
    answer_gcode_script(reply.script, run, reply.intent, reply.success_cb, reply.error_cb);
}

nlohmann::json MoonrakerClientMock::pin_watch_status_json() const {
    const MedusaVariant variant = mock_medusa_variant();
    if (variant == MedusaVariant::NONE) {
        return nlohmann::json::object();
    }
    std::lock_guard<std::mutex> lock(medusa_mutex_);
    return nlohmann::json{{"current_tool", medusa_pin_tool_locked()}};
}
