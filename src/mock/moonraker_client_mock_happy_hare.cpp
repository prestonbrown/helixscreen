// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Happy Hare 4 on one ERCF-style unit (the default --test persona): printer.mmu,
// mmu_machine and the configfile sections Happy Hare's Klipper plugin publishes, and
// the MMU_* / T<n> commands that change them, so real discovery builds the production
// AmsBackendHappyHare. Field shapes follow tests/fixtures/happy_hare_v4_*.json and
// Happy Hare's mmu.py get_status().

#include "moonraker_client_mock_happy_hare.h"

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "hv/json.hpp"

using json = nlohmann::json;

namespace helix::mock {

namespace {

/// The mock Spoolman's spools 1-8, gate N carrying spool N+1 in turn.
struct SampleSpool {
    uint32_t rgb;
    const char* name;
    const char* material;
    const char* vendor;
    int temperature;
};
constexpr SampleSpool kSampleSpools[] = {
    {0x1A1A2E, "Jet Black", "PLA", "Polymaker", 210},
    {0x26DCD9, "Silk Blue", "Silk PLA", "eSUN", 215},
    {0x00AEFF, "Pop Blue", "ASA", "Elegoo", 250},
    {0xD20000, "Fire Engine Red", "ABS", "Flashforge", 245},
    {0xF4E111, "Signal Yellow", "PETG", "Kingroon", 235},
    {0xE8E8E8, "Clear", "TPU", "Overture", 225},
    {0x8A949E, "Gray ASA", "ASA", "Bambu Lab", 250},
    {0xA2AAAD, "PolyMax PC Grey", "PC", "Polymaker", 270},
};
constexpr int kNumSampleSpools = sizeof(kSampleSpools) / sizeof(kSampleSpools[0]);
constexpr int kMaxGates = 16;
constexpr const char* kVersion = "4.1.0";
constexpr const char* kUnitName = "unit0";
constexpr const char* kUnitDisplayName = "ERCF";

std::optional<int> int_param(const std::string& gcode, const char* key) {
    const auto v = mock_internal::gcode_param(gcode, key);
    return v ? text_io::parse_leading<int>(*v) : std::nullopt;
}

std::string hex_color(uint32_t rgb) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out(6, '0');
    for (int i = 5; i >= 0; --i, rgb >>= 4) {
        out[static_cast<size_t>(i)] = kDigits[rgb & 0xF];
    }
    return out;
}

HappyHareReply refuse(std::string message) {
    HappyHareReply r;
    r.handled = true;
    r.error = std::move(message);
    return r;
}

HappyHareReply done() {
    HappyHareReply r;
    r.handled = true;
    return r;
}

} // namespace

HappyHareSim::HappyHareSim(int gates, std::string environment_sensor)
    : environment_sensor_(std::move(environment_sensor)) {
    const int n = std::clamp(gates, 1, kMaxGates);
    for (int i = 0; i < n; ++i) {
        const SampleSpool& s = kSampleSpools[i % kNumSampleSpools];
        gates_.push_back({1, s.rgb, s.material, s.vendor, s.name, i + 1, s.temperature});
        ttg_map_.push_back(i);
        endless_spool_groups_.push_back(i);
    }
    if (n > 3) {
        gates_[3].status = 0;
    }
}

int HappyHareSim::gate_for_tool(int tool) const {
    return tool >= 0 && tool < static_cast<int>(ttg_map_.size()) ? ttg_map_[tool] : -1;
}

int HappyHareSim::tool_for_gate(int gate) const {
    if (gate == kGateBypass) {
        return kGateBypass;
    }
    const auto it = std::find(ttg_map_.begin(), ttg_map_.end(), gate);
    return it == ttg_map_.end() ? -1 : static_cast<int>(it - ttg_map_.begin());
}

json HappyHareSim::status() const {
    const size_t n = gates_.size();
    json status = json::array(), rgb = json::array(), color = json::array(),
         material = json::array(), vendor = json::array(), name = json::array(),
         spool_id = json::array(), temperature = json::array(), espooler = json::array(),
         drying = json::array();
    json sensors = json::object();
    for (size_t i = 0; i < n; ++i) {
        const HappyHareGate& g = gates_[i];
        status.push_back(g.status);
        rgb.push_back(g.rgb);
        color.push_back(hex_color(g.rgb));
        material.push_back(g.material);
        vendor.push_back(g.vendor);
        name.push_back(g.name);
        spool_id.push_back(g.spool_id);
        temperature.push_back(g.temperature);
        const bool feeding = filament_pos_ == kPosLoaded && static_cast<int>(i) == gate_;
        espooler.push_back(feeding ? "assist" : "");
        drying.push_back("");
        sensors["mmu_pre_gate_" + std::to_string(i)] = g.status > 0;
    }
    sensors["mmu_gate"] = filament_pos_ >= 1;
    sensors["extruder"] = filament_pos_ >= 7;
    sensors["toolhead"] = filament_pos_ >= 8;

    const char* filament = filament_pos_ == kPosLoaded     ? "Loaded"
                           : filament_pos_ == kPosUnloaded ? "Unloaded"
                                                           : "Unknown";
    const bool feeding = filament_pos_ == kPosLoaded;
    return {
        {"enabled", true},
        {"num_units", 1},
        {"num_gates", static_cast<int>(n)},
        {"unit", 0},
        {"is_homed", true},
        {"print_state", print_state_},
        {"reason_for_pause", reason_for_pause_},
        {"gate", gate_},
        {"tool", tool_},
        {"filament", filament},
        {"filament_pos", filament_pos_},
        {"action", action_},
        {"bowden_progress", -1},
        {"has_bypass", true},
        {"num_toolchanges", 0},
        {"slicer_tool_map", {{"total_toolchanges", nullptr}}},
        {"ttg_map", ttg_map_},
        {"endless_spool_groups", endless_spool_groups_},
        {"endless_spool_enabled", endless_spool_enabled_},
        {"gate_status", status},
        {"gate_color_rgb", rgb},
        {"gate_color", color},
        {"gate_material", material},
        {"gate_vendor", vendor},
        {"gate_name", name},
        {"gate_spool_id", spool_id},
        {"gate_temperature", temperature},
        {"spoolman_support", "off"},
        {"pending_spool_id", -1},
        {"sensors", sensors},
        {"clog_detection_enabled", 2},
        {"encoder",
         {{"enabled", true},
          {"flow_rate", 85},
          {"detection_mode", 2},
          {"desired_headroom", 5.0},
          {"detection_length", 12.4},
          {"headroom", 8.0},
          {"min_headroom", 4.2}}},
        {"sync_drive", feeding},
        {"sync_feedback_state", feeding ? "compressed" : "neutral"},
        {"sync_feedback_bias_modelled", feeding ? 0.15 : 0.0},
        {"sync_feedback_bias_raw", feeding ? 0.18 : 0.0},
        {"sync_feedback_flow_rate", 98.5},
        {"sync_feedback_enabled", sync_feedback_enabled_},
        {"fans", json::array({{{"unit", kUnitName},
                               {"first_gate", 0},
                               {"per_gate", false},
                               {"enabled", fan_enabled_},
                               {"modes", {fan_mode_}}}})},
        {"espooler_active", feeding ? "assist" : ""},
        {"espooler", espooler},
        {"drying_state", drying},
        // The encoder's clog detection: an encoder-only unit has no buffer FlowGuard
        // readings.
        {"flowguard", {{"enabled", true}, {"active", feeding}, {"encoder_mode", 2}}},
        {"tangle_prevention",
         {{"enabled", true},
          {"active", false},
          {"boosted", false},
          {"threshold", 0.6},
          {"release", 0.3}}},
    };
}

json HappyHareSim::machine_status() const {
    json unit = {{"name", kUnitName},
                 {"display_name", kUnitDisplayName},
                 {"vendor", "ERCF"},
                 {"version", "3.0"},
                 {"num_gates", static_cast<int>(gates_.size())},
                 {"first_gate", 0},
                 {"selector_type", "LinearSelector"},
                 {"is_homed", true},
                 {"has_bypass", true},
                 {"filament_always_gripped", false},
                 {"filament_buffer", true}};
    if (!environment_sensor_.empty()) {
        unit["environment_sensor"] = environment_sensor_;
    }
    return {{"happy_hare_version", kVersion},
            {"num_units", 1},
            {"num_gates", static_cast<int>(gates_.size())},
            {"unit_0", unit}};
}

json HappyHareSim::config_sections() const {
    json unit = {{"num_gates", static_cast<int>(gates_.size())},
                 {"vendor", "ERCF"},
                 {"display_name", kUnitDisplayName},
                 {"selector_type", "LinearSelector"},
                 {"encoder", true},
                 {"toolhead", "default"}};
    if (const size_t sp = environment_sensor_.find(' '); sp != std::string::npos) {
        unit["environment_sensor"] = environment_sensor_.substr(sp + 1);
    }
    return {{"mmu_machine", {{"happy_hare_version", kVersion}, {"units", {kUnitName}}}},
            {"mmu_parameters",
             {{"form_tip_macro", "_MMU_FORM_TIP"},
              {"extruder_load_speed", 15.0},
              {"extruder_unload_speed", 20.0}}},
            {std::string("mmu_unit ") + kUnitName, unit},
            {std::string("mmu_unit_parameters ") + kUnitName,
             {{"gear_load_speed", 150.0},
              {"gear_from_filament_buffer_speed", 150.0},
              {"gear_unload_speed", 150.0},
              {"sync_to_extruder", 1},
              {"flowguard_encoder_mode", 2}}},
            {"mmu_toolhead default",
             {{"toolhead_extruder_to_nozzle", 72.0},
              {"toolhead_sensor_to_nozzle", 62.0},
              {"toolhead_entry_to_extruder", 8.0},
              {"toolhead_residual_filament", 0.0},
              {"toolhead_ooze_reduction", 0.0}}}};
}

void HappyHareSim::append_unload(std::vector<HappyHareStep>& steps) const {
    for (const auto& [action, pos] : {std::pair<const char*, int>{"Forming Tip", kPosLoaded},
                                      {"Unloading", 7},
                                      {"Unloading", 3},
                                      {"Unloading", 1},
                                      {"Unloading", kPosUnloaded}}) {
        steps.push_back({action, pos, gate_, tool_});
    }
}

void HappyHareSim::append_load(std::vector<HappyHareStep>& steps, int gate, int tool) const {
    steps.push_back({"Selecting", kPosUnloaded, gate, tool});
    for (int pos : {1, 3, 5, 7, kPosLoaded}) {
        steps.push_back({"Loading", pos, gate, tool});
    }
}

HappyHareReply HappyHareSim::move(std::vector<HappyHareStep> steps) {
    HappyHareReply r = done();
    if (steps.empty()) {
        return r;
    }
    steps.back().action = "Idle";
    busy_ = true;
    r.steps = std::move(steps);
    return r;
}

void HappyHareSim::apply(const HappyHareStep& step, bool last) {
    action_ = step.action;
    filament_pos_ = step.filament_pos;
    gate_ = step.gate;
    tool_ = step.tool;
    if (step.emptied_gate >= 0 && step.emptied_gate < static_cast<int>(gates_.size())) {
        gates_[static_cast<size_t>(step.emptied_gate)].status = 0;
    }
    if (last) {
        busy_ = false;
    }
}

void HappyHareSim::load_tool_now(int tool) {
    const int gate = gate_for_tool(tool);
    if (busy_ || gate < 0 || gates_[static_cast<size_t>(gate)].status == 0) {
        return;
    }
    gate_ = gate;
    tool_ = tool;
    filament_pos_ = kPosLoaded;
    action_ = "Idle";
}

HappyHareReply HappyHareSim::gate_map(const std::string& gcode) {
    const auto gate = int_param(gcode, "GATE");
    if (!gate || *gate < 0 || *gate >= static_cast<int>(gates_.size())) {
        return refuse("MMU_GATE_MAP: GATE must name a gate");
    }
    HappyHareGate& g = gates_[static_cast<size_t>(*gate)];
    if (const auto v = mock_internal::gcode_param(gcode, "MATERIAL")) {
        g.material = *v;
    }
    if (const auto v = mock_internal::gcode_param(gcode, "COLOR")) {
        std::string hex = *v;
        if (!hex.empty() && hex[0] == '#') {
            hex.erase(0, 1);
        }
        g.rgb = hex.empty() ? 0 : static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    }
    if (const auto v = mock_internal::gcode_param(gcode, "NAME")) {
        g.name = *v;
    }
    if (const auto v = mock_internal::gcode_param(gcode, "VENDOR")) {
        g.vendor = *v;
    }
    if (const auto v = int_param(gcode, "SPOOLID")) {
        g.spool_id = *v;
    }
    if (const auto v = int_param(gcode, "TEMP")) {
        g.temperature = *v;
    }
    if (const auto v = int_param(gcode, "AVAILABLE")) {
        g.status = *v;
    }
    return done();
}

HappyHareReply HappyHareSim::endless_spool(const std::string& gcode) {
    if (const auto v = int_param(gcode, "ENABLE")) {
        endless_spool_enabled_ = *v != 0;
    }
    if (int_param(gcode, "RESET").value_or(0) != 0) {
        for (size_t i = 0; i < endless_spool_groups_.size(); ++i) {
            endless_spool_groups_[i] = static_cast<int>(i);
        }
    }
    if (const auto csv = mock_internal::gcode_param(gcode, "GROUPS")) {
        std::vector<int> groups;
        size_t start = 0;
        while (start <= csv->size()) {
            const size_t comma = std::min(csv->find(',', start), csv->size());
            if (const auto v = text_io::parse_leading<int>(csv->substr(start, comma - start))) {
                groups.push_back(*v);
            }
            start = comma + 1;
        }
        if (groups.size() != endless_spool_groups_.size()) {
            return refuse("MMU_ENDLESS_SPOOL: GROUPS needs one group per gate");
        }
        endless_spool_groups_ = std::move(groups);
    }
    return done();
}

HappyHareReply HappyHareSim::command(const std::string& gcode) {
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t\r\n"));
    const int num_gates = static_cast<int>(gates_.size());
    const auto valid_gate = [&](std::optional<int> g) { return g && *g >= 0 && *g < num_gates; };
    const bool moves = cmd == "MMU_LOAD" || cmd == "MMU_UNLOAD" || cmd == "MMU_EJECT" ||
                       cmd == "MMU_SELECT" || cmd == "MMU_SELECT_BYPASS" || cmd == "MMU_HOME" ||
                       cmd == "MMU_CHANGE_TOOL" ||
                       (cmd.size() >= 2 && cmd[0] == 'T' &&
                        cmd.find_first_not_of("0123456789", 1) == std::string::npos);
    if (moves && busy_) {
        return refuse("MMU is busy");
    }

    if (cmd.size() >= 2 && cmd[0] == 'T' &&
        cmd.find_first_not_of("0123456789", 1) == std::string::npos) {
        const auto tool = text_io::parse_leading<int>(cmd.substr(1));
        const int gate = tool ? gate_for_tool(*tool) : -1;
        if (gate < 0) {
            return refuse("Invalid tool " + cmd);
        }
        if (gates_[static_cast<size_t>(gate)].status == 0) {
            return refuse("Gate " + std::to_string(gate) + " is empty");
        }
        if (loaded() && gate == gate_) {
            return done();
        }
        std::vector<HappyHareStep> steps;
        if (loaded()) {
            append_unload(steps);
        }
        append_load(steps, gate, *tool);
        return move(std::move(steps));
    }
    if (cmd == "MMU_LOAD") {
        if (int_param(gcode, "EXTRUDER_ONLY").value_or(0) != 0) {
            return move({{"Loading Ext", 7, gate_, tool_}, {"Idle", kPosLoaded, gate_, tool_}});
        }
        const auto g = int_param(gcode, "GATE");
        const int gate = g.value_or(gate_);
        if (gate < 0 || gate >= num_gates) {
            return refuse("MMU_LOAD: no gate selected");
        }
        if (loaded()) {
            return refuse("Filament already loaded");
        }
        if (gates_[static_cast<size_t>(gate)].status == 0) {
            return refuse("Gate " + std::to_string(gate) + " is empty");
        }
        std::vector<HappyHareStep> steps;
        append_load(steps, gate, tool_for_gate(gate));
        return move(std::move(steps));
    }
    if (cmd == "MMU_UNLOAD") {
        if (!loaded()) {
            return done();
        }
        std::vector<HappyHareStep> steps;
        append_unload(steps);
        return move(std::move(steps));
    }
    if (cmd == "MMU_EJECT") {
        const auto g = int_param(gcode, "GATE");
        const int gate = g.value_or(gate_);
        if (gate < 0 || gate >= num_gates) {
            return refuse("MMU_EJECT: GATE must name a gate");
        }
        std::vector<HappyHareStep> steps;
        if (loaded() && gate == gate_) {
            append_unload(steps);
        } else {
            steps.push_back({"Selecting", kPosUnloaded, gate, tool_for_gate(gate)});
        }
        steps.push_back({"Unloading", kPosUnloaded, gate, tool_for_gate(gate), gate});
        return move(std::move(steps));
    }
    if (cmd == "MMU_SELECT" || cmd == "MMU_SELECT_BYPASS") {
        if (loaded()) {
            return refuse("Unload before selecting another gate");
        }
        int gate = kGateBypass;
        if (cmd == "MMU_SELECT" && int_param(gcode, "BYPASS").value_or(0) == 0) {
            const auto t = int_param(gcode, "TOOL");
            const auto g = t ? std::optional<int>(gate_for_tool(*t)) : int_param(gcode, "GATE");
            if (!valid_gate(g)) {
                return refuse("MMU_SELECT: GATE or TOOL must name a gate");
            }
            gate = *g;
        }
        return move({{"Selecting", kPosUnloaded, gate, tool_for_gate(gate)},
                     {"Idle", kPosUnloaded, gate, tool_for_gate(gate)}});
    }
    if (cmd == "MMU_HOME") {
        if (loaded()) {
            return refuse("Unload before homing the selector");
        }
        return move(
            {{"Homing", kPosUnloaded, gate_, tool_}, {"Idle", kPosUnloaded, 0, tool_for_gate(0)}});
    }
    if (cmd == "MMU_PRELOAD") {
        const auto g = int_param(gcode, "GATE");
        if (!valid_gate(g)) {
            return refuse("MMU_PRELOAD: GATE must name a gate");
        }
        gates_[static_cast<size_t>(*g)].status = 1;
        return done();
    }
    if (cmd == "MMU_CHECK_GATE") {
        for (HappyHareGate& g : gates_) {
            if (g.status < 0) {
                g.status = 1;
            }
        }
        return done();
    }
    if (cmd == "MMU_RECOVER") {
        reason_for_pause_.clear();
        if (print_state_ == "pause_locked") {
            print_state_ = "paused";
        }
        return done();
    }
    if (cmd == "MMU_GATE_MAP") {
        return gate_map(gcode);
    }
    if (cmd == "MMU_TTG_MAP") {
        if (int_param(gcode, "RESET").value_or(0) != 0) {
            for (size_t i = 0; i < ttg_map_.size(); ++i) {
                ttg_map_[i] = static_cast<int>(i);
            }
            return done();
        }
        const auto t = int_param(gcode, "TOOL");
        const auto g = int_param(gcode, "GATE");
        if (!t || *t < 0 || *t >= static_cast<int>(ttg_map_.size()) || !valid_gate(g)) {
            return refuse("MMU_TTG_MAP: TOOL and GATE must name a tool and a gate");
        }
        ttg_map_[static_cast<size_t>(*t)] = *g;
        if (gate_ == *g) {
            tool_ = *t;
        }
        return done();
    }
    if (cmd == "MMU_ENDLESS_SPOOL") {
        return endless_spool(gcode);
    }
    // Parameters as Happy Hare's own commands take them (extras/mmu/commands/
    // mmu_sync_feedback.py, mmu_fan.py); RESET, ADJUST_TENSION and AUTOTUNE change
    // nothing the status reports, so they are only acknowledged.
    if (cmd == "MMU_SYNC_FEEDBACK") {
        if (const auto enable = int_param(gcode, "ENABLE")) {
            sync_feedback_enabled_ = *enable != 0;
        }
        return done();
    }
    if (cmd == "MMU_FAN") {
        if (const auto enable = int_param(gcode, "ENABLE")) {
            fan_enabled_ = *enable != 0;
        }
        if (const auto forced = int_param(gcode, "FAN_FORCED");
            forced && *forced >= 0 && *forced <= 2) {
            fan_mode_ = *forced;
        }
        return done();
    }
    return {};
}

} // namespace helix::mock

// ============================================================================
// MoonrakerClientMock glue
// ============================================================================

bool MoonrakerClientMock::is_mock_happy_hare() const {
    return mmu_enabled_ && !is_mock_medusahc() && !is_mock_ifs_module() && !is_mock_zmod_ifs() &&
           !is_mock_openams() && !is_mock_indx() && !is_mock_lane_status() &&
           !mock_toolchanger_selected() && !is_mock_afc_boxturtle() && !is_mock_u1_snapswap() &&
           helix::mock::inherits_default(printer_type_, helix::mock::default_object::kHappyHareMmu);
}

void MoonrakerClientMock::init_happy_hare(int gates) {
    const bool dryer_sensor =
        helix::mock::inherits_default(printer_type_, helix::mock::default_object::kHtu21dDryer);
    hh_sim_ = std::make_unique<helix::mock::HappyHareSim>(
        gates, dryer_sensor ? "temperature_sensor dryer" : "");
}

json MoonrakerClientMock::happy_hare_mmu_status() const {
    std::lock_guard<std::mutex> lock(hh_mutex_);
    return hh_sim_->status();
}

json MoonrakerClientMock::happy_hare_status_json() const {
    if (!is_mock_happy_hare()) {
        return json::object();
    }
    std::lock_guard<std::mutex> lock(hh_mutex_);
    return {{"mmu", hh_sim_->status()}, {"mmu_machine", hh_sim_->machine_status()}};
}

json MoonrakerClientMock::happy_hare_config_sections() const {
    if (!is_mock_happy_hare()) {
        return json::object();
    }
    std::lock_guard<std::mutex> lock(hh_mutex_);
    return hh_sim_->config_sections();
}

bool MoonrakerClientMock::happy_hare_drives() const {
    // The mmu object is published either way; its commands are the sim's only while
    // the production backend is the one sending them.
    return is_mock_happy_hare() &&
           helix::mock::mock_hardware_backend(mock_internal::effective_mock_ams_env(),
                                              std::getenv("HELIX_MOCK_PRINTER")) != nullptr;
}

void MoonrakerClientMock::happy_hare_load_tool_now(int tool) {
    if (!happy_hare_drives()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(hh_mutex_);
        hh_sim_->load_tool_now(tool);
    }
    dispatch_status_update({{"mmu", happy_hare_mmu_status()}});
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_happy_hare(const std::string& gcode) {
    if (!happy_hare_drives()) {
        return std::nullopt;
    }
    helix::mock::HappyHareReply reply;
    {
        std::lock_guard<std::mutex> lock(hh_mutex_);
        reply = hh_sim_->command(gcode);
    }
    if (!reply.handled) {
        return std::nullopt;
    }
    if (!reply.error.empty()) {
        spdlog::info("[MoonrakerClientMock] Happy Hare refused '{}': {}", gcode, reply.error);
        dispatch_gcode_response("!! " + reply.error);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = reply.error;
        return 1;
    }
    spdlog::debug("[MoonrakerClientMock] Happy Hare: {}", gcode);
    if (reply.steps.empty()) {
        dispatch_status_update({{"mmu", happy_hare_mmu_status()}});
        return 0;
    }

    // One frame per step, the way Happy Hare's action and filament_pos move
    // through an operation.
    constexpr uint32_t kStepMs = 400;
    const auto apply = [this](helix::mock::HappyHareStep step, bool last) {
        {
            std::lock_guard<std::mutex> lock(hh_mutex_);
            hh_sim_->apply(step, last);
        }
        dispatch_status_update({{"mmu", happy_hare_mmu_status()}});
    };
    const helix::mock::HappyHareStep final_step = reply.steps.back();
    reply.steps.pop_back();
    std::vector<helix::SharedCallback<void()>> steps;
    for (const auto& step : reply.steps) {
        steps.emplace_back([apply, step] { apply(step, false); });
    }
    play_console_steps(std::move(steps), kStepMs, [apply, final_step] {
        apply(final_step, true);
        spdlog::info("[MoonrakerClientMock] Happy Hare: gate {} tool {} filament_pos {}",
                     final_step.gate, final_step.tool, final_step.filament_pos);
    });
    return 0;
}
