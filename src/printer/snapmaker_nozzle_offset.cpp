// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_nozzle_offset.h"

#include "connection_state.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "printer_discovery.h"
#include "snapmaker_exceptions.h"
#include "snapmaker_screws_tilt.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace helix::snapmaker::nozzle_offset {

namespace {

/// The macro each per-head step runs; the module refuses a head whose macro is missing.
constexpr std::array<const char*, 5> kPerHeadHooks = {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN",
                                                      "WAIT_COOL", "PROBE_CALIBRATE"};

/// Runners in this process that are mid-run.
std::atomic<int> s_live_runs{0};

/// What a run here left behind on a printer.
enum class ExitLeft { NeverSent, SentUnconfirmed };

/// The printer reconcile_on_connect() last named (its URL), and the printer a
/// run here left its EXIT outstanding on. Replies land on the WebSocket thread.
std::mutex s_pending_mutex;
std::string s_printer;
std::optional<std::string> s_pending_for;
ExitLeft s_pending_kind = ExitLeft::NeverSent;

/// The Runner most recently gone live. Main thread only.
Runner* s_live_runner = nullptr;

/// leave_live_run()'s wait for EXIT's reply. Main thread only.
uint32_t s_teardown_exit_wait_ms = kTeardownExitWaitMs;

bool phase_is_live(Runner::Phase phase) {
    return phase == Runner::Phase::Running || phase == Runner::Phase::AwaitingUser ||
           phase == Runner::Phase::Finishing;
}

/// The status map inside an objects.query response, or the response itself
/// when it already is one.
const nlohmann::json* status_map(const nlohmann::json& response) {
    const nlohmann::json* status = &response;
    if (const auto result = response.find("result");
        result != response.end() && result->is_object()) {
        status = &*result;
    }
    if (const auto inner = status->find("status"); inner != status->end() && inner->is_object()) {
        status = &*inner;
    }
    return status->is_object() ? status : nullptr;
}

/// The status object Klipper names each head's extruder by.
std::string extruder_object(int head) {
    return head == 0 ? std::string("extruder") : fmt::format("extruder{}", head);
}

} // namespace

bool supported(const helix::PrinterDiscovery& discovery) {
    if (!discovery.has_macro("_EXTRUDER_OFFSET_ACTION_PRESTART") ||
        !discovery.has_macro("_EXTRUDER_OFFSET_ACTION_EXIT")) {
        return false;
    }
    for (int head = 0; head < kHeads; ++head) {
        for (const char* hook : kPerHeadHooks) {
            if (!discovery.has_macro(fmt::format("_EXTRUDER_OFFSET_ACTION_{}_T{}", hook, head))) {
                return false;
            }
        }
    }
    return true;
}

std::vector<Step> build_sequence() {
    std::vector<Step> steps;
    steps.push_back({StepKind::Prestart, -1, "EXTRUDER_OFFSET_ACTION_PRESTART", Prompt::None});
    for (int t = 0; t < kHeads; ++t) {
        steps.push_back({StepKind::Heat, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_HEAT TOOL_ID=T{}", t), Prompt::None});
        steps.push_back(
            {StepKind::AutoClean, t,
             fmt::format("EXTRUDER_OFFSET_ACTION_AUTO_CLEAN TOOL_ID=T{} SAFE_MOVE_Y_POS={}", t,
                         kSafeMoveYPos),
             Prompt::None});
        steps.push_back({StepKind::ManualClean, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_MANUAL_CLEAN TOOL_ID=T{}", t),
                         Prompt::None});
        // MANUAL_CLEAN parks the hot nozzle at the front; the user brushes it
        // before it cools to probing temperature.
        steps.push_back(
            {StepKind::WaitCool, t,
             fmt::format("EXTRUDER_OFFSET_ACTION_WAIT_COOL TOOL_ID=T{} TEMP={}", t, kProbeTempC),
             Prompt::CleanNozzle});
    }
    steps.push_back({StepKind::DetectPlate, -1, "EXTRUDER_OFFSET_ACTION_DETECT_PLATE PRESENCE=0",
                     Prompt::RemovePlate});
    for (int t = 0; t < kHeads; ++t) {
        steps.push_back({StepKind::Probe, t,
                         fmt::format("EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE TOOL_ID=T{}", t),
                         Prompt::None});
    }
    return steps;
}

std::string failure_text(const std::string& error_message) {
    if (const auto code = decode_exception_code(error_message)) {
        const auto ours = exception_message(*code);
        if (!ours.empty()) {
            return std::string(ours);
        }
    }
    const auto parsed = nlohmann::json::parse(error_message, nullptr, false);
    if (parsed.is_object()) {
        const auto msg = parsed.find("msg");
        if (msg != parsed.end() && msg->is_string() && !msg->get<std::string>().empty()) {
            return msg->get<std::string>();
        }
    }
    return error_message;
}

nlohmann::json offsets_query_params() {
    nlohmann::json objects = nlohmann::json::object();
    for (int head = 0; head < kHeads; ++head) {
        objects[extruder_object(head)] = nlohmann::json::array({"extruder_offset"});
    }
    return {{"objects", objects}};
}

std::optional<std::vector<std::array<double, 3>>>
offsets_from_query(const nlohmann::json& response) {
    const nlohmann::json* status = status_map(response);
    if (!status) {
        return std::nullopt;
    }

    std::vector<std::array<double, 3>> out;
    for (int head = 0; head < kHeads; ++head) {
        const auto obj = status->find(extruder_object(head));
        if (obj == status->end() || !obj->is_object()) {
            return std::nullopt;
        }
        const auto offset = obj->find("extruder_offset");
        if (offset == obj->end() || !offset->is_array() || offset->size() < 3) {
            return std::nullopt;
        }
        std::array<double, 3> xyz{};
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!(*offset)[axis].is_number()) {
                return std::nullopt;
            }
            xyz[axis] = (*offset)[axis].get<double>();
        }
        out.push_back(xyz);
    }
    return out;
}

bool run_live() {
    return s_live_runs.load() > 0;
}

HeldState held_state_action(const nlohmann::json& response, bool live, bool pending) {
    const nlohmann::json* status = status_map(response);
    if (!status || live) {
        return HeldState::None;
    }
    const auto held = screws_tilt::main_state_from_status(*status);
    if (!held || *held != kMainStateXyzOffsetCalibrate) {
        return HeldState::None;
    }
    return pending ? HeldState::Exit : HeldState::AskUser;
}

void mark_exit_pending() {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    s_pending_for = s_printer;
    s_pending_kind = ExitLeft::NeverSent;
}

bool exit_pending() {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    return s_pending_for && *s_pending_for == s_printer && s_pending_kind == ExitLeft::NeverSent;
}

void mark_exit_unconfirmed() {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    s_pending_for = s_printer;
    s_pending_kind = ExitLeft::SentUnconfirmed;
}

bool exit_unconfirmed() {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    return s_pending_for && *s_pending_for == s_printer &&
           s_pending_kind == ExitLeft::SentUnconfirmed;
}

void clear_exit_pending() {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    s_pending_for.reset();
}

void send_exit(helix::IMoonrakerClient& client) {
    client.send_jsonrpc(
        "printer.gcode.script", {{"script", kExitGcode}},
        [](const nlohmann::json&) {
            clear_exit_pending();
            spdlog::info("[NozzleOffsetCal] Calibration state cleared");
        },
        [](const MoonrakerError& err) {
            mark_exit_pending();
            spdlog::warn("[NozzleOffsetCal] {} failed: {}", kExitGcode, err.message);
        },
        kStepTimeoutMs, true);
}

void send_probe_abort(helix::IMoonrakerClient& client) {
    client.send_jsonrpc(
        kProbeAbortMethod, nlohmann::json::object(),
        [](const nlohmann::json&) { spdlog::info("[NozzleOffsetCal] Probe pass aborted"); },
        [](const MoonrakerError& err) {
            spdlog::warn("[NozzleOffsetCal] Probe abort failed: {}", err.message);
        },
        0, true);
}

void reconcile_on_connect(helix::IMoonrakerClient& client, std::function<void()> ask_user) {
    {
        std::lock_guard<std::mutex> lock(s_pending_mutex);
        s_printer = client.get_last_url();
    }
    nlohmann::json params = {
        {"objects", {{"machine_state_manager", nlohmann::json::array({"main_state"})}}}};
    client.send_jsonrpc(
        "printer.objects.query", params,
        [&client, ask_user = std::move(ask_user)](const nlohmann::json& response) {
            // Decided at answer time: a run started since the query went out owns the state.
            const nlohmann::json* status = status_map(response);
            const auto main_state =
                status ? screws_tilt::main_state_from_status(*status) : std::nullopt;
            if (main_state && *main_state != kMainStateXyzOffsetCalibrate) {
                clear_exit_pending();
            }
            switch (held_state_action(response, run_live(), exit_pending())) {
            case HeldState::None:
                return;
            case HeldState::Exit:
                spdlog::info("[NozzleOffsetCal] Ending the calibration state a run here left");
                send_exit(client);
                return;
            case HeldState::AskUser:
                spdlog::info("[NozzleOffsetCal] A calibration state no run here owns is held{}",
                             exit_unconfirmed() ? " (our exit went out unconfirmed)" : "");
                if (ask_user) {
                    ask_user();
                }
                return;
            }
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[NozzleOffsetCal] Connect-time state read failed: {}", err.message);
        },
        0, true);
}

namespace {

/// Sends EXIT and blocks until its reply or @p timeout_ms. A send queued right
/// before disconnect() can sit in libhv's loop and never reach the socket; the
/// reply proves Klipper got it. Bounded, so a dead link cannot hold teardown.
bool send_exit_and_wait(helix::IMoonrakerClient& client, uint32_t timeout_ms) {
    struct Reply {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto reply = std::make_shared<Reply>();
    const auto finish = [reply] {
        {
            std::lock_guard<std::mutex> lock(reply->mutex);
            reply->done = true;
        }
        reply->cv.notify_all();
    };
    client.send_jsonrpc(
        "printer.gcode.script", {{"script", kExitGcode}},
        [finish](const nlohmann::json&) {
            clear_exit_pending();
            spdlog::info("[NozzleOffsetCal] Calibration state cleared");
            finish();
        },
        [finish](const MoonrakerError& err) {
            // Sent: a disconnect failing it does not say the firmware never ran it.
            mark_exit_unconfirmed();
            spdlog::warn("[NozzleOffsetCal] {} failed: {}", kExitGcode, err.message);
            finish();
        },
        kStepTimeoutMs, true);
    std::unique_lock<std::mutex> lock(reply->mutex);
    return reply->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&reply] { return reply->done; });
}

} // namespace

void set_teardown_exit_wait_ms(uint32_t ms) {
    s_teardown_exit_wait_ms = ms;
}

void leave_live_run(helix::IMoonrakerClient* client) {
    Runner* runner = s_live_runner;
    // Finishing already has its EXIT in flight.
    if (!runner || (runner->phase() != Runner::Phase::Running &&
                    runner->phase() != Runner::Phase::AwaitingUser)) {
        return;
    }
    const bool probing = runner->probing();
    runner->abandon();
    if (!client || client->get_connection_state() != helix::ConnectionState::CONNECTED) {
        spdlog::warn("[NozzleOffsetCal] Leaving a run with no connection; the calibration "
                     "state stays held");
        mark_exit_pending();
        return;
    }
    spdlog::info("[NozzleOffsetCal] Leaving a run before disconnect; exiting the calibration "
                 "state");
    if (probing) {
        send_probe_abort(*client);
    }
    if (!send_exit_and_wait(*client, s_teardown_exit_wait_ms)) {
        spdlog::warn("[NozzleOffsetCal] No reply to {} within {}ms", kExitGcode,
                     s_teardown_exit_wait_ms);
        mark_exit_unconfirmed();
    }
}

// ============================================================================
// Runner
// ============================================================================

Runner::Runner(Hooks hooks) : hooks_(std::move(hooks)), steps_(build_sequence()) {}

Runner::~Runner() {
    if (phase_is_live(phase_)) {
        s_live_runs.fetch_sub(1);
    }
    if (s_live_runner == this) {
        s_live_runner = nullptr;
    }
}

const Step* Runner::current_step() const {
    if (phase_ == Phase::Idle || index_ >= steps_.size()) {
        return nullptr;
    }
    return &steps_[index_];
}

Prompt Runner::prompt() const {
    if (phase_ != Phase::AwaitingUser || index_ >= steps_.size()) {
        return Prompt::None;
    }
    return steps_[index_].prompt_before;
}

void Runner::start() {
    if (phase_ == Phase::Running || phase_ == Phase::AwaitingUser || phase_ == Phase::Finishing) {
        return;
    }
    if (hooks_.start_refusal) {
        std::string refused = hooks_.start_refusal();
        if (!refused.empty()) {
            spdlog::warn("[NozzleOffsetCal] Not starting: {}", refused);
            error_ = std::move(refused);
            set_phase(Phase::Failed);
            return;
        }
    }
    ++run_;
    index_ = 0;
    error_.clear();
    // This run owns the state from here, whatever an earlier one left.
    clear_exit_pending();
    spdlog::info("[NozzleOffsetCal] Starting a {}-step run", steps_.size());
    run_current();
}

void Runner::confirm() {
    if (phase_ != Phase::AwaitingUser) {
        return;
    }
    run_current();
}

void Runner::cancel() {
    if (phase_ != Phase::Running && phase_ != Phase::AwaitingUser) {
        return;
    }
    ++run_;
    // A probe pass holds Klipper's command queue until it ends, so EXIT would
    // wait behind it without the abort.
    if (probing() && hooks_.abort_probe) {
        hooks_.abort_probe();
    }
    spdlog::info("[NozzleOffsetCal] Cancelled at step {}", index_);
    send_exit(Phase::Cancelled);
}

void Runner::abandon() {
    if (phase_ != Phase::Running && phase_ != Phase::AwaitingUser) {
        return;
    }
    ++run_;
    spdlog::info("[NozzleOffsetCal] Run abandoned at step {}", index_);
    set_phase(Phase::Cancelled);
}

bool Runner::probing() const {
    return phase_ == Phase::Running && index_ < steps_.size() &&
           steps_[index_].kind == StepKind::Probe;
}

void Runner::reset() {
    if (phase_ != Phase::Done && phase_ != Phase::Failed && phase_ != Phase::Cancelled) {
        return;
    }
    index_ = 0;
    error_.clear();
    set_phase(Phase::Idle);
}

void Runner::run_current() {
    const Step& step = steps_[index_];
    if (step.prompt_before != Prompt::None && phase_ != Phase::AwaitingUser) {
        set_phase(Phase::AwaitingUser);
        return;
    }
    set_phase(Phase::Running);
    spdlog::debug("[NozzleOffsetCal] Step {}: {}", index_, step.gcode);
    const uint64_t run = run_;
    hooks_.send(step.gcode, kStepTimeoutMs,
                [this, run](bool ok, const std::string& error) { on_step_done(run, ok, error); });
}

void Runner::on_step_done(uint64_t run, bool ok, const std::string& error) {
    if (run != run_ || phase_ != Phase::Running) {
        return;
    }
    const Step& step = steps_[index_];
    if (!ok) {
        error_ = failure_text(error);
        spdlog::warn("[NozzleOffsetCal] {} failed: {}", step.gcode, error);
        if (step.kind == StepKind::DetectPlate && screws_tilt::plate_still_on_bed(error)) {
            set_phase(Phase::AwaitingUser);
            return;
        }
        ++run_;
        send_exit(Phase::Failed);
        return;
    }
    error_.clear();
    ++index_;
    if (index_ < steps_.size()) {
        run_current();
        return;
    }
    // The firmware saved every head's result inside the last probe step.
    spdlog::info("[NozzleOffsetCal] All heads calibrated; exiting the calibration state");
    send_exit(Phase::Finishing);
}

void Runner::send_exit(Phase then) {
    set_phase(then);
    const uint64_t run = run_;
    hooks_.send(kExitGcode, kStepTimeoutMs, [this, run](bool ok, const std::string& error) {
        if (ok) {
            clear_exit_pending();
        } else {
            mark_exit_pending();
            spdlog::warn("[NozzleOffsetCal] {} failed: {}", kExitGcode, error);
        }
        if (run == run_ && phase_ == Phase::Finishing) {
            set_phase(Phase::Done);
        }
    });
}

void Runner::set_phase(Phase phase) {
    const bool was_live = phase_is_live(phase_);
    const bool is_live = phase_is_live(phase);
    if (is_live != was_live) {
        s_live_runs.fetch_add(is_live ? 1 : -1);
    }
    if (is_live) {
        s_live_runner = this;
    } else if (s_live_runner == this) {
        s_live_runner = nullptr;
    }
    phase_ = phase;
    // leave_live_run() sends EXIT from these two; Finishing's is already in flight.
    heater_hold_.set(phase == Phase::Running || phase == Phase::AwaitingUser);
    if (hooks_.changed) {
        hooks_.changed();
    }
}

} // namespace helix::snapmaker::nozzle_offset
