// SPDX-License-Identifier: GPL-3.0-or-later

#include "flashforge_pa_sweep.h"

#include "core/text_io.h"
#include "json_utils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

#include "hv/json.hpp"

namespace helix::flashforge::pa_sweep {
namespace {

constexpr const char* kGcodeState = "helix_pa_sweep";
constexpr const char* kPinNameX = "enable_pin_tmc_x";
constexpr const char* kPinNameY = "enable_pin_tmc_y";
/// The stock app's arm/disarm codes. The eBoard tests == 11 for start and
/// treats anything else as stop; PC is stored and never read.
constexpr int kActionStart = 11;
constexpr int kActionStop = 0;
constexpr int kActionPc = 666;

std::string pin_lines(float value) {
    return fmt::format("SET_PIN PIN={} VALUE={:.2f}\nSET_PIN PIN={} VALUE={:.2f}", kPinNameX, value,
                       kPinNameY, value);
}

std::string disarm_line() {
    return fmt::format("PA_ACTION ACTION={} PC={}", kActionStop, kActionPc);
}

} // namespace

Sweep::Sweep(Config cfg, int tool_index, std::string extruder_name, Snapshot snapshot)
    : cfg_(cfg), tool_(tool_index), extruder_(std::move(extruder_name)), snap_(snapshot) {
    if (tool_ < 0 || tool_ >= static_cast<int>(cfg_.tool_dx.size())) {
        failure_ = Failure::Error;
        error_ = fmt::format("no purge chute position for tool T{}", tool_);
        phase_ = Phase::Done;
        return;
    }
    phase_ = snap_.homed ? Phase::Prologue : Phase::Home;
}

int Sweep::expected_lines() const {
    return cfg_.winners_needed * static_cast<int>(cfg_.candidates.size());
}

float Sweep::candidate_k(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= cfg_.candidates.size()) {
        return 0.f;
    }
    return cfg_.candidates[static_cast<size_t>(index)];
}

std::string Sweep::prologue_script() const {
    const float x = cfg_.park_x + cfg_.tool_dx[static_cast<size_t>(tool_)];
    const float y = cfg_.park_y + cfg_.tool_dy[static_cast<size_t>(tool_)];
    std::string s = fmt::format("SAVE_GCODE_STATE NAME={}\nG90\nG92 E0\nM83\n"
                                "SET_VELOCITY_LIMIT ACCEL={:.0f}\n"
                                "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY={:.0f}\n",
                                kGcodeState, cfg_.sweep_accel, cfg_.sweep_scv);
    // Raise before travelling, never lower.
    if (snap_.gcode_z < cfg_.line_z) {
        s += fmt::format("G1 Z{:.3f} F{}\n", cfg_.line_z, cfg_.line_z_feed);
    }
    // Stage clear of the chute on X, square up on Y, then come in on X.
    s += fmt::format("G1 X{:.3f} F{}\nG1 Y{:.3f} F{}\nG1 X{:.3f} F{}\n", x - cfg_.park_approach_dx,
                     cfg_.park_x_feed, y, cfg_.park_y_feed, x, cfg_.park_approach_feed);
    // Over the chute, clear of the bed, a head that started higher comes down
    // to the line height the scorer expects.
    if (snap_.gcode_z > cfg_.line_z) {
        s += fmt::format("G1 Z{:.3f} F{}\n", cfg_.line_z, cfg_.line_z_feed);
    }
    s += "M400";
    return s;
}

std::string Sweep::latch_script() const {
    return pin_lines(1.f) + "\nM400";
}

std::string Sweep::line_script(int index) const {
    const float y = cfg_.y_start + cfg_.y_step * static_cast<float>(index);
    std::string s = fmt::format("PA_ACTION ACTION={} PC={}\n"
                                "SET_PRESSURE_ADVANCE EXTRUDER={} ADVANCE={:.4f}\n"
                                "G1 X{:.3f} Y{:.3f} F{}\n",
                                kActionStart, kActionPc, extruder_, candidate_k(index),
                                cfg_.x_start, y, cfg_.travel_feed);
    for (const Segment& seg : cfg_.segments) {
        s += fmt::format("G1 F{}\nG1 X{:.3f} E{:.5f}\n", seg.feed, seg.x, seg.e);
    }
    // No M400 before the disarm: the closed scorer was tuned against the
    // capture closing while the last moves are still queued.
    s += disarm_line() + "\nM400\nPA_GET";
    return s;
}

std::string Sweep::restore_script(bool homed) const {
    // Klipper planned every line against dead drivers, so it believes the head
    // is at the end of the last line; it never left the park. The position is
    // put right while the drivers are still off, so a later line of this
    // script failing cannot leave live drivers aimed at the wrong place. Z is
    // left alone: nothing in the sweep moved it. When something unhomed the
    // machine mid-run, no position is replayed and it stays unhomed.
    std::string s = "M400\n";
    if (located_ && homed) {
        s += fmt::format("SET_KINEMATIC_POSITION X={:.3f} Y={:.3f}\n", raw_x_, raw_y_);
    }
    s += pin_lines(0.f) + "\nM400\n";
    if (located_ && !homed) {
        s += "M84\n";
    }
    s += disarm_line() + "\n";
    s += fmt::format("SET_VELOCITY_LIMIT ACCEL={:.0f} SQUARE_CORNER_VELOCITY={:.0f}\n"
                     "SET_PRESSURE_ADVANCE EXTRUDER={} ADVANCE={:.6f} SMOOTH_TIME={:.6f}\n"
                     "RESTORE_GCODE_STATE NAME={}",
                     snap_.max_accel, snap_.square_corner_velocity, extruder_,
                     snap_.pressure_advance, snap_.smooth_time, kGcodeState);
    return s;
}

std::string Sweep::force_restore() {
    if (!prologue_sent_ || phase_ == Phase::Done || (phase_ == Phase::Restore && awaiting_) ||
        phase_ == Phase::Apply) {
        awaiting_ = false;
        phase_ = Phase::Done;
        return {};
    }
    if (failure_ == Failure::None) {
        failure_ = Failure::Stopped;
    }
    result_.reset();
    latched_ = false;
    awaiting_ = false;
    phase_ = Phase::Done;
    return restore_script(/*homed=*/true);
}

Step Sweep::next() {
    if (awaiting_) {
        return {Phase::Done, {}, -1};
    }
    Step st;
    st.phase = phase_;
    switch (phase_) {
    case Phase::Home:
        st.script = "G28";
        break;
    case Phase::ReadZ:
    case Phase::CheckHomed:
        break;
    case Phase::Prologue:
        st.script = prologue_script();
        prologue_sent_ = true;
        break;
    case Phase::Locate:
        break;
    case Phase::Latch:
        st.script = latch_script();
        latched_ = true;
        break;
    case Phase::Line:
        st.script = line_script(index_);
        st.candidate = index_;
        line_sent_ = true;
        break;
    case Phase::Restore:
        st.script = restore_script(homed_for_restore_);
        latched_ = false;
        break;
    case Phase::Apply:
        st.script = fmt::format("SET_PRESSURE_ADVANCE EXTRUDER={} ADVANCE={:.6f}", extruder_,
                                result_.value_or(0.f));
        break;
    case Phase::Done:
        return st;
    }
    awaiting_ = true;
    return st;
}

void Sweep::step_done() {
    if (!awaiting_) {
        return;
    }
    awaiting_ = false;
    switch (phase_) {
    case Phase::Home:
        phase_ = Phase::ReadZ;
        break;
    case Phase::Prologue:
        phase_ = Phase::Locate;
        break;
    case Phase::Latch:
        phase_ = Phase::Line;
        break;
    case Phase::Restore:
        phase_ = (result_ && failure_ == Failure::None) ? Phase::Apply : Phase::Done;
        break;
    case Phase::Apply:
        phase_ = Phase::Done;
        break;
    case Phase::ReadZ:
    case Phase::Locate:
    case Phase::Line:
    case Phase::CheckHomed:
    case Phase::Done:
        break;
    }
}

void Sweep::on_z(float gcode_z) {
    if (phase_ != Phase::ReadZ || !awaiting_) {
        return;
    }
    awaiting_ = false;
    snap_.gcode_z = gcode_z;
    phase_ = Phase::Prologue;
}

void Sweep::on_homed(bool homed) {
    if (phase_ != Phase::CheckHomed || !awaiting_) {
        return;
    }
    awaiting_ = false;
    homed_for_restore_ = homed;
    phase_ = Phase::Restore;
}

void Sweep::on_located(float raw_x, float raw_y) {
    if (phase_ != Phase::Locate || !awaiting_) {
        return;
    }
    awaiting_ = false;
    raw_x_ = raw_x;
    raw_y_ = raw_y;
    located_ = true;
    phase_ = Phase::Latch;
}

void Sweep::on_verdict(int value) {
    if (phase_ != Phase::Line || !awaiting_) {
        return;
    }
    awaiting_ = false;
    if (verdicts_.size() <= static_cast<size_t>(sweep_)) {
        verdicts_.resize(static_cast<size_t>(sweep_) + 1);
    }
    verdicts_[static_cast<size_t>(sweep_)].push_back(value);
    ++lines_done_;
    ++index_;
    end_sweep_if_complete();
}

void Sweep::end_sweep_if_complete() {
    if (index_ < static_cast<int>(cfg_.candidates.size())) {
        return;
    }
    index_ = 0;
    ++sweep_;
    const auto picked = select(verdicts_, cfg_);
    const bool out_of_sweeps = sweep_ >= cfg_.max_sweeps;
    if (!picked && !out_of_sweeps) {
        return;
    }
    if (!discriminates(verdicts_)) {
        failure_ = Failure::NoDiscrimination;
    } else if (picked) {
        result_ = picked;
    } else {
        failure_ = Failure::TooFewWinners;
    }
    phase_ = restore_phase();
}

void Sweep::enter_restore_or_done() {
    // In flight, the reply still has to come back before anything else is sent;
    // step_done()/on_verdict() then find the phase already moved.
    awaiting_ = false;
    phase_ = prologue_sent_ ? restore_phase() : Phase::Done;
}

Phase Sweep::restore_phase() const {
    return located_ ? Phase::CheckHomed : Phase::Restore;
}

void Sweep::stop() {
    if (phase_ == Phase::Done) {
        return;
    }
    const bool restoring = phase_ == Phase::CheckHomed || phase_ == Phase::Restore;
    const bool applying = phase_ == Phase::Apply;
    if (failure_ == Failure::None) {
        failure_ = Failure::Stopped;
    }
    // A stop after the measurement still goes through the restore, but nothing
    // is applied: the user asked for the run to end.
    result_.reset();
    if (restoring) {
        return;
    }
    if (applying) {
        if (!awaiting_) {
            phase_ = Phase::Done;
        }
        return;
    }
    enter_restore_or_done();
}

void Sweep::fail(std::string message) {
    if (phase_ == Phase::Done) {
        return;
    }
    const bool restoring = (phase_ == Phase::Restore && awaiting_);
    failure_ = Failure::Error;
    error_ = std::move(message);
    result_.reset();
    if (restoring || phase_ == Phase::Apply) {
        // The restore itself failed, or the apply did: nothing more to send.
        awaiting_ = false;
        phase_ = Phase::Done;
        return;
    }
    if (phase_ == Phase::CheckHomed && awaiting_) {
        // Could not read homed_axes: restore without trusting a position.
        awaiting_ = false;
        homed_for_restore_ = false;
        phase_ = Phase::Restore;
        return;
    }
    if (phase_ == Phase::Restore || phase_ == Phase::CheckHomed) {
        return;
    }
    enter_restore_or_done();
}

std::optional<int> parse_pa_get(std::string_view line) {
    constexpr std::string_view kPrefix = "Result is value=";
    if (line.substr(0, 3) == "// ") {
        line.remove_prefix(3);
    }
    if (line.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    line.remove_prefix(kPrefix.size());
    if (line.empty() || line.front() < '0' || line.front() > '9') {
        return std::nullopt;
    }
    return text_io::parse_leading<int>(line);
}

std::optional<float> select(const std::vector<std::vector<int>>& verdicts_by_sweep,
                            const Config& cfg) {
    std::vector<float> winners;
    for (const auto& sweep : verdicts_by_sweep) {
        std::optional<float> best;
        for (size_t i = 0; i < sweep.size() && i < cfg.candidates.size(); ++i) {
            if (sweep[i] == cfg.pass_verdict) {
                best = best ? std::min(*best, cfg.candidates[i]) : cfg.candidates[i];
            }
        }
        if (best) {
            winners.push_back(*best);
            if (static_cast<int>(winners.size()) == cfg.winners_needed) {
                float sum = 0.f;
                for (float w : winners) {
                    sum += w;
                }
                return sum / static_cast<float>(winners.size());
            }
        }
    }
    return std::nullopt;
}

bool discriminates(const std::vector<std::vector<int>>& verdicts_by_sweep) {
    std::optional<int> first;
    for (const auto& sweep : verdicts_by_sweep) {
        for (int v : sweep) {
            if (!first) {
                first = v;
            } else if (v != *first) {
                return true;
            }
        }
    }
    return false;
}

std::string latch_recovery_gcode(float pin_x_value, float pin_y_value) {
    if (pin_x_value == 0.f && pin_y_value == 0.f) {
        return {};
    }
    // M84 drops the homed state, so the next move must home: Klipper's idea
    // of X/Y is wherever the interrupted sweep's last planned move ended.
    return pin_lines(0.f) + "\nM84";
}

namespace {

using nlohmann::json;

const json* status_member(const json& status, std::string_view key) {
    if (!status.is_object()) {
        return nullptr;
    }
    const auto it = status.find(std::string(key));
    return it == status.end() || !it->is_object() ? nullptr : &*it;
}

bool pin_latched(const json& status, std::string_view object) {
    const json* pin = status_member(status, object);
    return pin && json_util::safe_float(*pin, "value", 0.f) != 0.f;
}

/// toolhead.position as reported, or an empty array.
json planned_position(const json& status) {
    const json* toolhead = status_member(status, "toolhead");
    if (!toolhead) {
        return json::array();
    }
    const auto it = toolhead->find("position");
    return it != toolhead->end() && it->is_array() ? *it : json::array();
}

/// "extruder", "extruder1", ...: the sections a candidate K could have been
/// installed on.
bool is_extruder_section(const std::string& name) {
    constexpr std::string_view kPrefix = "extruder";
    if (name.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    for (size_t i = kPrefix.size(); i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

std::string stale_latch_recovery_gcode(const json& first, const json& second) {
    const bool latched_both = (pin_latched(first, kPinX) || pin_latched(first, kPinY)) &&
                              (pin_latched(second, kPinX) || pin_latched(second, kPinY));
    if (!latched_both) {
        return {};
    }
    const json before = planned_position(first);
    const json after = planned_position(second);
    if (before.empty() || before != after) {
        return {}; // a sweep is still drawing lines
    }
    const json* idle = status_member(second, "idle_timeout");
    if (!idle || json_util::safe_string(*idle, "state") == "Printing") {
        return {}; // Klipper is busy: not proof that nothing owns the latch
    }

    std::string s = latch_recovery_gcode(1.f, 1.f);
    const json* configfile = status_member(second, "configfile");
    const json* settings = configfile ? status_member(*configfile, "settings") : nullptr;
    if (settings) {
        if (const json* printer = status_member(*settings, "printer")) {
            const float accel = json_util::safe_float(*printer, "max_accel", 0.f);
            const float scv = json_util::safe_float(*printer, "square_corner_velocity", 5.f);
            if (accel > 0.f) {
                s += fmt::format("\nSET_VELOCITY_LIMIT ACCEL={:.0f} SQUARE_CORNER_VELOCITY={:.0f}",
                                 accel, scv);
            }
        }
        for (const auto& [name, section] : settings->items()) {
            if (!is_extruder_section(name) || !section.is_object()) {
                continue;
            }
            s += fmt::format("\nSET_PRESSURE_ADVANCE EXTRUDER={} ADVANCE={:.6f} SMOOTH_TIME={:.6f}",
                             name, json_util::safe_float(section, "pressure_advance", 0.f),
                             json_util::safe_float(section, "pressure_advance_smooth_time", 0.04f));
        }
    }
    // Last: the saved state is gone if Klipper restarted, and an unknown name
    // ends the script.
    s += fmt::format("\nRESTORE_GCODE_STATE NAME={}", kGcodeState);
    return s;
}

} // namespace helix::flashforge::pa_sweep
