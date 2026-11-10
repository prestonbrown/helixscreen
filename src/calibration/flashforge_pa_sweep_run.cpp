// SPDX-License-Identifier: GPL-3.0-or-later

// The driver for flashforge::pa_sweep::Sweep: sends each step as one
// gcode/script call, reads PA_GET off the console, and owns the cleanup.

#include "cleanup_hold.h"
#include "core/run_after.h"
#include "core/update_queue.h"
#include "flashforge_pa_sweep.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "i_moonraker_sub_apis.h"
#include "json_utils.h"

#include <spdlog/fmt/fmt.h>
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
/// PA_GET's reply prints before the script returns; this long after the
/// script returned, it is not coming.
constexpr uint32_t kVerdictDeadlineMs = 5000;

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

float pin_value(const json* status, std::string_view object) {
    const json* pin = member(status, std::string(object).c_str());
    return pin && pin->is_object() ? json_util::safe_float(*pin, "value", 0.f) : 0.f;
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
            disconnected_ = true;
            if (!sweep_) {
                stopped_before_snapshot_ = true;
            } else {
                script = sweep_->force_restore();
            }
        }
        if (!script.empty()) {
            spdlog::warn("[PaSweep] Link going down mid-run: sending the restore now");
            client_.gcode_script(script);
        }
        unregister();
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
            {"toolhead",
             json::array({"homed_axes", "max_accel", "square_corner_velocity", "extruder"})},
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
        // Every G1 E drives the mounted extruder, and the candidates go to the
        // requested one: measuring on one and applying to the other is wrong.
        const std::string mounted = json_util::safe_string(*toolhead, "extruder");
        if (!mounted.empty() && mounted != req_.extruder) {
            end_without_sweep(fmt::format("T{} is not mounted, so it cannot be measured. Mount it, "
                                          "then start again.",
                                          req_.tool_index));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            if (disconnected_) {
                return;
            }
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
                ++step_serial_;
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
        if (st.phase == Phase::ReadZ) {
            read_z();
            return;
        }
        if (st.phase == Phase::CheckHomed) {
            check_homed();
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

    /// A query step: the status it reads, then `apply` under the lock.
    void query_step(json objects, std::function<void(const json* status)> apply, const char* what) {
        auto self = shared_from_this();
        client_.send_jsonrpc(
            "printer.objects.query", json{{"objects", std::move(objects)}},
            [self, apply](const json& response) {
                {
                    std::lock_guard<std::mutex> lock(self->m_);
                    self->in_flight_ = false;
                    apply(query_status(response));
                }
                self->pump();
            },
            [self, what](const MoonrakerError& err) {
                self->on_step_error(std::string("Could not read ") + what + ": " + err.message);
            },
            0, true);
    }

    void read_z() {
        query_step(
            {{"gcode_move", json::array({"gcode_position"})}},
            [this](const json* status) {
                sweep_->on_z(
                    array_at(member(status, "gcode_move"), "gcode_position", 2).value_or(0.f));
            },
            "the height after homing");
    }

    void check_homed() {
        query_step(
            {{"toolhead", json::array({"homed_axes"})}},
            [this](const json* status) {
                const json* toolhead = member(status, "toolhead");
                const std::string homed =
                    toolhead ? json_util::safe_string(*toolhead, "homed_axes") : std::string();
                sweep_->on_homed(homed.find('x') != std::string::npos &&
                                 homed.find('y') != std::string::npos &&
                                 homed.find('z') != std::string::npos);
            },
            "whether the printer is still homed");
    }

    /// The line's script returned without a verdict: give the reply a few
    /// seconds, then end the run rather than wait latched and hot.
    void arm_verdict_deadline() {
        const uint64_t serial = step_serial_;
        std::weak_ptr<SweepRun> weak = shared_from_this();
        helix::ui::queue_update("PaSweep::verdict_deadline", [weak, serial]() {
            helix::run_after_ms(kVerdictDeadlineMs, [weak, serial]() {
                if (auto self = weak.lock()) {
                    self->on_verdict_deadline(serial);
                }
            });
        });
    }

    void on_verdict_deadline(uint64_t serial) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (!in_flight_ || current_.phase != Phase::Line || step_serial_ != serial ||
                line_verdict_) {
                return;
            }
        }
        on_step_error("The printer did not report a verdict for the test line.");
    }

    void on_step_ok() {
        std::optional<pacal::Attempt> attempt;
        bool arm = false;
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
                    arm = true; // the PA_GET line has not arrived yet
                } else {
                    attempt = feed_verdict_locked(*line_verdict_);
                }
            } else {
                in_flight_ = false;
                sweep_->step_done();
            }
        }
        if (arm) {
            arm_verdict_deadline();
            return;
        }
        if (attempt) {
            report_attempt(*attempt);
        }
        pump();
    }

    void on_step_error(const std::string& message) {
        bool restore_failed = false;
        {
            std::lock_guard<std::mutex> lock(m_);
            spdlog::error("[PaSweep] Step {} failed: {}", static_cast<int>(current_.phase),
                          message);
            restore_failed = current_.phase == Phase::Restore && in_flight_;
            in_flight_ = false;
            if (sweep_) {
                sweep_->fail(message);
            }
        }
        if (restore_failed) {
            // Klipper stops a script at its first error, so the latch may still
            // be set, or the drivers back with Klipper's position wrong. Either
            // way: drivers on, machine unhomed.
            spdlog::warn("[PaSweep] Restore failed: releasing the latch and unhoming");
            client_.gcode_script(latch_recovery_gcode(1.f, 1.f));
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
    uint64_t step_serial_ = 0;
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

void check_interrupted_run(std::function<IMoonrakerAPI*()> api_now,
                           std::function<void()> on_recovered) {
    const json objects = {
        {std::string(kPinX), nullptr},
        {std::string(kPinY), nullptr},
        {"toolhead", json::array({"position"})},
        {"idle_timeout", json::array({"state"})},
        {"configfile", json::array({"settings"})},
    };
    IMoonrakerAPI* api = api_now();
    if (!api) {
        return;
    }
    api->get_client().send_jsonrpc(
        "printer.objects.query", json{{"objects", objects}},
        [api_now, on_recovered, objects](const json& first_response) {
            const json* first = query_status(first_response);
            if (!first ||
                latch_recovery_gcode(pin_value(first, kPinX), pin_value(first, kPinY)).empty()) {
                return;
            }
            spdlog::warn(
                "[PaSweep] X/Y latch set at connect; checking whether a run still owns it");
            const json first_copy = *first;
            helix::ui::queue_update("PaSweep::stale_check", [api_now, on_recovered, objects,
                                                             first_copy]() {
                helix::run_after_ms(kStaleCheckMs, [api_now, on_recovered, objects, first_copy]() {
                    IMoonrakerAPI* api = api_now();
                    if (!api) {
                        return;
                    }
                    api->get_client().send_jsonrpc(
                        "printer.objects.query", json{{"objects", objects}},
                        [api_now, on_recovered, first_copy](const json& second_response) {
                            const json* second = query_status(second_response);
                            const std::string fix =
                                second ? stale_latch_recovery_gcode(first_copy, *second)
                                       : std::string();
                            if (fix.empty()) {
                                spdlog::info("[PaSweep] The latch is in use; leaving it");
                                return;
                            }
                            IMoonrakerAPI* api = api_now();
                            if (!api) {
                                return;
                            }
                            spdlog::warn("[PaSweep] Undoing an interrupted run: {}", fix);
                            api->execute_gcode(
                                fix,
                                [on_recovered]() {
                                    helix::ui::queue_update("PaSweep::recovered", on_recovered);
                                },
                                [](const MoonrakerError& err) {
                                    spdlog::error(
                                        "[PaSweep] Undoing the interrupted run failed: {}",
                                        err.message);
                                },
                                0, /*silent=*/true, /*on_queued=*/nullptr,
                                /*caller_surfaces_errors=*/false);
                        },
                        [](const MoonrakerError&) {}, 0, true);
                });
            });
        },
        [](const MoonrakerError&) {}, 0, true);
}

} // namespace helix::flashforge::pa_sweep
