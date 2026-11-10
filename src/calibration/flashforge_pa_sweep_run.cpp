// SPDX-License-Identifier: GPL-3.0-or-later

// The driver for flashforge::pa_sweep::Sweep: sends each step as one
// gcode/script call, reads PA_GET off the console, and owns the cleanup.

#include "cleanup_hold.h"
#include "core/update_queue.h"
#include "flashforge_pa_sweep.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "i_moonraker_sub_apis.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>
#include <optional>

#include "hv/json.hpp"

namespace helix::flashforge::pa_sweep {
namespace {

using nlohmann::json;

/// Each step is a few seconds of motion; the home is the long one.
constexpr uint32_t kStepTimeoutMs = IAdvancedAPI::kProbingTimeoutMs;

const json* query_status(const json& response) {
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

const json* member(const json* obj, const char* key) {
    if (!obj || !obj->is_object()) {
        return nullptr;
    }
    const auto it = obj->find(key);
    return it == obj->end() ? nullptr : &*it;
}

/// Element `i` of a numeric array member, or nullopt.
std::optional<float> array_at(const json* obj, const char* key, size_t i) {
    const json* arr = member(obj, key);
    if (!arr || !arr->is_array() || arr->size() <= i || !(*arr)[i].is_number()) {
        return std::nullopt;
    }
    return (*arr)[i].get<float>();
}

std::string failure_message(const Sweep& sweep) {
    switch (sweep.failure()) {
    case Failure::TooFewWinners:
        return "The printer passed too few test lines to settle on a value. Check that the "
               "nozzle is clean and filament is loaded, then try again.";
    case Failure::NoDiscrimination:
        return "The printer's sensor gave every test line the same verdict, so it is not "
               "telling the candidates apart. No value was set.";
    case Failure::Error:
        return sweep.error();
    case Failure::Stopped: // a stopped run is silenced, so this is never shown
    case Failure::None:
        break;
    }
    return "The measurement ended without a result.";
}

class SweepRun final : public pacal::Run, public std::enable_shared_from_this<SweepRun> {
  public:
    SweepRun(IMoonrakerAPI& api, IMoonrakerClient& client, pacal::RunRequest req,
             pacal::RunCallbacks cb)
        : api_(api), client_(client), req_(std::move(req)), cb_(std::move(cb)) {}

    ~SweepRun() override {
        unregister();
    }

    /// Main thread.
    void start() {
        // From here until the restore has been sent, SIGTERM takes the
        // graceful path, whose teardown calls before_disconnect().
        hold_.set(true);
        static std::atomic<uint64_t> s_id{0};
        handler_ = "pa_sweep_" + std::to_string(++s_id);
        std::weak_ptr<SweepRun> weak = shared_from_this();
        client_.register_method_callback("notify_gcode_response", handler_,
                                         [weak](const json& msg) {
                                             if (auto self = weak.lock()) {
                                                 self->on_console(msg);
                                             }
                                         });
        registered_ = true;
        // From the next tick, so the caller holds the run before any reply can
        // arrive on its callbacks.
        auto self = shared_from_this();
        helix::ui::queue_update("PaSweep::start", [self]() { self->read_snapshot(); });
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lock(m_);
            silenced_ = true;
            if (!sweep_) {
                stopped_before_snapshot_ = true;
                return;
            }
            sweep_->stop();
            // A line whose reply came back is over even without its verdict.
            if (in_flight_ && line_rpc_done_) {
                in_flight_ = false;
            }
        }
        pump();
    }

    void before_disconnect() override {
        std::string script;
        {
            std::lock_guard<std::mutex> lock(m_);
            silenced_ = true;
            if (!sweep_ || sweep_->finished()) {
                return;
            }
            sweep_->stop();
            const Step st = sweep_->next();
            if (st.phase == Phase::Restore) {
                script = st.script;
            }
            disconnected_ = true;
        }
        if (!script.empty()) {
            spdlog::warn("[PaSweep] Link going down mid-run: sending the restore now");
            client_.gcode_script(script);
        }
        release_hold();
    }

  private:
    void read_snapshot() {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (stopped_before_snapshot_ || disconnected_) {
                finished_ = true;
                unregister();
                release_hold();
                return;
            }
        }
        json objects = {
            {"toolhead", json::array({"homed_axes", "max_accel", "square_corner_velocity"})},
            {"gcode_move", json::array({"gcode_position"})},
            {req_.extruder, json::array({"pressure_advance", "smooth_time"})},
        };
        auto self = shared_from_this();
        client_.send_jsonrpc(
            "printer.objects.query", json{{"objects", objects}},
            [self](const json& response) { self->on_snapshot(response); },
            [self](const MoonrakerError& err) {
                self->end_without_sweep("Could not read the printer's state: " + err.message);
            },
            0, true);
    }

    void on_snapshot(const json& response) {
        const json* status = query_status(response);
        const json* toolhead = member(status, "toolhead");
        const json* extruder = member(status, req_.extruder.c_str());
        if (!toolhead || !extruder) {
            end_without_sweep("The printer did not report its toolhead and extruder state.");
            return;
        }
        Snapshot snap;
        const std::string homed = json_util::safe_string(*toolhead, "homed_axes");
        snap.homed = homed.find('x') != std::string::npos && homed.find('y') != std::string::npos &&
                     homed.find('z') != std::string::npos;
        snap.max_accel = json_util::safe_float(*toolhead, "max_accel");
        snap.square_corner_velocity = json_util::safe_float(*toolhead, "square_corner_velocity");
        snap.pressure_advance = json_util::safe_float(*extruder, "pressure_advance");
        snap.smooth_time = json_util::safe_float(*extruder, "smooth_time");
        snap.gcode_z = array_at(member(status, "gcode_move"), "gcode_position", 2).value_or(0.f);
        if (snap.max_accel <= 0.f || snap.square_corner_velocity <= 0.f) {
            end_without_sweep("The printer did not report its motion limits.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            sweep_.emplace(Config{}, req_.tool_index, req_.extruder, snap);
            if (stopped_before_snapshot_) {
                sweep_->stop();
            }
        }
        pump();
    }

    /// Send whatever the sweep wants next, unless a step is still in flight.
    void pump() {
        Step st;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (!sweep_ || in_flight_ || disconnected_) {
                return;
            }
            st = sweep_->next();
            if (st.phase == Phase::Done) {
                // fall through to finish() outside the lock
            } else {
                in_flight_ = true;
                line_rpc_done_ = false;
                line_verdict_.reset();
                current_ = st;
            }
        }
        if (st.phase == Phase::Done) {
            finish();
            return;
        }
        if (st.phase == Phase::Restore) {
            // Once the restore is on its way the printer is owed nothing that
            // a SIGTERM could still deliver.
            release_hold();
        }
        if (st.phase == Phase::Locate) {
            locate();
            return;
        }
        send(st);
    }

    void send(const Step& st) {
        auto self = shared_from_this();
        spdlog::debug("[PaSweep] Sending step {} (candidate {})", static_cast<int>(st.phase),
                      st.candidate);
        api_.execute_gcode(
            st.script, [self]() { self->on_step_ok(); },
            [self](const MoonrakerError& err) { self->on_step_error(err.message); }, kStepTimeoutMs,
            /*silent=*/true,
            /*on_queued=*/
            [self]() { self->on_step_error("The printer is busy with another command."); });
    }

    void locate() {
        auto self = shared_from_this();
        client_.send_jsonrpc(
            "printer.objects.query", json{{"objects", {{"toolhead", json::array({"position"})}}}},
            [self](const json& response) {
                const json* toolhead = member(query_status(response), "toolhead");
                const auto x = array_at(toolhead, "position", 0);
                const auto y = array_at(toolhead, "position", 1);
                if (!x || !y) {
                    self->on_step_error("The printer did not report where the head parked.");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(self->m_);
                    self->in_flight_ = false;
                    self->sweep_->on_located(*x, *y);
                }
                self->pump();
            },
            [self](const MoonrakerError& err) {
                self->on_step_error("Could not read where the head parked: " + err.message);
            },
            0, true);
    }

    void on_step_ok() {
        std::optional<pacal::Attempt> attempt;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (!in_flight_) {
                return;
            }
            if (current_.phase == Phase::Line) {
                line_rpc_done_ = true;
                if (sweep_->failure() != Failure::None) {
                    in_flight_ = false; // stopped mid-line: the restore goes next
                } else if (!line_verdict_) {
                    return; // the PA_GET line has not arrived yet
                } else {
                    attempt = feed_verdict_locked(*line_verdict_);
                }
            } else {
                in_flight_ = false;
                sweep_->step_done();
            }
        }
        if (attempt) {
            report_attempt(*attempt);
        }
        pump();
    }

    void on_step_error(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(m_);
            spdlog::error("[PaSweep] Step {} failed: {}", static_cast<int>(current_.phase),
                          message);
            in_flight_ = false;
            if (sweep_) {
                sweep_->fail(message);
            }
        }
        pump();
    }

    void on_console(const json& msg) {
        const json* params = member(&msg, "params");
        if (!params || !params->is_array() || params->empty() || !(*params)[0].is_string()) {
            return;
        }
        const auto verdict = parse_pa_get((*params)[0].get_ref<const std::string&>());
        if (!verdict) {
            return;
        }
        std::optional<pacal::Attempt> attempt;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (!in_flight_ || current_.phase != Phase::Line || line_verdict_) {
                return;
            }
            line_verdict_ = *verdict;
            if (!line_rpc_done_) {
                return; // the script's own reply ends the line
            }
            attempt = feed_verdict_locked(*verdict);
        }
        if (attempt) {
            report_attempt(*attempt);
        }
        pump();
    }

    pacal::Attempt feed_verdict_locked(int verdict) {
        in_flight_ = false;
        const float k = sweep_->candidate_k(current_.candidate);
        sweep_->on_verdict(verdict);
        spdlog::info("[PaSweep] K={:.4f} -> verdict {}", k, verdict);
        Config cfg;
        return pacal::Attempt{sweep_->lines_done(), sweep_->expected_lines(), k,
                              verdict == cfg.pass_verdict ? pacal::Verdict::Pass
                                                          : pacal::Verdict::Fail};
    }

    void report_attempt(const pacal::Attempt& a) {
        if (silenced_.load() || !cb_.on_attempt) {
            return;
        }
        cb_.on_attempt(a);
    }

    void finish() {
        std::optional<float> k;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(m_);
            if (finished_) {
                return;
            }
            finished_ = true;
            k = sweep_->result();
            if (!k) {
                error = failure_message(*sweep_);
            }
        }
        unregister();
        release_hold();
        if (silenced_.load()) {
            spdlog::info("[PaSweep] Stopped; restore sent");
            return;
        }
        if (k) {
            spdlog::info("[PaSweep] Result K={:.4f}, applied for this session", *k);
            if (cb_.on_result) {
                cb_.on_result(*k);
            }
        } else {
            spdlog::warn("[PaSweep] No result: {}", error);
            if (cb_.on_error) {
                cb_.on_error(error);
            }
        }
    }

    void end_without_sweep(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (finished_) {
                return;
            }
            finished_ = true;
        }
        unregister();
        release_hold();
        if (!silenced_.load() && cb_.on_error) {
            cb_.on_error(message);
        }
    }

    void unregister() {
        if (registered_.exchange(false)) {
            client_.unregister_method_callback("notify_gcode_response", handler_);
        }
    }

    /// CleanupHold is set and cleared on the main thread only.
    void release_hold() {
        if (hold_released_.exchange(true)) {
            return;
        }
        auto self = shared_from_this();
        helix::ui::queue_update("PaSweep::release_hold", [self]() { self->hold_.set(false); });
    }

    IMoonrakerAPI& api_;
    IMoonrakerClient& client_;
    pacal::RunRequest req_;
    pacal::RunCallbacks cb_;
    CleanupHold hold_;
    std::atomic<bool> hold_released_{false};
    std::string handler_;
    std::atomic<bool> registered_{false};
    std::atomic<bool> silenced_{false};

    std::mutex m_;
    std::optional<Sweep> sweep_;
    Step current_;
    bool in_flight_ = false;
    bool line_rpc_done_ = false;
    std::optional<int> line_verdict_;
    bool finished_ = false;
    bool disconnected_ = false;
    bool stopped_before_snapshot_ = false;
};

} // namespace

std::shared_ptr<pacal::Run> start_run(IMoonrakerAPI& api, IMoonrakerClient& client,
                                      const pacal::RunRequest& req, pacal::RunCallbacks callbacks) {
    auto run = std::make_shared<SweepRun>(api, client, req, std::move(callbacks));
    run->start();
    return run;
}

} // namespace helix::flashforge::pa_sweep
