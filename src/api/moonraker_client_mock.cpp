// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock.h"

#include "ui_update_queue.h"

#include "../tests/mocks/mock_printer_state.h"
#include "accel_sensor_manager.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "gcode_parser.h"
#include "macro_param_cache.h"
#include "moonraker_client_mock_internal.h"
#include "power_device_state.h"
#include "printer_state.h"
#include "probe_sensor_manager.h"
#include "runtime_config.h"
#include "sensor_state.h"
#include "shaper_response.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <unordered_set>

using namespace helix;

// Generate mock geometry for exclude_object status updates.
// Spreads objects in a grid inset from the edges of the mock bed, matching the
// `center` + `polygon` shape Moonraker reports for EXCLUDE_OBJECT_DEFINE.
static json mock_object_entry(const std::string& name, int index, int total) {
    constexpr float INSET = 20.0f; // keep the plate margin visible in the map
    const float bed_w =
        static_cast<float>(mock_internal::MOCK_BED_X_MAX - mock_internal::MOCK_BED_X_MIN) -
        2.0f * INSET;
    const float bed_h =
        static_cast<float>(mock_internal::MOCK_BED_Y_MAX - mock_internal::MOCK_BED_Y_MIN) -
        2.0f * INSET;
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max(1, total)))));
    int rows = std::max(1, (total + cols - 1) / cols);
    int row = index / cols, col = index % cols;
    float cell_w = bed_w / static_cast<float>(cols);
    float cell_h = bed_h / static_cast<float>(rows);
    float cx = static_cast<float>(mock_internal::MOCK_BED_X_MIN) + INSET +
               (static_cast<float>(col) + 0.5f) * cell_w;
    float cy = static_cast<float>(mock_internal::MOCK_BED_Y_MIN) + INSET +
               (static_cast<float>(row) + 0.5f) * cell_h;
    float hw = cell_w * 0.35f, hh = cell_h * 0.35f;
    return {{"name", name},
            {"center", {cx, cy}},
            {"polygon",
             {{cx - hw, cy - hh}, {cx + hw, cy - hh}, {cx + hw, cy + hh}, {cx - hw, cy + hh}}}};
}

namespace {

/// Slicer-style plate names for HELIX_MOCK_EXCLUDE_OBJECTS. Real slicers emit
/// `<model>_id_<n>_copy_<n>`, so these are long enough to exercise label
/// truncation/wrapping in the side list rather than flattering it.
constexpr const char* MOCK_EXCLUDE_OBJECT_NAMES[] = {
    "Benchy_id_0_copy_0",        "Calibration_Cube_id_1_copy_0", "Bracket_Left_id_2_copy_0",
    "Bracket_Right_id_3_copy_0", "Gear_Housing_id_4_copy_0",     "Vase_id_5_copy_0",
    "Spool_Holder_id_6_copy_0",  "Cable_Clip_id_7_copy_0",       "Hinge_Pin_id_8_copy_0",
    "Knob_id_9_copy_0",          "Fan_Duct_id_10_copy_0",        "Strain_Relief_id_11_copy_0"};

constexpr int MAX_MOCK_EXCLUDE_OBJECT_COUNT =
    static_cast<int>(sizeof(MOCK_EXCLUDE_OBJECT_NAMES) / sizeof(MOCK_EXCLUDE_OBJECT_NAMES[0]));

/// Objects published for a bare `HELIX_MOCK_EXCLUDE_OBJECTS=1`. Enough to fill a
/// side list past one screen on the short landscape panel without being absurd.
constexpr int DEFAULT_MOCK_EXCLUDE_OBJECT_COUNT = 5;

/// Registry consult: is this object a chamber HEATER (heater_generic /
/// temperature_fan whose bare name a chamber-heater backend claims)?
/// Replaces the old find("chamber") scans — backend-named heaters like
/// "heater_generic dragonbreath" contain no "chamber" at all.
bool is_chamber_heater_object(const std::string& obj) {
    std::string name;
    if (obj.rfind("heater_generic ", 0) == 0) {
        name = obj.substr(15);
    } else if (obj.rfind("temperature_fan ", 0) == 0) {
        name = obj.substr(16);
    } else {
        return false;
    }
    return chamber::match(name).confidence > 0;
}

/// Does any registered backend expose this exact bare object as its
/// diagnostics status object? Used by the HELIX_MOCK_OBJECTS tokenizer to
/// recognize a standalone diagnostics token ("dragonbreath") that follows a
/// completed chamber heater instead of appending to it.
bool is_registered_diagnostics_object(const std::string& token) {
    for (const auto* backend : chamber::registry()) {
        if (!backend->diagnostics_object().empty() && backend->diagnostics_object() == token) {
            return true;
        }
    }
    return false;
}

} // namespace

// Delegating constructor - uses default speedup of 1.0
MoonrakerClientMock::MoonrakerClientMock(PrinterType type) : MoonrakerClientMock(type, 1.0) {}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type, double speedup_factor)
    : printer_type_(type) {
    // Initialize idle timeout tracking
    last_activity_time_ = std::chrono::steady_clock::now();

    // Set speedup factor (clamped)
    speedup_factor_.store(std::clamp(speedup_factor, 0.1, 10000.0));

    spdlog::debug("[MoonrakerClientMock] Created with printer type: {}, speedup: {}x",
                  static_cast<int>(type), speedup_factor_.load());

    // Register method handlers for all RPC domains
    mock_internal::register_file_handlers(method_handlers_);
    mock_internal::register_print_handlers(method_handlers_);
    mock_internal::register_object_handlers(method_handlers_);
    mock_internal::register_history_handlers(method_handlers_);
    mock_internal::register_server_handlers(method_handlers_);
    mock_internal::register_queue_handlers(method_handlers_);
    spdlog::debug("[MoonrakerClientMock] Registered {} RPC method handlers",
                  method_handlers_.size());

    // Populate hardware immediately (available for wizard without calling discover_printer())
    populate_hardware();
    spdlog::debug(
        "[MoonrakerClientMock] Hardware populated: {} heaters, {} sensors, {} fans, {} LEDs",
        discovery_.heaters().size(), discovery_.sensors().size(), discovery_.fans().size(),
        discovery_.leds().size());

    // Generate synthetic bed mesh data
    generate_mock_bed_mesh();

    // Pre-populate so capabilities are available for UI tests that skip connect()
    populate_capabilities();

    // Check HELIX_MOCK_SPOOLMAN env var for Spoolman availability
    const char* spoolman_env = std::getenv("HELIX_MOCK_SPOOLMAN");
    if (spoolman_env && (std::string(spoolman_env) == "0" || std::string(spoolman_env) == "off")) {
        mock_spoolman_enabled_ = false;
        spdlog::info("[MoonrakerClientMock] Mock Spoolman disabled via HELIX_MOCK_SPOOLMAN=0");
    }

    // HELIX_MOCK_EXCLUDE_OBJECTS=<n>|1 — publish a synthetic multi-object plate at
    // print start. The stock test G-codes declare a single EXCLUDE_OBJECT_DEFINE,
    // and the objects button is gated on two or more, so without this the
    // exclude-object map and side list are unreachable under --test.
    const char* exclude_env = std::getenv("HELIX_MOCK_EXCLUDE_OBJECTS");
    if (exclude_env && exclude_env[0] && std::string(exclude_env) != "0" &&
        std::string(exclude_env) != "off") {
        int requested = static_cast<int>(std::strtol(exclude_env, nullptr, 10));
        // "1"/"on"/"yes" mean "give me a plausible plate", not "give me one
        // object" — one object would leave the button hidden, which is the very
        // thing the flag exists to defeat.
        if (requested <= 1) {
            requested = DEFAULT_MOCK_EXCLUDE_OBJECT_COUNT;
        }
        mock_exclude_object_count_ = std::clamp(requested, 2, MAX_MOCK_EXCLUDE_OBJECT_COUNT);
        spdlog::info("[MoonrakerClientMock] HELIX_MOCK_EXCLUDE_OBJECTS={} — will publish {} "
                     "synthetic exclude_object entries at print start",
                     exclude_env, mock_exclude_object_count_);
    }

    // Set up bed mesh callback to handle incoming status updates
    // This ensures dispatch_status_update updates the mock's internal bed mesh state
    set_bed_mesh_callback([this](const json& bed_mesh) { parse_incoming_bed_mesh(bed_mesh); });
}

void MoonrakerClientMock::set_simulation_speedup(double factor) {
    double clamped = std::clamp(factor, 0.1, 10000.0);
    speedup_factor_.store(clamped);
    spdlog::info("[MoonrakerClientMock] Simulation speedup set to {}x", clamped);
}

double MoonrakerClientMock::get_simulation_speedup() const {
    return speedup_factor_.load();
}

bool MoonrakerClientMock::arm_event_replay(const std::string& json_path) {
    replay_events_.clear();
    replay_next_ = 0;

    std::ifstream f(json_path);
    if (!f.good()) {
        spdlog::warn("[Mock Replay] Cannot open replay script '{}'", json_path);
        return false;
    }
    try {
        json script = json::parse(f);
        for (const auto& ev : script.at("events")) {
            ReplayEvent re;
            re.t_ms = ev.value("t", 0);
            if (ev.value("type", "") == "gcode_response") {
                re.is_gcode = true;
                re.line = ev.at("line").get<std::string>();
            } else {
                re.object = ev.at("object").get<std::string>();
                re.payload = ev.at("payload");
            }
            replay_events_.push_back(std::move(re));
        }
        // Optional: serve configfile.settings.bed_mesh.probe_count so the
        // collector's denominator query answers like the real printer did.
        if (script.contains("config_probe_count") && script["config_probe_count"].is_array() &&
            script["config_probe_count"].size() == 2) {
            set_config_bed_mesh_probe_count(script["config_probe_count"][0].get<int>(),
                                            script["config_probe_count"][1].get<int>());
        }
    } catch (const std::exception& e) {
        replay_events_.clear();
        spdlog::warn("[Mock Replay] Failed to parse '{}': {}", json_path, e.what());
        return false;
    }

    if (replay_events_.empty()) {
        spdlog::warn("[Mock Replay] Script '{}' carries no events", json_path);
        return false;
    }
    spdlog::info("[Mock Replay] Armed {} events from '{}' (span {:.0f}s)", replay_events_.size(),
                 json_path, static_cast<double>(replay_events_.back().t_ms) / 1000.0);
    return true;
}

namespace {
/// Per-persona travel bounds reported as toolhead axis_maximum. The K1
/// persona carries the real values from a K1C capture (printer.cfg
/// position_max 229/227/255) so printer detection scores it as the Creality
/// it is; every other persona keeps the long-standing generic volume and
/// detects exactly as before.
std::array<double, 3> persona_axis_maximum(MoonrakerClientMock::PrinterType type) {
    switch (type) {
    case MoonrakerClientMock::PrinterType::CREALITY_K1:
        return {229.0, 227.0, 255.0};
    default:
        return {235.0, 235.0, 250.0};
    }
}
} // namespace

void MoonrakerClientMock::start_replay_timer() {
    if (replay_events_.empty() || replay_timer_ != nullptr) {
        return;
    }
    // Grace before the first event: the app re-dispatches its cached status
    // snapshot after subsystem init (~1.4s in), which would otherwise land
    // after the script's first print_stats edge and stamp standby over it.
    replay_start_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    spdlog::info("[Mock Replay] Starting: {} events at {:.0f}x speedup", replay_events_.size(),
                 get_simulation_speedup());
    replay_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<MoonrakerClientMock*>(lv_timer_get_user_data(t));
            self->pump_replay();
        },
        20, this);
}

void MoonrakerClientMock::pump_replay() {
    const double speed = std::max(1.0, get_simulation_speedup());
    const auto now = std::chrono::steady_clock::now();
    if (now < replay_start_) {
        return;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - replay_start_).count();
    const uint64_t due_ms = static_cast<uint64_t>(static_cast<double>(elapsed_ms) * speed);

    while (replay_next_ < replay_events_.size() && replay_events_[replay_next_].t_ms <= due_ms) {
        fire_replay_event(replay_events_[replay_next_]);
        ++replay_next_;
    }
    if (replay_next_ >= replay_events_.size() && replay_timer_ != nullptr) {
        spdlog::info("[Mock Replay] Finished: {} events fired", replay_events_.size());
        lv_timer_t* timer = replay_timer_;
        replay_timer_ = nullptr;
        lv_timer_delete(timer);
    }
}

void MoonrakerClientMock::fire_replay_event(const ReplayEvent& event) {
    if (event.is_gcode) {
        json msg = {{"method", "notify_gcode_response"}, {"params", {event.line}}};
        dispatch_method_callback("notify_gcode_response", msg);
        return;
    }

    // Keep the mock's own simulators in step with the capture so their later
    // pushes (idle-timeout steppers, cooldown transitions, snapshots rebuilt
    // from internal state) agree with the script instead of fighting it.
    if (event.object == "print_stats" && event.payload.contains("state")) {
        const std::string state = event.payload.value("state", "");
        int mapped = 0; // standby
        if (state == "printing") {
            mapped = 1;
        } else if (state == "paused") {
            mapped = 2;
        } else if (state == "complete") {
            mapped = 3;
        } else if (state == "cancelled") {
            mapped = 4;
        } else if (state == "error") {
            mapped = 5;
        }
        print_state_.store(mapped);
        if (event.payload.contains("filename") && event.payload["filename"].is_string()) {
            std::lock_guard<std::mutex> lock(print_mutex_);
            print_filename_ = event.payload["filename"].get<std::string>();
        }
    } else if (event.object == "extruder") {
        if (event.payload.contains("target") && event.payload["target"].is_number()) {
            extruder_target_.store(event.payload["target"].get<double>());
        }
        if (event.payload.contains("temperature") && event.payload["temperature"].is_number()) {
            extruder_temp_.store(event.payload["temperature"].get<double>());
        }
    } else if (event.object == "heater_bed") {
        if (event.payload.contains("target") && event.payload["target"].is_number()) {
            bed_target_.store(event.payload["target"].get<double>());
        }
        if (event.payload.contains("temperature") && event.payload["temperature"].is_number()) {
            bed_temp_.store(event.payload["temperature"].get<double>());
        }
    }

    json status = {{event.object, event.payload}};
    dispatch_status_update(status);
}

void MoonrakerClientMock::reset_idle_timeout() {
    last_activity_time_ = std::chrono::steady_clock::now();
    if (idle_timeout_triggered_.load()) {
        idle_timeout_triggered_.store(false);
        spdlog::debug("[MoonrakerClientMock] Idle timeout reset");
    }
}

int MoonrakerClientMock::get_current_layer() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (print_metadata_.layer_count == 0) {
        return 0;
    }
    return static_cast<int>(print_progress_.load() * print_metadata_.layer_count);
}

int MoonrakerClientMock::get_total_layers() const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    return static_cast<int>(print_metadata_.layer_count);
}

bool MoonrakerClientMock::has_chamber_sensor() const {
    // The simulation thread calls this every iteration while discover_printer()
    // is still reassigning discovery_.sensors() on the calling thread — the same
    // heap-use-after-free the writers below guard against. Neither caller (here
    // and dispatch_historical_temperatures()) holds the lock, so taking it here
    // cannot self-deadlock.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
    for (const auto& s : discovery_.sensors()) {
        if (s == "temperature_sensor chamber") {
            return true;
        }
    }
    return false;
}

std::string MoonrakerClientMock::chamber_heater_status_key() const {
    return cached_chamber_status_key_;
}

std::string MoonrakerClientMock::chamber_heater_bare_name() const {
    const std::string key = chamber_heater_status_key();
    if (key.rfind("heater_generic ", 0) == 0) {
        return key.substr(15);
    }
    if (key.rfind("temperature_fan ", 0) == 0) {
        return key.substr(16);
    }
    return {};
}

std::string MoonrakerClientMock::chamber_filter_pin_object() const {
    const auto hw = discovery_.hardware();
    const auto* backend = chamber::backend_by_id(hw.chamber_heater_backend_id());
    if (!backend) {
        return {};
    }
    const std::string pin(backend->filter_fan_pin());
    if (pin.empty()) {
        return {};
    }
    const auto& objects = hw.printer_objects();
    if (std::find(objects.begin(), objects.end(), pin) == objects.end()) {
        return {};
    }
    return pin;
}

void MoonrakerClientMock::append_chamber_backend_status(json& status_obj, double sim_time,
                                                        const json* requested) const {
    const auto hw = discovery_.hardware();
    const auto* backend = chamber::backend_by_id(hw.chamber_heater_backend_id());
    if (!backend) {
        return;
    }
    const auto& objects = hw.printer_objects();
    auto present = [&objects, requested](const std::string& key) {
        return std::find(objects.begin(), objects.end(), key) != objects.end() &&
               (!requested || requested->contains(key));
    };

    const std::string diag(backend->diagnostics_object());
    if (!diag.empty() && present(diag)) {
        // VENDOR_OK: the mock SIMULATES the vendor's hardware — this payload
        // mirrors the dragonbreath status schema that
        // chamber_heater_backend_dragonbreath.cpp parses (verified live on the
        // U1 rig, issue #1290). Detection stays registry-driven.
        if (backend->id() == "dragonbreath") {
            const double chamber_temp = chamber_temp_.load();
            const double chamber_target = chamber_target_.load();
            const double filter_value = chamber_filter_value_.load();
            const bool filter_on = filter_value > 0.0;
            // Test hook: HELIX_MOCK_DRAGONBREATH_FAULT=1 latches a fault into
            // every synthesized frame.
            const char* fault_env = std::getenv("HELIX_MOCK_DRAGONBREATH_FAULT");
            const bool mock_fault = fault_env && fault_env[0] == '1';
            // PTC element rides a few degrees above chamber air, drifting
            // with the same slow sine the other mock sensors use.
            const double ptc_temp =
                chamber_temp + 4.0 + 2.0 * std::sin(2.0 * M_PI * sim_time / 75.0);
            status_obj[diag] = {{"temperature", chamber_temp},
                                {"target", chamber_target},
                                {"fault", mock_fault},
                                {"inhibited", false},
                                {"fault_reason", mock_fault ? json("ptc_overtemp") : json(nullptr)},
                                {"ptc_temp", ptc_temp},
                                {"fan_percent", filter_on ? 100 : 0},
                                {"fan_reason", filter_on ? "filter" : "off"},
                                {"mode", chamber_target > 0.0 ? "power_on" : "off"},
                                {"source", "klipper"},
                                {"lease_owned", chamber_target > 0.0},
                                {"connected", true}};
        }
    }

    const std::string pin(backend->filter_fan_pin());
    if (!pin.empty() && present(pin)) {
        status_obj[pin] = {{"value", chamber_filter_value_.load()}};
    }
}

void MoonrakerClientMock::update_cached_chamber_key() {
    cached_chamber_status_key_.clear();
    for (const auto& h : discovery_.heaters()) {
        if (h == "heater_bed" || h.rfind("extruder", 0) == 0) {
            continue;
        }
        // Registry consult (not find("chamber")) so backend-named heaters
        // ("heater_generic dragonbreath") win the chamber status key too.
        if (is_chamber_heater_object(h)) {
            cached_chamber_status_key_ = h;
            return;
        }
    }
}

// PRECONDITION: caller holds discovery_mutex_. Reached only from
// populate_capabilities() and rebuild_hardware_from_lists(), both of which take it.
// Locking here instead would self-deadlock — discovery_mutex_ is not recursive.
void MoonrakerClientMock::override_chamber_heater(const std::string& heater_obj) {
    auto& heaters = discovery_.heaters();
    // Registry-matched erase: a later "heater_generic chamber" replaces a
    // dragonbreath heater and vice versa (find("chamber") would miss the
    // backend-named ones entirely).
    heaters.erase(std::remove_if(heaters.begin(), heaters.end(),
                                 [](const std::string& h) { return is_chamber_heater_object(h); }),
                  heaters.end());
    heaters.push_back(heater_obj);

    // temperature_fan needs to be in sensors list for periodic status updates
    if (heater_obj.rfind("temperature_fan ", 0) == 0) {
        discovery_.sensors().push_back(heater_obj);
    }

    update_cached_chamber_key();
}

std::set<std::string> MoonrakerClientMock::get_excluded_objects() const {
    // If shared state is set, use that for consistency with MoonrakerAPIMock
    if (mock_state_) {
        return mock_state_->get_excluded_objects();
    }
    // Fallback to local state for backward compatibility
    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
    return excluded_objects_;
}

void MoonrakerClientMock::set_mock_state(std::shared_ptr<MockPrinterState> state) {
    mock_state_ = state;
    if (state) {
        spdlog::debug("[MoonrakerClientMock] Shared mock state attached");
    } else {
        spdlog::debug("[MoonrakerClientMock] Shared mock state detached");
    }
}

MoonrakerClientMock::~MoonrakerClientMock() {
    // Signal restart thread to stop and wait for it (under lock to prevent race)
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        restart_pending_.store(false);
        if (restart_thread_.joinable()) {
            restart_thread_.join();
        }
    }

    // Pass true to skip logging during destruction - spdlog may already be destroyed
    stop_temperature_simulation(true);

    // Clean up any outstanding calibration timers (PID, MPC, shaper) to prevent
    // use-after-free when a subsequent test calls process_lvgl().
    // Timer callbacks self-delete via lv_timer_delete, so some tracked pointers
    // may be stale. Verify each exists in LVGL's timer list before deleting.
    // The payload deleter runs either way when the timer is still armed: the
    // callback that would have freed it will never fire.
    if (lv_is_initialized()) {
        if (replay_timer_ != nullptr) {
            lv_timer_delete(replay_timer_);
            replay_timer_ = nullptr;
        }
        for (auto& tracked : calibration_timers_) {
            bool still_alive = false;
            lv_timer_t* t = lv_timer_get_next(nullptr);
            while (t) {
                if (t == tracked.timer) {
                    still_alive = true;
                    break;
                }
                t = lv_timer_get_next(t);
            }
            if (still_alive) {
                if (tracked.free_payload) {
                    tracked.free_payload();
                }
                lv_timer_delete(tracked.timer);
            }
        }
    }
    calibration_timers_.clear();
}

int MoonrakerClientMock::connect(const char* url, std::function<void()> on_connected,
                                 [[maybe_unused]] std::function<void()> on_disconnected) {
    spdlog::debug("[MoonrakerClientMock] Simulating connection to: {}", url ? url : "(null)");

    // Record the URL exactly as the real connect() does. Without this,
    // get_last_url() stays empty for the whole life of a mock session and
    // anything asking which host we are talking to -- is_local_moonraker(),
    // telemetry topology -- silently reads "no connection".
    set_last_url(url ? url : "");

    // Simulate connection state change (same as real client)
    set_connection_state(ConnectionState::CONNECTING);

    // Small delay to simulate realistic connection (250ms / speedup)
    double speedup = speedup_factor_.load();
    auto delay_ms = static_cast<int>(250.0 / speedup);
    if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    // Check if we should simulate disconnected state for testing
    if (get_runtime_config()->simulate_disconnect) {
        spdlog::warn(
            "[MoonrakerClientMock] --disconnected flag set, simulating connection failure");
        set_connection_state(ConnectionState::DISCONNECTED);
        // Don't invoke on_connected callback or dispatch any state
        return 0;
    }

    set_connection_state(ConnectionState::CONNECTED);

    // Dispatch historical temperature data first (fills graph with 2-3 min of data)
    dispatch_historical_temperatures();

    // Start live temperature simulation
    start_temperature_simulation();

    // Dispatch initial state BEFORE calling on_connected (matches real Moonraker behavior)
    // Real client sends initial state from subscription response - mock does it here
    dispatch_initial_state();

    // Auto-start a print if configured (e.g., when testing print-status panel)
    if (get_runtime_config()->mock_auto_start_print) {
        // Use --gcode-file if specified, otherwise fall back to default test file
        const char* print_file = get_runtime_config()->gcode_test_file
                                     ? get_runtime_config()->gcode_test_file
                                     : RuntimeConfig::DEFAULT_TEST_FILE;
        spdlog::info("[MoonrakerClientMock] Auto-starting print simulation with '{}'", print_file);
        start_print_internal(print_file);
    }

    // Walk a captured print-start sequence through the real dispatch paths
    // (HELIX_MOCK_REPLAY). Runs after the initial state so the replay's
    // print_stats edge arms the collector the same way a live print would.
    if (!replay_events_.empty()) {
        start_replay_timer();
    }

    // Immediately invoke connection callback
    if (on_connected) {
        spdlog::debug("[MoonrakerClientMock] Simulated connection successful");
        on_connected();
    }

    // Seed AmsBackendHappyHare's initial "mmu" status (--real-ams only — see
    // should_mock_ams() guard below). Real Moonraker delivers gate data as part
    // of the printer.objects.subscribe response, which Application::
    // dispatch_status_update() re-broadcasts AFTER init_subsystems_from_hardware()
    // creates the AMS backend. Those two calls live in two DIFFERENT queued
    // callbacks, not one: init_subsystems_from_hardware() runs inside the
    // set_on_hardware_discovered handler's queue_update (application.cpp,
    // Application::setup_discovery_callbacks), and dispatch_status_update() runs
    // inside the later set_on_discovery_complete handler's queue_update (same
    // function). The mock's own discovery path (just above, via on_connected()
    // -> discover_printer()) reports an empty status snapshot instead, so
    // nothing ever primes the backend's notify_status_update listener with
    // gate_status. The ordering this seed depends on holds because
    // discover_printer() invokes invoke_hardware_discovered() then
    // invoke_discovery_complete() synchronously and in that order — each one
    // enqueues its own queue_update() from the same call stack — and
    // UpdateQueue::pending_ is a plain std::queue drained FIFO, so this seed's
    // own queue_update() call below (issued after both of those) is guaranteed
    // to drain after them, by which point the backend (and its notify
    // subscription) already exists.
    // Gated on test_mode + !should_mock_ams(): AmsState only creates a real
    // AmsSubscriptionBackend (the thing that would ever consume this) when
    // should_mock_ams() is false (src/printer/ams_state.cpp
    // init_backends_from_hardware()); the plain --test default builds
    // AmsBackendMock instead, which never subscribes to "mmu" at all.
    // should_mock_ams() alone is not a safe gate here: it also reads false
    // when test_mode is false, which is the case for most unit tests that
    // construct a MoonrakerClientMock directly without setting --test — that
    // unconditional read leaked a pending UpdateQueue callback into every
    // other unit test connecting a mock client with MMU hardware and not
    // draining the queue before teardown (#seed_mmu_status isolation leak).
    // Requiring test_mode explicitly scopes this to real --test runs.
    if (discovery_.hardware().has_mmu() && get_runtime_config()->test_mode &&
        !get_runtime_config()->should_mock_ams()) {
        // Guard with lifetime_weak() (same idiom SubscriptionGuard uses) rather than
        // a bare `this` capture: the queue drains on a later main-loop tick, and a
        // printer switch / reconnect can destroy this MoonrakerClientMock before
        // then. Both queuing and draining are main-thread-only here, so a plain
        // expired() check (no AsyncLifetimeGuard token) is sufficient -- this isn't
        // the bg-thread TOCTOU L081 Mechanism C targets.
        std::weak_ptr<bool> alive = lifetime_weak();
        helix::ui::queue_update(
            "MoonrakerClientMock::seed_mmu_status",
            [this, alive]() { // QUEUE_RAW_THIS_OK: guarded by alive.expired() below
                if (alive.expired()) {
                    return;
                }
                dispatch_status_update({{"mmu", mock_internal::get_mock_mmu_status()}});
            });
    }

    // Store disconnect callback (never invoked in mock, but stored for consistency)
    // Note: Not needed for this simple mock implementation

    return 0; // Success
}

void MoonrakerClientMock::populate_capabilities() {
    // Held for the whole body: the simulation thread may already be running (connect()
    // starts it before discover_printer() calls this) and iterates these same lists.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Create mock Klipper object list for capabilities parsing
    json mock_objects = json::array();

    // Add common objects
    mock_objects.push_back("heater_bed");
    mock_objects.push_back("extruder");
    mock_objects.push_back("bed_mesh");
    mock_objects.push_back("probe"); // Most printers have a probe for bed mesh/leveling

    // Add capabilities for UI testing (speaker for M300, firmware retraction for G10/G11)
    mock_objects.push_back("output_pin beeper");   // Triggers has_speaker_ capability
    mock_objects.push_back("firmware_retraction"); // Triggers has_firmware_retraction_ capability

    // Chamber temperature sensor for UI testing
    mock_objects.push_back("temperature_sensor chamber");

    // HELIX_MOCK_OBJECTS: space-separated list of additional Klipper objects to add
    // e.g., HELIX_MOCK_OBJECTS="temperature_fan chamber" to test temperature_fan chamber
    // heaters, or the dragonbreath trio:
    //   "heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter"
    // Runs BEFORE the discovery-list snapshot below on purpose: a chamber
    // heater from the env REPLACES the default-profile one
    // (override_chamber_heater), and a stale "heater_generic chamber" left in
    // the snapshot would outscore a backend-named heater in parse_objects
    // (keyword 100 beats the appliance backends' 95).
    const char* mock_obj_env = std::getenv("HELIX_MOCK_OBJECTS");
    if (mock_obj_env && mock_obj_env[0]) {
        std::istringstream iss(mock_obj_env);
        std::string token;
        std::string current_obj;
        auto flush_object = [&]() {
            mock_objects.push_back(current_obj);
            spdlog::info("[MoonrakerClientMock] Added mock object: {}", current_obj);
            // A chamber heater from the env replaces the default-profile
            // chamber heater so the registry pick — and the mock's
            // chamber-status-key cache — resolve to the env-specified one.
            if (is_chamber_heater_object(current_obj)) {
                override_chamber_heater(current_obj);
            }
        };
        while (iss >> token) {
            // Accumulate tokens: "temperature_fan" + "chamber" → "temperature_fan chamber"
            if (!current_obj.empty()) {
                // A type prefix always starts a new object...
                bool is_prefix = (token.rfind("heater_generic", 0) == 0 ||
                                  token.rfind("temperature_fan", 0) == 0 ||
                                  token.rfind("temperature_sensor", 0) == 0 ||
                                  token.rfind("output_pin", 0) == 0);
                // ...and so does a bare backend diagnostics object
                // ("dragonbreath") following a COMPLETED chamber heater —
                // without this the second "dragonbreath" would append to
                // "heater_generic dragonbreath" instead of standing alone.
                bool is_standalone_diagnostics = current_obj.find(' ') != std::string::npos &&
                                                 is_chamber_heater_object(current_obj) &&
                                                 is_registered_diagnostics_object(token);
                if (is_prefix || is_standalone_diagnostics) {
                    // Flush previous object
                    flush_object();
                    current_obj = token;
                } else {
                    current_obj += " " + token;
                }
            } else {
                current_obj = token;
            }
        }
        if (!current_obj.empty()) {
            flush_object();
        }
    }

    // Add hardware objects from populated lists
    for (const auto& heater : discovery_.heaters()) {
        // Skip if already added (heater_bed, extruder)
        if (heater != "heater_bed" && heater != "extruder") {
            mock_objects.push_back(heater);
        }
    }
    for (const auto& fan : discovery_.fans()) {
        mock_objects.push_back(fan);
    }
    for (const auto& sensor : discovery_.sensors()) {
        // Only include sensors with proper prefixes — skip bare heater names
        // (e.g., "extruder", "heater_bed") which are already in heaters list
        if (sensor.rfind("temperature_sensor ", 0) == 0 ||
            sensor.rfind("temperature_fan ", 0) == 0 || sensor.rfind("tmc2240 ", 0) == 0 ||
            sensor.rfind("tmc2209 ", 0) == 0 || sensor.rfind("tmc5160 ", 0) == 0) {
            mock_objects.push_back(sensor);
        }
    }
    for (const auto& led : discovery_.leds()) {
        mock_objects.push_back(led);
    }
    // Additional objects set via set_additional_objects() for capability testing
    for (const auto& obj : additional_objects_) {
        mock_objects.push_back(obj);
    }

    // Add printer-specific objects
    switch (printer_type_) {
    case PrinterType::VORON_24:
        mock_objects.push_back("quad_gantry_level");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    case PrinterType::VORON_TRIDENT:
        mock_objects.push_back("z_tilt");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    default:
        // Other printers may not have these features
        break;
    }

    // Add LED effects (klipper-led_effect plugin objects)
    mock_objects.push_back("led_effect breathing");
    mock_objects.push_back("led_effect fire_comet");
    mock_objects.push_back("led_effect rainbow");
    mock_objects.push_back("led_effect static_white");

    // [exclude_object] — present on modern Klipper/Kalico configs. Trips
    // has_exclude_object_ in PrinterDiscovery so capability-gated features
    // (e.g. adaptive bed mesh) light up under --test.
    mock_objects.push_back("exclude_object");

    // Add common macros for all printer types (for testing macro panel)
    mock_objects.push_back("gcode_macro START_PRINT");
    mock_objects.push_back("gcode_macro END_PRINT");
    mock_objects.push_back("gcode_macro PAUSE");
    mock_objects.push_back("gcode_macro RESUME");
    mock_objects.push_back("gcode_macro CANCEL_PRINT");
    mock_objects.push_back("gcode_macro LOAD_FILAMENT");
    mock_objects.push_back("gcode_macro UNLOAD_FILAMENT");
    mock_objects.push_back("gcode_macro BED_MESH_CALIBRATE");
    mock_objects.push_back("gcode_macro G28");           // Home all
    mock_objects.push_back("gcode_macro M600");          // Filament change
    mock_objects.push_back("gcode_macro _SYSTEM_MACRO"); // System macro (hidden by default)

    // Add LED-related macros (auto-detected by printer_discovery via LED keywords)
    mock_objects.push_back("gcode_macro LIGHTS_ON");
    mock_objects.push_back("gcode_macro LIGHTS_OFF");
    mock_objects.push_back("gcode_macro LIGHTS_TOGGLE");
    mock_objects.push_back("gcode_macro LED_PARTY");
    mock_objects.push_back("gcode_macro LED_NIGHTLIGHT");

    // MCU objects for discovery
    mock_objects.push_back("mcu");
    mock_objects.push_back("mcu EBBCan");

    // Humidity sensors (BME280/HTU21D for enclosure monitoring)
    mock_objects.push_back("bme280 chamber");
    mock_objects.push_back("htu21d dryer");
    spdlog::debug("[MoonrakerClientMock] Mock humidity sensors: bme280 chamber, htu21d dryer");

    // Width sensor (filament diameter measurement via Hall effect sensor)
    mock_objects.push_back("hall_filament_width_sensor");

    // Moonraker plugins
    mock_objects.push_back("timelapse"); // Moonraker-Timelapse plugin

    // MMU/AMS system - Happy Hare uses "mmu" object name.
    // Suppressed in the MedusaHC modes: the default mock ships "mmu", which
    // detects Happy Hare and would stand a second AMS backend up alongside the
    // tool changer. A real MedusaHC has no MMU.
    if (mmu_enabled_ && !is_mock_medusahc()) {
        mock_objects.push_back("mmu");
    }

    // Probe sensor (HELIX_MOCK_PROBE_TYPE: cartographer, tap, bltouch, beacon, klicky, standard,
    // none)
    const char* probe_env = std::getenv("HELIX_MOCK_PROBE_TYPE");
    std::string mock_probe_type = (probe_env && probe_env[0]) ? probe_env : "cartographer";
    if (mock_probe_type == "none") {
        spdlog::debug("[MoonrakerClientMock] Probe disabled via env var");
    } else if (mock_probe_type == "cartographer") {
        mock_objects.push_back("cartographer");
        spdlog::debug("[MoonrakerClientMock] Mock probe: cartographer");
    } else if (mock_probe_type == "bltouch") {
        mock_objects.push_back("bltouch");
        spdlog::debug("[MoonrakerClientMock] Mock probe: bltouch");
    } else if (mock_probe_type == "beacon") {
        mock_objects.push_back("beacon");
        spdlog::debug("[MoonrakerClientMock] Mock probe: beacon");
    } else {
        // tap, klicky, standard, etc. → generic "probe" object
        mock_objects.push_back("probe");
        spdlog::debug("[MoonrakerClientMock] Mock probe: {} (as generic probe)", mock_probe_type);
    }

    // Filament sensors (common setup: runout sensor at spool holder)
    // Check HELIX_MOCK_FILAMENT_SENSORS env var for custom sensor names
    // Default: single switch sensor named "runout_sensor"
    const char* sensor_env = std::getenv("HELIX_MOCK_FILAMENT_SENSORS");
    if (sensor_env && std::string(sensor_env) == "none") {
        // Explicitly disabled
        spdlog::debug("[MoonrakerClientMock] Filament sensors disabled via env var");
    } else if (sensor_env) {
        // Custom sensor list (comma-separated, e.g., "switch:fsensor,motion:encoder")
        std::string sensors_str(sensor_env);
        size_t pos = 0;
        while ((pos = sensors_str.find(',')) != std::string::npos || !sensors_str.empty()) {
            std::string token =
                (pos != std::string::npos) ? sensors_str.substr(0, pos) : sensors_str;
            size_t colon = token.find(':');
            if (colon != std::string::npos) {
                std::string type = token.substr(0, colon);
                std::string name = token.substr(colon + 1);
                if (type == "switch") {
                    mock_objects.push_back("filament_switch_sensor " + name);
                } else if (type == "motion") {
                    mock_objects.push_back("filament_motion_sensor " + name);
                }
            }
            if (pos == std::string::npos)
                break;
            sensors_str.erase(0, pos + 1);
        }
        spdlog::debug("[MoonrakerClientMock] Custom filament sensors from env: {}", sensor_env);
    } else {
        // Default: one switch sensor (typical Voron setup)
        mock_objects.push_back("filament_switch_sensor runout_sensor");
        spdlog::debug(
            "[MoonrakerClientMock] Default filament sensor: filament_switch_sensor runout_sensor");
    }

    // Toolchanger mock mode (HELIX_MOCK_AMS=toolchanger): push the toolchanger +
    // per-tool discovery objects so PrinterDiscovery sets has_tool_changer() and
    // tool_names() = {T0..T3}. The 4 extruder heaters (extruder + extruder1/2/3)
    // are already flowing in via the discovery_.heaters() loop above. Tool naming
    // uses "tool TN" to match the established test convention (test_hardware_validator).
    if (is_mock_toolchanger()) {
        mock_objects.push_back("toolchanger");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("tool T" + std::to_string(i));
        }
    }

    // MedusaHC mock mode (HELIX_MOCK_AMS=medusahc[-fork]): a hotend changer
    // bolted onto klipper-toolchanger. Unlike the toolchanger mode above this
    // does NOT build an AmsBackendMock - try_create_mock() declines these values
    // - so the objects pushed here are what real discovery sees, and the
    // production AmsBackendToolChanger + toolchanger_addon path runs against
    // them. Each variant emits the object set its controller actually has, so
    // toolchanger_addon's detection and schema discrimination are exercised at
    // runtime and not only in the unit tests.
    // See docs/devel/FILAMENT_BACKEND_MEDUSAHC.md § "Two shipping configurations".
    if (const MedusaVariant variant = mock_medusa_variant(); variant != MedusaVariant::NONE) {
        // The fork drops both halves of the Irbis3D signature; [medusahc] alone
        // is the only thing left to detect on.
        if (variant != MedusaVariant::FORK) {
            mock_objects.push_back("pin_watch io");
            mock_objects.push_back("toolchanger");
            for (int i = 0; i < 4; ++i) {
                mock_objects.push_back("tool T" + std::to_string(i));
            }
        } else {
            // No [tool N] objects: the tools are named from the extruder heaters
            // (one hot end per extruder), and T<n> drives the swap.
            for (int i = 0; i < 4; ++i) {
                mock_objects.push_back("gcode_macro T" + std::to_string(i));
            }
        }
        // Every MedusaHC runs one of the two Python extras, so [medusahc] is
        // always present.
        mock_objects.push_back("medusahc");
        // Sibling object on a real controller. Present so the exact-match rule
        // in PrinterDiscovery (which must NOT claim it) has something to not
        // claim.
        mock_objects.push_back("medusahc_calibrate");
        // Feeder macros. The Irbis3D controller registers the native MHC_*
        // commands and ships legacy OPEN/CLOSE aliases forwarding to them, so
        // both are present there and detection should prefer MHC_OPEN.
        if (variant == MedusaVariant::CONTROLLER) {
            mock_objects.push_back("gcode_macro MHC_OPEN");
            mock_objects.push_back("gcode_macro MHC_CLOSE");
        }
        mock_objects.push_back("gcode_macro OPEN");
        mock_objects.push_back("gcode_macro CLOSE");
        spdlog::info("[MoonrakerClientMock] MedusaHC mock: variant={}",
                     variant == MedusaVariant::CONTROLLER ? "controller" : "fork");
    }

    // Parse objects into hardware discovery (unified hardware access)
    discovery_.modify_hardware([&](PrinterDiscovery& hw) { hw.parse_objects(mock_objects); });

    // Mock accelerometer configuration for input shaper wizard testing
    // Real Klipper doesn't expose accelerometers in objects list (no get_status()),
    // so we simulate what parse_config_keys() would find from configfile.config
    json mock_config = mock_internal::get_mock_accel_config();
    // Bed screws — same story as the accelerometers: screws_tilt_adjust has no
    // get_status(), so Klipper never lists it and the capability is detected
    // from configfile.config. Without this the whole mock screws-tilt state
    // machine is unreachable in --test.
    mock_config["screws_tilt_adjust"] = {{"screw_thread", mock_internal::MOCK_SCREW_THREAD},
                                         {"speed", "50"},
                                         {"horizontal_move_z", "10"}};
    // Provide kinematics so bed_moves detection works
    // HELIX_MOCK_KINEMATICS overrides; otherwise default matches printer type
    const char* kin_env = std::getenv("HELIX_MOCK_KINEMATICS");
    std::string default_kinematics;
    switch (printer_type_) {
    case PrinterType::VORON_24:
    case PrinterType::VORON_TRIDENT:
        default_kinematics = "corexy";
        break;
    case PrinterType::CREALITY_K1:
        default_kinematics = "corexy";
        break;
    default:
        default_kinematics = "cartesian";
        break;
    }
    std::string mock_kinematics = (kin_env && kin_env[0]) ? kin_env : default_kinematics;
    mock_config["printer"] = {{"kinematics", mock_kinematics}};
    // Add gcode_macro entries for param detection (shared with configfile.config response)
    mock_config.merge_patch(mock_internal::get_mock_gcode_macro_config());
    // Probe section — shared with the configfile.config query/subscribe responses
    // so all three payloads describe the same probe.
    mock_config.merge_patch(mock_internal::get_mock_probe_config());
    // Stepper travel limits, matching the configfile.settings the query and
    // subscribe handlers report. The real discovery sequence derives the build
    // volume from these before detection runs, so the mock has to carry them or
    // --test detects against an empty volume the live path no longer sees.
    mock_config["stepper_x"] = {{"position_min", mock_internal::MOCK_BED_X_MIN},
                                {"position_max", mock_internal::MOCK_BED_X_MAX}};
    mock_config["stepper_y"] = {{"position_min", mock_internal::MOCK_BED_Y_MIN},
                                {"position_max", mock_internal::MOCK_BED_Y_MAX}};
    mock_config["stepper_z"] = {{"position_min", 0.0},
                                {"position_max", mock_internal::MOCK_BED_Z_MAX}};

    std::unordered_set<std::string> macros_snapshot;
    discovery_.modify_hardware([&](PrinterDiscovery& hw) {
        hw.parse_config_keys(mock_config);
        hw.parse_build_volume(mock_config);
        macros_snapshot = hw.macros();
    });

    // Populate macro param cache from mock config (same as real discovery sequence)
    helix::MacroParamCache::instance().populate_from_configfile(mock_config, macros_snapshot);

    spdlog::debug("[MoonrakerClientMock] Mock config: adxl345, resonance_tester, kinematics={}",
                  mock_kinematics);

    // Populate printer objects for hardware discovery
    std::vector<std::string> all_objects;
    for (const auto& obj : mock_objects) {
        std::string name = obj.get<std::string>();
        all_objects.push_back(name);
    }
    discovery_.modify_hardware([&](PrinterDiscovery& hw) { hw.set_printer_objects(all_objects); });
    update_cached_chamber_key();

    // Set mock MCU version data (after parse_objects which clears everything)
    discovery_.modify_hardware([](PrinterDiscovery& hw) {
        hw.set_mcu("stm32f446xx");
        hw.set_mcu_list({"stm32f446xx", "stm32g0b1xx"});
        hw.set_mcu_versions(
            {{"mcu", "v0.12.0-155-g4cfa273e"}, {"mcu EBBCan", "v0.12.0-155-g4cfa273e"}});
    });

    // Also populate filament_sensors vector for subscription (same as real parse_objects)
    discovery_.filament_sensors().clear();
    for (const auto& obj : mock_objects) {
        std::string name = obj.get<std::string>();
        if (name.rfind("filament_switch_sensor ", 0) == 0 ||
            name.rfind("filament_motion_sensor ", 0) == 0) {
            discovery_.filament_sensors().push_back(name);
        }
    }

    spdlog::debug("[MoonrakerClientMock] Hardware populated: {} macros, {} filament sensors",
                  discovery_.hardware().macros().size(), discovery_.filament_sensors().size());
}

void MoonrakerClientMock::rebuild_hardware_from_lists() {
    // See populate_capabilities(). The set_* helpers release their own lock before
    // calling this, so taking it here is a fresh acquisition, not a nested one.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Lightweight re-parse: build objects array from current discovery lists only.
    // Used by test helpers (set_heaters, set_fans, etc.) that need to update hardware()
    // without adding hardcoded common objects from populate_capabilities().

    // Apply additional_objects overrides to discovery lists
    // (e.g., temperature_fan chamber replacing heater_generic dragonbreath —
    // registry-matched, so backend-named heaters swap both ways)
    for (const auto& obj : additional_objects_) {
        if (is_chamber_heater_object(obj)) {
            override_chamber_heater(obj);
        }
    }

    // Build objects array from the corrected lists
    json objects = json::array();

    for (const auto& h : discovery_.heaters()) {
        objects.push_back(h);
    }
    for (const auto& f : discovery_.fans()) {
        objects.push_back(f);
    }
    for (const auto& s : discovery_.sensors()) {
        if (s.rfind("temperature_sensor ", 0) == 0 || s.rfind("temperature_fan ", 0) == 0 ||
            s.rfind("tmc2240 ", 0) == 0 || s.rfind("tmc2209 ", 0) == 0 ||
            s.rfind("tmc5160 ", 0) == 0) {
            objects.push_back(s);
        }
    }
    for (const auto& l : discovery_.leds()) {
        objects.push_back(l);
    }
    for (const auto& fs : discovery_.filament_sensors()) {
        objects.push_back(fs);
    }
    for (const auto& obj : additional_objects_) {
        objects.push_back(obj);
    }

    // Chamber-backend surfaces (bare diagnostics object, filter-fan pin) are
    // not derivable from the discovery lists — parse_objects() classifies
    // neither into one. Preserve the ones a previous populate materialized
    // (and only while the backend's heater itself survived the rebuild), or
    // the chamber status helpers go silent after set_heaters()/set_fans().
    {
        const auto hw_prev = discovery_.hardware();
        const auto* backend = chamber::backend_by_id(hw_prev.chamber_heater_backend_id());
        const std::string& heater = hw_prev.chamber_heater_name();
        if (backend && !heater.empty() &&
            std::find(objects.begin(), objects.end(), json(heater)) != objects.end()) {
            const auto& prev = hw_prev.printer_objects();
            for (const std::string key : {std::string(backend->diagnostics_object()),
                                          std::string(backend->filter_fan_pin())}) {
                if (!key.empty() && std::find(prev.begin(), prev.end(), key) != prev.end() &&
                    std::find(objects.begin(), objects.end(), json(key)) == objects.end()) {
                    objects.push_back(key);
                }
            }
        }
    }

    discovery_.modify_hardware([&](PrinterDiscovery& hw) {
        hw.parse_objects(objects);
        // parse_objects() clears printer_objects_; repopulate from the same
        // array the way populate_capabilities() does (and the real discovery
        // sequence does at moonraker_discovery_sequence.cpp), or
        // objects.list and the chamber-backend status helpers go dark after
        // a rebuild.
        std::vector<std::string> all_objects;
        all_objects.reserve(objects.size());
        for (const auto& obj : objects) {
            all_objects.push_back(obj.get<std::string>());
        }
        hw.set_printer_objects(all_objects);
    });
    update_cached_chamber_key();
}

void MoonrakerClientMock::discover_printer(
    std::function<void()> on_complete, std::function<void(const std::string& reason)> on_error) {
    spdlog::debug("[MoonrakerClientMock] Simulating hardware discovery");

    // Check Klippy state - discovery fails if Klippy not connected
    KlippyState state = klippy_state_.load();
    if (state == KlippyState::STARTUP || state == KlippyState::ERROR) {
        std::string reason = "Klippy Host not connected";
        spdlog::warn("[MoonrakerClientMock] Discovery failed: {}", reason);

        // Emit discovery failed event (matches real client behavior)
        emit_event(MoonrakerEventType::DISCOVERY_FAILED, reason, true);

        // Invoke error callback if provided
        if (on_error) {
            on_error(reason);
        }
        return;
    }

    // Populate hardware based on printer type (may have already been done in constructor)
    populate_hardware();

    // This shortcut never queries configfile, so the accelerometer seeding the
    // real sequence does in moonraker_discovery_sequence.cpp is missing here.
    // Without it AccelSensorManager stays empty under --test and Settings >
    // Sensors shows no accelerometer on a mock printer that reports one.
    // Main thread only — discover_from_config() sets LVGL subjects.
    json accel_config = mock_internal::get_mock_accel_config();
    helix::ui::queue_update([accel_config]() {
        helix::sensors::AccelSensorManager::instance().discover_from_config(accel_config);
    });

    // Generate synthetic bed mesh data (may have already been done in constructor)
    generate_mock_bed_mesh();

    // Query server.info to get moonraker_version (uses registered RPC handler)
    send_jsonrpc("server.info", json::object(), [this, on_complete](json response) {
        if (response.contains("result")) {
            auto moonraker_version = response["result"].value("moonraker_version", "unknown");
            discovery_.modify_hardware(
                [&](PrinterDiscovery& hw) { hw.set_moonraker_version(moonraker_version); });
            spdlog::debug("[MoonrakerClientMock] Moonraker version: {}", moonraker_version);
        }

        // Chain to printer.info to get hostname and software_version
        send_jsonrpc("printer.info", json::object(), [this, on_complete](json response) {
            spdlog::debug("[MoonrakerClientMock] printer.info response received");

            // Re-populate after mock discovery may have changed hardware data
            populate_capabilities();

            // Now set the metadata AFTER parse_objects() has run
            if (response.contains("result")) {
                auto hostname = response["result"].value("hostname", "unknown");
                auto software_version = response["result"].value("software_version", "unknown");
                discovery_.modify_hardware([&](PrinterDiscovery& hw) {
                    hw.set_hostname(hostname);
                    hw.set_software_version(software_version);
                });
                spdlog::debug("[MoonrakerClientMock] Printer hostname: {}", hostname);
                spdlog::debug("[MoonrakerClientMock] Klipper software version: {}",
                              software_version);
            }

            // Query machine.system_info for OS version (uses registered RPC handler)
            send_jsonrpc(
                "machine.system_info", json::object(),
                [this](json sys_response) {
                    if (sys_response.contains("result") &&
                        sys_response["result"].contains("system_info") &&
                        sys_response["result"]["system_info"].contains("distribution") &&
                        sys_response["result"]["system_info"]["distribution"].contains("name")) {
                        std::string os_name =
                            sys_response["result"]["system_info"]["distribution"]["name"]
                                .get<std::string>();
                        discovery_.modify_hardware(
                            [&](PrinterDiscovery& hw) { hw.set_os_version(os_name); });
                        spdlog::debug("[MoonrakerClientMock] OS version: {}", os_name);
                    }
                },
                [](const MoonrakerError& err) {
                    spdlog::debug("[MoonrakerClientMock] machine.system_info failed: {}",
                                  err.message);
                });

            // Set Spoolman availability during discovery (matches real Moonraker behavior)
            // Real client queries server.spoolman.status during discovery - see
            // moonraker_client.cpp:1047
            get_printer_state().set_spoolman_available(mock_spoolman_enabled_);
            spdlog::debug("[MoonrakerClientMock] Spoolman available: {}", mock_spoolman_enabled_);

            // Set webcam availability during discovery (matches real Moonraker behavior)
            // Real client queries server.webcams.list during discovery
            get_printer_state().set_webcam_available(true, "/webcam/?action=stream",
                                                     "/webcam/?action=snapshot");
            spdlog::debug("[MoonrakerClientMock] Webcam available: true (mock always has webcam)");

            // Set power device count during discovery (matches real Moonraker behavior)
            // Real client queries machine.device_power.devices during discovery
            if (std::getenv("MOCK_EMPTY_POWER")) {
                get_printer_state().set_power_device_count(0);
                helix::PowerDeviceState::instance().set_devices({});
                spdlog::debug("[MoonrakerClientMock] Power devices: 0 (MOCK_EMPTY_POWER set)");
            } else {
                get_printer_state().set_power_device_count(4);
                std::vector<PowerDevice> mock_power_devices = {
                    {"printer_psu", "gpio", "on", false},
                    {"chamber_light", "klipper_device", "on", true},
                    {"exhaust_fan", "klipper_device", "off", false},
                    {"led_strip", "gpio", "on", false},
                };
                helix::PowerDeviceState::instance().set_devices(mock_power_devices);
                spdlog::debug("[MoonrakerClientMock] Power devices: 4 (mock default)");
            }

            // Set up mock sensors
            std::vector<helix::SensorInfo> mock_sensors = {
                {"mock_energy",
                 "Mock Energy Monitor",
                 "mqtt",
                 {"power", "voltage", "current", "energy"}},
            };
            nlohmann::json mock_sensor_values = {
                {"mock_energy",
                 {{"power", 45.0}, {"voltage", 230.5}, {"current", 0.195}, {"energy", 123.4}}},
            };
            helix::SensorState::instance().set_sensors(mock_sensors, mock_sensor_values);
            spdlog::debug("[MoonrakerClientMock] Sensors: {} (mock default)", mock_sensors.size());

            // Log discovered hardware
            spdlog::debug("[MoonrakerClientMock] Discovered: {} heaters, {} sensors, {} fans, {} "
                          "LEDs",
                          discovery_.heaters().size(), discovery_.sensors().size(),
                          discovery_.fans().size(), discovery_.leds().size());

            // Early hardware discovery callback (for AMS/MMU initialization)
            // Must be called BEFORE discovery_complete to match real implementation timing
            spdlog::debug("[MoonrakerClientMock] Invoking early hardware discovery callback");
            discovery_.invoke_hardware_discovered();

            // Seed probe z_offset from the mock configfile, mirroring Step 4 of
            // MoonrakerDiscoverySequence. This shortcut of a discover_printer()
            // never queries configfile, so without it the whole configfile→probe
            // path — the one that rescues probes whose runtime status reports a
            // null z_offset — is unreachable under --test.
            //
            // Queued, not called inline, for ordering: ProbeSensorManager's
            // sensor list is populated by the hardware-discovered callback just
            // above, which Application also queues. Seeding runs on a sensor list
            // that does not exist yet if it jumps the queue. FIFO puts it second.
            helix::ui::queue_update("MoonrakerClientMock::probe_config_seed", []() {
                helix::sensors::ProbeSensorManager::instance().discover_from_config(
                    mock_internal::get_mock_probe_config());
            });

            // Invoke discovery complete callback with hardware (for PrinterState binding)
            discovery_.invoke_discovery_complete();

            // Invoke completion callback immediately (no async delay in mock)
            if (on_complete) {
                on_complete();
            }
        });
    });
}

bool MoonrakerClientMock::is_mock_toolchanger() const {
    // Mirror the HELIX_MOCK_AMS parsing in ams_backend.cpp: toolchanger mode is
    // selected by "toolchanger", "tool_changer", or "tc" (case-insensitive).
    const char* ams_env = std::getenv("HELIX_MOCK_AMS");
    if (!ams_env || !ams_env[0]) {
        return false;
    }
    std::string ams_type(ams_env);
    std::transform(ams_type.begin(), ams_type.end(), ams_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc";
}

MoonrakerClientMock::MedusaVariant MoonrakerClientMock::mock_medusa_variant() const {
    const char* ams_env = std::getenv("HELIX_MOCK_AMS");
    if (!ams_env || !ams_env[0]) {
        return MedusaVariant::NONE;
    }
    std::string ams_type(ams_env);
    std::transform(ams_type.begin(), ams_type.end(), ams_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ams_type == "medusahc-fork" || ams_type == "medusa-fork") {
        return MedusaVariant::FORK;
    }
    if (ams_type == "medusahc" || ams_type == "medusa" || ams_type == "mhc") {
        return MedusaVariant::CONTROLLER;
    }
    return MedusaVariant::NONE;
}

bool MoonrakerClientMock::is_mock_medusahc() const {
    return mock_medusa_variant() != MedusaVariant::NONE;
}

nlohmann::json MoonrakerClientMock::medusa_status_json() const {
    const MedusaVariant variant = mock_medusa_variant();
    if (variant == MedusaVariant::NONE) {
        return nlohmann::json::object();
    }

    constexpr int kToolCount = 4;
    const int current = medusa_current_tool_.load();
    const int target = medusa_target_tool_.load();
    const bool sensor_error = medusa_sensor_error_.load();
    const int phase = medusa_phase_.load();
    const char* operation = (phase == 1) ? "dropping" : (phase == 2) ? "picking" : "idle";

    // -2 is how both schemas encode "the docks cannot tell", and it is a
    // distinct answer from -1 "nothing on the head".
    const int reported_tool = sensor_error ? -2 : current;

    nlohmann::json obj;
    obj["current_tool"] = reported_tool;
    obj["tool_count"] = kToolCount;

    if (variant == MedusaVariant::CONTROLLER) {
        obj["operation"] = operation;
        obj["target_tool"] = target;
        obj["last_error"] = "";
        obj["feeder_open"] = medusa_feeder_open_.load();
        obj["layer"] = 0;
        obj["sensor_error"] = sensor_error;
        // "e" is the toolhead, "t<n>" each dock: a tool is seated in its dock
        // exactly when it is not the one on the head.
        nlohmann::json sensors;
        sensors["e"] = (current >= 0) ? 1 : 0;
        for (int i = 0; i < kToolCount; ++i) {
            sensors["t" + std::to_string(i)] = (i == current) ? 0 : 1;
        }
        obj["sensors"] = sensors;
    } else {
        // topi314/MedusaHC (scripts/medusahc.py). Verified against that source,
        // NOT inferred from the upstream schema: `state` is a COARSER vocabulary
        // than `operation`, not a rename of it. _set_state() is only ever called
        // with uninitialized / ready / changing / error - there is no picking or
        // dropping - so a swap here is one undifferentiated "changing", the same
        // resolution klipper-toolchanger's own status gives.
        obj["state"] = (phase == 0) ? "ready" : "changing";
        // A bool here, where upstream sends a `last_error` message string.
        obj["error"] = false;
        // Published by BOTH controllers (medusahc.py:370), so the gripper phases
        // are drivable on this machine too.
        obj["feeder_open"] = medusa_feeder_open_.load();
        obj["target_tool"] = target;
        obj["layer"] = 0;
        obj["head_loaded"] = (current >= 0);
        for (int i = 0; i < kToolCount; ++i) {
            obj["tool" + std::to_string(i) + "_docked"] = (i != current);
        }
    }
    return obj;
}

// Notifications a single swap phase lasts. One notification is
// NOTIFICATION_INTERVAL_TICKS * SIMULATION_INTERVAL_MS = ~1s, so a full
// drop-then-pick swap runs ~6s: slow enough to watch the step bar move,
// fast enough not to be a wait.
static constexpr int kMedusaPhaseNotifications = 3;

void MoonrakerClientMock::start_medusa_swap(int tool) {
    const int current = medusa_current_tool_.load();
    medusa_target_tool_.store(tool);
    // Releasing the filament is the first thing a swap does: on a hotend changer
    // the frame-side gripper is the only thing holding it, and the hot end
    // cannot leave with the strand still clamped.
    medusa_feeder_open_.store(true);
    // Nothing on the head means there is nothing to drop; go straight to picking.
    const bool needs_drop = (current >= 0);
    const int next_phase = needs_drop ? 1 : (tool >= 0 ? 2 : 0);
    // Ticks BEFORE the phase: the simulation thread polls medusa_phase_ to decide
    // whether to advance, so publishing the phase first lets it observe the new
    // phase with the previous count of 0, complete the phase on its very first
    // tick and leave the counter at -1.
    medusa_phase_ticks_.store(next_phase == 0 ? 0 : kMedusaPhaseNotifications);
    medusa_phase_.store(next_phase);
    if (next_phase == 0) {
        // Unmount requested with an empty head: a no-op on real firmware too.
        medusa_feeder_open_.store(false);
        medusa_target_tool_.store(-1);
    }
    spdlog::info("[MoonrakerClientMock] MedusaHC swap armed: {} -> {} (phase={})", current, tool,
                 next_phase);
}

void MoonrakerClientMock::advance_medusa_swap() {
    const int phase = medusa_phase_.load();
    if (phase == 0) {
        return;
    }
    if (medusa_phase_ticks_.fetch_sub(1) > 1) {
        return; // still inside this phase
    }

    if (phase == 1) {
        // Drop complete: the head is empty and the old tool is back in its dock.
        medusa_current_tool_.store(-1);
        const int target = medusa_target_tool_.load();
        if (target < 0) {
            // UNSELECT_TOOL / DROP_TOOL: unmount only, re-grip the filament.
            medusa_phase_.store(0);
            medusa_phase_ticks_.store(0);
            medusa_feeder_open_.store(false);
            spdlog::info("[MoonrakerClientMock] MedusaHC unmount complete");
            return;
        }
        medusa_phase_.store(2);
        medusa_phase_ticks_.store(kMedusaPhaseNotifications);
        spdlog::debug("[MoonrakerClientMock] MedusaHC drop complete, picking T{}", target);
        return;
    }

    // Pick complete: the new hot end is on the head, so the gripper closes again.
    const int target = medusa_target_tool_.load();
    medusa_current_tool_.store(target);
    medusa_target_tool_.store(-1);
    medusa_phase_.store(0);
    medusa_phase_ticks_.store(0);
    medusa_feeder_open_.store(false);
    spdlog::info("[MoonrakerClientMock] MedusaHC swap complete: T{} on head", target);
}

nlohmann::json MoonrakerClientMock::pin_watch_status_json() const {
    const MedusaVariant variant = mock_medusa_variant();
    if (variant == MedusaVariant::NONE || variant == MedusaVariant::FORK) {
        return nlohmann::json::object();
    }
    const int current = medusa_current_tool_.load();
    return nlohmann::json{{"current_tool", medusa_sensor_error_.load() ? -2 : current}};
}

void MoonrakerClientMock::populate_hardware() {
    // See populate_capabilities(). The clear()/assign below are exactly the writes
    // that freed string buffers out from under the simulation thread.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Clear existing data (in discovery sequence)
    discovery_.heaters().clear();
    discovery_.sensors().clear();
    discovery_.fans().clear();
    discovery_.leds().clear();

    // Populate based on printer type
    switch (printer_type_) {
    case PrinterType::VORON_24:
        // Voron 2.4 configuration
        discovery_.heaters() = {"heater_bed", "extruder", "heater_generic chamber"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber",
                                "temperature_sensor raspberry_pi",
                                "temperature_sensor mcu_temp",
                                "tmc2240 stepper_x",
                                "tmc2240 stepper_y"};
        discovery_.fans() = {"heater_fan hotend_fan",
                             "fan", // Part cooling fan
                             "fan_generic nevermore", "controller_fan controller_fan"};
        discovery_.leds() = {"neopixel chamber_light", "neopixel status_led", "led caselight",
                             "output_pin Enclosure_LEDs"};
        break;

    case PrinterType::VORON_TRIDENT:
        // Voron Trident configuration
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber",
                                "temperature_sensor raspberry_pi",
                                "temperature_sensor mcu_temp",
                                "temperature_sensor z_thermal_adjust",
                                "tmc2240 stepper_x",
                                "tmc2240 stepper_y"};
        discovery_.fans() = {"heater_fan hotend_fan", "fan", "fan_generic exhaust_fan",
                             "controller_fan electronics_fan"};
        discovery_.leds() = {"neopixel sb_leds", "neopixel chamber_leds"};
        break;

    case PrinterType::CREALITY_K1:
        // Creality K1/K1 Max configuration
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor mcu_temp", "temperature_sensor host_temp"};
        discovery_.fans() = {"heater_fan hotend_fan", "fan", "fan_generic auxiliary_fan"};
        discovery_.leds() = {"neopixel logo_led"};
        break;

    case PrinterType::FLASHFORGE_AD5M:
        // FlashForge Adventurer 5M configuration
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber", "temperature_sensor mcu_temp"};
        discovery_.fans() = {"heater_fan hotend_fan", "fan", "fan_generic chamber_fan"};
        discovery_.leds() = {"led chamber_led"};
        break;

    case PrinterType::GENERIC_COREXY:
        // Generic CoreXY printer
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor raspberry_pi"};
        discovery_.fans() = {"heater_fan hotend_fan", "fan"};
        discovery_.leds() = {"neopixel chamber_led"};
        break;

    case PrinterType::GENERIC_BEDSLINGER:
        // Generic i3-style bedslinger
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {
            "heater_bed", // Bed thermistor (Klipper naming: bare heater name)
            "extruder"    // Hotend thermistor (Klipper naming: bare heater name)
        };
        discovery_.fans() = {"heater_fan hotend_fan", "fan"};
        discovery_.leds() = {};
        break;

    case PrinterType::MULTI_EXTRUDER:
        // Multi-extruder test case
        discovery_.heaters() = {"heater_bed", "extruder", "extruder1"};
        discovery_.sensors() = {
            "heater_bed", // Bed thermistor (Klipper naming: bare heater name)
            "extruder",   // Hotend thermistor primary (Klipper naming: bare heater name)
            "extruder1",  // Hotend thermistor secondary (Klipper naming: bare heater name)
            "temperature_sensor chamber", "temperature_sensor mcu_temp"};
        discovery_.fans() = {"heater_fan hotend_fan", "heater_fan hotend_fan1", "fan",
                             "fan_generic exhaust_fan"};
        discovery_.leds() = {"neopixel chamber_light"};
        break;
    }

    // Toolchanger mock mode (HELIX_MOCK_AMS=toolchanger): emulate a real Klipper
    // toolchanger with 4 distinct extruder heaters so ToolState::init_tools()
    // maps each of the 4 tools to its own extruder and the Nozzle Temps widget
    // renders the full 4-row multi-extruder layout. The bare "extruder" is
    // already present from the printer-type switch above; add extruder1/2/3.
    // Gated entirely on is_mock_toolchanger() so other AMS modes are unaffected.
    // MedusaHC joins this: a hotend changer is one hot end (and so one extruder
    // heater) per tool, which is also how PrinterDiscovery names its tools when
    // the fork variant ships no [tool N] objects.
    if (is_mock_toolchanger() || is_mock_medusahc()) {
        for (const char* ext : {"extruder1", "extruder2", "extruder3"}) {
            // Klipper exposes secondary extruders as both a heater and a sensor
            // under the bare name (matching the MULTI_EXTRUDER case convention).
            if (std::find(discovery_.heaters().begin(), discovery_.heaters().end(), ext) ==
                discovery_.heaters().end()) {
                discovery_.heaters().push_back(ext);
            }
            if (std::find(discovery_.sensors().begin(), discovery_.sensors().end(), ext) ==
                discovery_.sensors().end()) {
                discovery_.sensors().push_back(ext);
            }
        }
        spdlog::debug("[MoonrakerClientMock] {} mock: registered 4 extruders "
                      "(extruder, extruder1, extruder2, extruder3)",
                      is_mock_medusahc() ? "MedusaHC" : "Toolchanger");
    }

    // Initialize LED states (all off by default)
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        led_states_.clear();
        for (const auto& led : discovery_.leds()) {
            if (led.rfind("output_pin ", 0) == 0)
                continue; // output_pin uses {value:} not color_data
            led_states_[led] = LedColor{0.0, 0.0, 0.0, 0.0};
        }
    }

    spdlog::trace("[MoonrakerClientMock] Populated hardware:");
    for (const auto& h : discovery_.heaters())
        spdlog::trace("  Heater: {}", h);
    for (const auto& s : discovery_.sensors())
        spdlog::trace("  Sensor: {}", s);
    for (const auto& f : discovery_.fans())
        spdlog::trace("  Fan: {}", f);
    for (const auto& l : discovery_.leds())
        spdlog::trace("  LED: {}", l);
}

void MoonrakerClientMock::parse_incoming_bed_mesh(const json& bed_mesh) {
    // Parse bed mesh JSON from dispatch_status_update into active_bed_mesh_
    // This mirrors the JSON format sent by real Moonraker

    // Parse profile name
    if (bed_mesh.contains("profile_name")) {
        if (bed_mesh["profile_name"].is_string()) {
            active_bed_mesh_.name = bed_mesh["profile_name"].get<std::string>();
        } else if (bed_mesh["profile_name"].is_null()) {
            active_bed_mesh_.name = "";
        }
    }

    // Parse probed_matrix (2D array of Z heights)
    if (bed_mesh.contains("probed_matrix") && bed_mesh["probed_matrix"].is_array()) {
        active_bed_mesh_.probed_matrix.clear();
        const auto& matrix = bed_mesh["probed_matrix"];

        for (const auto& row : matrix) {
            if (!row.is_array()) {
                continue;
            }
            std::vector<float> row_vec;
            for (const auto& val : row) {
                if (val.is_number()) {
                    row_vec.push_back(val.get<float>());
                }
                // Skip non-numeric values (strings, nulls)
            }
            active_bed_mesh_.probed_matrix.push_back(row_vec);
        }

        // Update counts based on parsed matrix
        if (!active_bed_mesh_.probed_matrix.empty()) {
            active_bed_mesh_.y_count = static_cast<int>(active_bed_mesh_.probed_matrix.size());
            active_bed_mesh_.x_count = static_cast<int>(active_bed_mesh_.probed_matrix[0].size());
        } else {
            active_bed_mesh_.x_count = 0;
            active_bed_mesh_.y_count = 0;
        }
    }

    // Parse mesh_min (array [x, y])
    if (bed_mesh.contains("mesh_min") && bed_mesh["mesh_min"].is_array() &&
        bed_mesh["mesh_min"].size() >= 2) {
        if (bed_mesh["mesh_min"][0].is_number()) {
            active_bed_mesh_.mesh_min[0] = bed_mesh["mesh_min"][0].get<float>();
        }
        if (bed_mesh["mesh_min"][1].is_number()) {
            active_bed_mesh_.mesh_min[1] = bed_mesh["mesh_min"][1].get<float>();
        }
    }

    // Parse mesh_max (array [x, y])
    if (bed_mesh.contains("mesh_max") && bed_mesh["mesh_max"].is_array() &&
        bed_mesh["mesh_max"].size() >= 2) {
        if (bed_mesh["mesh_max"][0].is_number()) {
            active_bed_mesh_.mesh_max[0] = bed_mesh["mesh_max"][0].get<float>();
        }
        if (bed_mesh["mesh_max"][1].is_number()) {
            active_bed_mesh_.mesh_max[1] = bed_mesh["mesh_max"][1].get<float>();
        }
    }

    // Parse algorithm from mesh_params
    if (bed_mesh.contains("mesh_params") && bed_mesh["mesh_params"].is_object()) {
        const auto& params = bed_mesh["mesh_params"];
        if (params.contains("algo") && params["algo"].is_string()) {
            active_bed_mesh_.algo = params["algo"].get<std::string>();
        }
    }

    // Parse profiles list
    if (bed_mesh.contains("profiles") && bed_mesh["profiles"].is_object()) {
        bed_mesh_profiles_.clear();
        for (const auto& [key, value] : bed_mesh["profiles"].items()) {
            bed_mesh_profiles_.push_back(key);
        }
    }

    spdlog::debug("[MoonrakerClientMock] Parsed incoming bed mesh: profile='{}', size={}x{}",
                  active_bed_mesh_.name, active_bed_mesh_.x_count, active_bed_mesh_.y_count);
}

void MoonrakerClientMock::generate_mock_bed_mesh() {
    // Helper lambda to generate a mesh with given shape parameters
    auto generate_mesh = [](const std::string& name, float amplitude, float x_tilt,
                            float y_tilt) -> BedMeshProfile {
        BedMeshProfile mesh;
        mesh.name = name;
        mesh.mesh_min[0] = static_cast<float>(mock_internal::MOCK_MESH_X_MIN);
        mesh.mesh_min[1] = static_cast<float>(mock_internal::MOCK_MESH_Y_MIN);
        mesh.mesh_max[0] = static_cast<float>(mock_internal::MOCK_MESH_X_MAX);
        mesh.mesh_max[1] = static_cast<float>(mock_internal::MOCK_MESH_Y_MAX);
        mesh.x_count = 7;
        mesh.y_count = 7;
        mesh.algo = "lagrange";

        float center_x = mesh.x_count / 2.0f;
        float center_y = mesh.y_count / 2.0f;
        float max_radius = std::min(center_x, center_y);

        for (int row = 0; row < mesh.y_count; row++) {
            std::vector<float> row_vec;
            for (int col = 0; col < mesh.x_count; col++) {
                float dx = col - center_x;
                float dy = row - center_y;
                float dist = std::sqrt(dx * dx + dy * dy);

                // Dome shape + optional tilt
                float normalized_dist = dist / max_radius;
                float height = amplitude * (1.0f - normalized_dist * normalized_dist);
                height += x_tilt * (col - center_x) / center_x * 0.1f;
                height += y_tilt * (row - center_y) / center_y * 0.1f;

                row_vec.push_back(height);
            }
            mesh.probed_matrix.push_back(row_vec);
        }
        return mesh;
    };

    // Generate "default" profile: centered dome, 0.3mm amplitude
    stored_bed_mesh_profiles_["default"] = generate_mesh("default", 0.3f, 0.0f, 0.0f);

    // Generate "adaptive" profile: dome with slight tilt, different amplitude
    stored_bed_mesh_profiles_["adaptive"] = generate_mesh("adaptive", 0.25f, 0.5f, -0.3f);

    // Set profile name list
    bed_mesh_profiles_ = {"default", "adaptive"};

    // Load "default" as active
    active_bed_mesh_ = stored_bed_mesh_profiles_["default"];

    spdlog::debug("[MoonrakerClientMock] Generated {} bed mesh profiles, active='{}'",
                  stored_bed_mesh_profiles_.size(), active_bed_mesh_.name);
}

void MoonrakerClientMock::generate_mock_bed_mesh_with_variation() {
    // Generate a realistic bed mesh with true randomness
    // Simulates re-probing with measurement noise and slight bed changes

    // Keep existing configuration using centralized mock printer constants
    active_bed_mesh_.mesh_min[0] = static_cast<float>(mock_internal::MOCK_MESH_X_MIN);
    active_bed_mesh_.mesh_min[1] = static_cast<float>(mock_internal::MOCK_MESH_Y_MIN);
    active_bed_mesh_.mesh_max[0] = static_cast<float>(mock_internal::MOCK_MESH_X_MAX);
    active_bed_mesh_.mesh_max[1] = static_cast<float>(mock_internal::MOCK_MESH_Y_MAX);
    active_bed_mesh_.x_count = 7;
    active_bed_mesh_.y_count = 7;
    active_bed_mesh_.algo = "lagrange";

    // Seed with both random_device and a monotonic counter to guarantee
    // distinct meshes even if random_device is deterministic (some Linux/embedded platforms)
    static std::atomic<uint32_t> calibration_counter{0};
    std::random_device rd;
    std::seed_seq seed{
        rd(), rd(), calibration_counter.fetch_add(1),
        static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    std::mt19937 gen(seed);
    std::uniform_real_distribution<float> noise(-0.03f, 0.03f);      // ±0.03mm probe noise
    std::uniform_real_distribution<float> amplitude(0.15f, 0.35f);   // Overall dome height
    std::uniform_real_distribution<float> tilt(-0.08f, 0.08f);       // Bed tilt per axis
    std::uniform_real_distribution<float> center_shift(-0.5f, 0.5f); // Dome center offset

    // Random parameters for this calibration
    float dome_amp = amplitude(gen);
    float x_tilt = tilt(gen);
    float y_tilt = tilt(gen);
    float cx_shift = center_shift(gen);
    float cy_shift = center_shift(gen);

    active_bed_mesh_.probed_matrix.clear();
    float center_x = active_bed_mesh_.x_count / 2.0f + cx_shift;
    float center_y = active_bed_mesh_.y_count / 2.0f + cy_shift;
    float max_radius = std::min(active_bed_mesh_.x_count, active_bed_mesh_.y_count) / 2.0f;

    for (int row = 0; row < active_bed_mesh_.y_count; row++) {
        std::vector<float> row_vec;
        for (int col = 0; col < active_bed_mesh_.x_count; col++) {
            float dx = col - center_x;
            float dy = row - center_y;
            float dist = std::sqrt(dx * dx + dy * dy);

            // Base dome shape
            float normalized_dist = dist / max_radius;
            float height = dome_amp * (1.0f - normalized_dist * normalized_dist);

            // Add bed tilt (simulates unlevel bed)
            float norm_x = static_cast<float>(col) / (active_bed_mesh_.x_count - 1) - 0.5f;
            float norm_y = static_cast<float>(row) / (active_bed_mesh_.y_count - 1) - 0.5f;
            height += x_tilt * norm_x + y_tilt * norm_y;

            // Add per-point probe noise (simulates measurement uncertainty)
            height += noise(gen);

            row_vec.push_back(height);
        }
        active_bed_mesh_.probed_matrix.push_back(row_vec);
    }

    spdlog::debug("[MoonrakerClientMock] Regenerated bed mesh: amp={:.3f}, tilt=({:.3f},{:.3f})",
                  dome_amp, x_tilt, y_tilt);
}

void MoonrakerClientMock::dispatch_bed_mesh_update() {
    // Build bed mesh JSON in Moonraker format
    json probed_matrix_json = json::array();
    for (const auto& row : active_bed_mesh_.probed_matrix) {
        json row_json = json::array();
        for (float val : row) {
            row_json.push_back(val);
        }
        probed_matrix_json.push_back(row_json);
    }

    json profiles_json = json::object();
    for (const auto& [name, profile] : stored_bed_mesh_profiles_) {
        // Build points array for this profile
        json points_json = json::array();
        for (const auto& row : profile.probed_matrix) {
            json row_json = json::array();
            for (float val : row) {
                row_json.push_back(val);
            }
            points_json.push_back(row_json);
        }

        profiles_json[name] = {{"points", points_json},
                               {"mesh_params",
                                {{"min_x", profile.mesh_min[0]},
                                 {"min_y", profile.mesh_min[1]},
                                 {"max_x", profile.mesh_max[0]},
                                 {"max_y", profile.mesh_max[1]},
                                 {"x_count", profile.x_count},
                                 {"y_count", profile.y_count}}}};
    }

    json bed_mesh_status = {
        {"bed_mesh",
         {{"profile_name", active_bed_mesh_.name},
          {"probed_matrix", probed_matrix_json},
          {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
          {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
          {"profiles", profiles_json},
          {"mesh_params", {{"algo", active_bed_mesh_.algo}}}}}};

    // Dispatch via base class method
    dispatch_status_update(bed_mesh_status);
}

void MoonrakerClientMock::disconnect() {
    spdlog::info("[MoonrakerClientMock] Simulating disconnection");
    stop_temperature_simulation(false);
    set_connection_state(ConnectionState::DISCONNECTED);
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {}", method);
    return 0; // Success
}

int MoonrakerClientMock::send_jsonrpc(const std::string& method,
                                      [[maybe_unused]] const json& params) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with params)", method);
    return 0; // Success
}

RequestId MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                            std::function<void(const json&)> cb) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with callback)", method);

    // Dispatch to handler registry (wrap callback to match error_cb signature)
    auto noop_error_cb = [](const MoonrakerError&) {};
    return send_jsonrpc(method, params, cb, noop_error_cb);
}

RequestId MoonrakerClientMock::send_jsonrpc(const std::string& method, const json& params,
                                            std::function<void(const json&)> success_cb,
                                            std::function<void(const MoonrakerError&)> error_cb,
                                            uint32_t timeout_ms, bool silent,
                                            std::optional<rpc_error_policy::CallerIntent> intent) {
    spdlog::trace("[MoonrakerClientMock] Mock send_jsonrpc: {} (with success/error callbacks)",
                  method);

    // Same fallback inference MoonrakerRequestTracker::send() applies, so a
    // handler asking rpc_error_policy::decide() gets the hardware answer.
    current_send_intent_ =
        intent.value_or(rpc_error_policy::CallerIntent{silent, error_cb != nullptr});

    // Capture for test inspection — used by tests verifying that specific callers pass
    // non-default timeout/silent values (e.g. exclude_object which must be silent+long).
    last_send_method_ = method;
    last_send_timeout_ms_ = timeout_ms;
    last_send_silent_ = silent;
    if (method == "printer.gcode.script" && params.contains("script") &&
        params["script"].is_string()) {
        last_send_script_ = params["script"].get<std::string>();
    }

    // Dispatch to method handler registry
    auto it = method_handlers_.find(method);
    if (it != method_handlers_.end()) {
        it->second(this, params, success_cb, error_cb);
        return next_mock_request_id();
    }

    // Unimplemented methods - log warning
    spdlog::debug("[MoonrakerClientMock] Method '{}' not implemented - callbacks not invoked",
                  method);
    return next_mock_request_id();
}

// Removed old implementation - now handled by method_handlers_ registry:
// Lines 527-916 deleted (file/print/objects/history handlers moved to separate modules)
// See: moonraker_client_mock_files.cpp, moonraker_client_mock_print.cpp,
//      moonraker_client_mock_objects.cpp, moonraker_client_mock_history.cpp
//
// Old logic was:
//   - server.files.* handlers (list, metadata, delete, move, copy, post_directory,
//   delete_directory)
//   - printer.gcode.script handler
//   - printer.print.* handlers (start, pause, resume, cancel)
//   - printer.objects.query handler
//   - server.history.* handlers (list, totals, delete_job)
//

std::string MoonrakerClientMock::get_last_gcode_error() const {
    std::lock_guard<std::mutex> lock(gcode_error_mutex_);
    return last_gcode_error_;
}

namespace {
// Real Klipper uppercases only the leading command word before dispatch
// (M117, m117, and M117 all route to the same handler) - it never touches
// anything after that word. Mirror that here: every branch below matches on
// the command name via gcode.find(...)/gcode == ..., so normalizing just the
// token up to the first whitespace lets lowercase/mixed-case commands (as
// typed in the console, or emitted by some slicers) match the same way they
// would on real hardware, without mangling M117 message text, filenames
// (SDCARD_PRINT_FILE FILENAME=...), or any other argument payload.
std::string normalize_gcode_command_case(const std::string& raw) {
    size_t token_end = raw.find_first_of(" \t");
    std::string result = raw;
    size_t end = (token_end == std::string::npos) ? raw.size() : token_end;
    for (size_t i = 0; i < end; ++i) {
        result[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[i])));
    }
    return result;
}
} // namespace

int MoonrakerClientMock::gcode_script(const std::string& raw_gcode) {
    spdlog::trace("[MoonrakerClientMock] Mock gcode_script: {}", raw_gcode);

    // Record for test inspection (ordered history of every script handled).
    // Uses the raw, un-normalized text so tests/logs see exactly what was sent.
    record_gcode_script(raw_gcode);

    // Normalize the command token only (see normalize_gcode_command_case above).
    // Every subsequent use of `gcode` in this function refers to this copy.
    const std::string gcode = normalize_gcode_command_case(raw_gcode);

    // Clear previous error at start
    {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_.clear();
    }

    // MedusaHC commands, handled before the generic chain below so the bare
    // OPEN/CLOSE feeder macros cannot be confused with a substring of anything
    // else: these match the COMMAND TOKEN exactly, not find() anywhere in the
    // line. Only armed in the MedusaHC mock modes.
    if (is_mock_medusahc()) {
        const size_t token_end = gcode.find_first_of(" \t");
        const std::string cmd = gcode.substr(0, token_end);
        auto tool_param = [&]() -> int {
            const size_t t = gcode.find("T=");
            if (t == std::string::npos) {
                return -1;
            }
            try {
                return std::stoi(gcode.substr(t + 2));
            } catch (...) {
                return -1;
            }
        };

        if (cmd == "OPEN" || cmd == "MHC_OPEN") {
            medusa_feeder_open_.store(true);
            spdlog::info("[MoonrakerClientMock] MedusaHC feeder opened ({})", cmd);
            return 0;
        }
        if (cmd == "CLOSE" || cmd == "MHC_CLOSE") {
            medusa_feeder_open_.store(false);
            spdlog::info("[MoonrakerClientMock] MedusaHC feeder closed ({})", cmd);
            return 0;
        }
        if (cmd == "SELECT_TOOL") {
            start_medusa_swap(tool_param());
            return 0;
        }
        if (cmd == "UNSELECT_TOOL" || cmd == "DROP_TOOL") {
            start_medusa_swap(-1);
            return 0;
        }
        // Bare T<n>: what the fork registers when it runs the swap itself.
        if (cmd.size() >= 2 && cmd[0] == 'T' &&
            std::all_of(cmd.begin() + 1, cmd.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            start_medusa_swap(std::stoi(cmd.substr(1)));
            return 0;
        }
    }

    // Parse temperature commands to update simulation targets
    // M104 Sxxx - Set extruder temp (no wait)
    // M109 Sxxx - Set extruder temp (wait)
    // M140 Sxxx - Set bed temp (no wait)
    // M190 Sxxx - Set bed temp (wait)
    // SET_HEATER_TEMPERATURE HEATER=extruder TARGET=xxx
    // SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=xxx

    // Check for Klipper-style SET_HEATER_TEMPERATURE commands
    if (gcode.find("SET_HEATER_TEMPERATURE") != std::string::npos) {
        double target = 0.0;
        size_t target_pos = gcode.find("TARGET=");
        if (target_pos != std::string::npos) {
            target = std::stod(gcode.substr(target_pos + 7));
        }

        if (gcode.find("HEATER=extruder") != std::string::npos) {
            set_extruder_target(target);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C", target);
            dispatch_status_update({{"extruder", {{"target", target}}}});
        } else if (gcode.find("HEATER=heater_bed") != std::string::npos) {
            set_bed_target(target);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Bed target set to {}°C", target);
            dispatch_status_update({{"heater_bed", {{"target", target}}}});
        } else if (gcode.find("HEATER=heater_generic ") != std::string::npos) {
            // Reject invalid format: Klipper expects bare object name, not "heater_generic chamber"
            spdlog::error(
                "[MoonrakerClientMock] Invalid SET_HEATER_TEMPERATURE: HEATER must use bare object "
                "name (e.g. HEATER=chamber), not prefixed type (HEATER=heater_generic chamber)");
            return 1;
        } else {
            // Chamber heater, matched by the BARE object name the resolved
            // chamber heater uses — "HEATER=chamber" for keyword heaters,
            // "HEATER=dragonbreath" for backend-named ones (a hard-coded
            // "chamber" comparison silently ignores those).
            const std::string bare = chamber_heater_bare_name();
            if (!bare.empty() && gcode.find("HEATER=" + bare) != std::string::npos) {
                set_chamber_target(target);
                reset_idle_timeout();
                spdlog::info("[MoonrakerClientMock] Chamber target set to {}°C", target);
                auto key = chamber_heater_status_key();
                if (!key.empty())
                    dispatch_status_update({{key, {{"target", target}}}});
            }
        }
    }
    // Check for SET_TEMPERATURE_FAN_TARGET (temperature_fan chamber heaters)
    else if (gcode.find("SET_TEMPERATURE_FAN_TARGET") != std::string::npos) {
        double target = 0.0;
        size_t target_pos = gcode.find("TARGET=");
        if (target_pos != std::string::npos) {
            target = std::stod(gcode.substr(target_pos + 7));
        }
        set_chamber_target(target);
        reset_idle_timeout();
        spdlog::info("[MoonrakerClientMock] Chamber (temperature_fan) target set to {}°C", target);
        auto key = chamber_heater_status_key();
        if (!key.empty())
            dispatch_status_update({{key, {{"target", target}}}});
    }
    // Check for SET_PIN (chamber filter fan: SET_PIN PIN=dragonbreath_filter VALUE=1)
    else if (gcode.find("SET_PIN") != std::string::npos) {
        const std::string pin_obj = chamber_filter_pin_object();
        if (!pin_obj.empty()) {
            const std::string bare_pin = pin_obj.substr(11); // strip "output_pin "
            if (gcode.find("PIN=" + bare_pin) != std::string::npos) {
                double value = 0.0;
                size_t value_pos = gcode.find("VALUE=");
                if (value_pos != std::string::npos) {
                    value = std::stod(gcode.substr(value_pos + 6));
                }
                chamber_filter_value_.store(value);
                reset_idle_timeout();
                spdlog::info("[MoonrakerClientMock] Chamber filter pin {} set to {}", bare_pin,
                             value);
                dispatch_status_update({{pin_obj, {{"value", value}}}});
            }
        }
    }
    // Check for M-code style temperature commands
    else if (gcode.find("M104") != std::string::npos || gcode.find("M109") != std::string::npos) {
        size_t s_pos = gcode.find('S');
        if (s_pos != std::string::npos) {
            double target = std::stod(gcode.substr(s_pos + 1));
            set_extruder_target(target);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C (M-code)", target);
            dispatch_status_update({{"extruder", {{"target", target}}}});
        }
    } else if (gcode.find("M140") != std::string::npos || gcode.find("M190") != std::string::npos) {
        size_t s_pos = gcode.find('S');
        if (s_pos != std::string::npos) {
            double target = std::stod(gcode.substr(s_pos + 1));
            set_bed_target(target);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Bed target set to {}°C (M-code)", target);
            dispatch_status_update({{"heater_bed", {{"target", target}}}});
        }
    }

    // M117 <message> - Set display message (LCD message on real printers).
    // Bare M117 (or M117 followed only by whitespace) clears the message.
    // Klipper strips exactly one leading space after the command, so
    // "M117 hello" -> "hello" but "M117  hello" (two spaces) -> " hello".
    if (gcode == "M117" || gcode.find("M117 ") == 0) {
        std::string text = gcode.substr(4);
        if (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }
        if (text.find_first_not_of(" \t") == std::string::npos) {
            text.clear();
        }
        {
            std::lock_guard<std::mutex> lock(display_message_mutex_);
            display_message_ = text;
            display_message_set_ = true;
        }
        spdlog::info("[MoonrakerClientMock] Display message set to \"{}\" (M117)", text);
        dispatch_status_update({{"display_status", {{"message", text}}}});
        return 0;
    }

    // Parse motion mode commands (G90/G91)
    // G90 - Absolute positioning mode
    // G91 - Relative positioning mode
    if (gcode.find("G90") != std::string::npos) {
        relative_mode_.store(false);
        spdlog::info("[MoonrakerClientMock] Set absolute positioning mode (G90)");
    } else if (gcode.find("G91") != std::string::npos) {
        relative_mode_.store(true);
        spdlog::info("[MoonrakerClientMock] Set relative positioning mode (G91)");
    }

    // M84 - Disable stepper motors (clears homed_axes + updates stepper_enable)
    if (gcode.find("M84") != std::string::npos || gcode.find("M18") != std::string::npos) {
        motors_enabled_.store(false);
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            homed_axes_.clear();
        }
        spdlog::info("[MoonrakerClientMock] Motors disabled (M84/M18), homed_axes cleared");
        // Dispatch toolhead with cleared homed_axes (primary motor state indicator)
        // and stepper_enable state change (fallback for printers that report it)
        json status = {{"toolhead", {{"homed_axes", ""}}},
                       {"stepper_enable",
                        {{"steppers",
                          {{"stepper_x", false},
                           {"stepper_y", false},
                           {"stepper_z", false},
                           {"extruder", false}}}}}};
        dispatch_status_update(status);
    }

    // Parse homing command (G28)
    // G28 - Home all axes
    // G28 X - Home X axis only
    // G28 Y - Home Y axis only
    // G28 Z - Home Z axis only
    // G28 X Y - Home X and Y axes
    if (gcode.find("G28") != std::string::npos) {
        // Re-enable motors when homing
        motors_enabled_.store(true);
        // Check if specific axes are mentioned after G28
        // Need to look after the G28 to avoid false matches
        size_t g28_pos = gcode.find("G28");
        std::string after_g28 = gcode.substr(g28_pos + 3);

        // Check for specific axis letters (case insensitive search)
        bool has_x =
            after_g28.find('X') != std::string::npos || after_g28.find('x') != std::string::npos;
        bool has_y =
            after_g28.find('Y') != std::string::npos || after_g28.find('y') != std::string::npos;
        bool has_z =
            after_g28.find('Z') != std::string::npos || after_g28.find('z') != std::string::npos;

        // If no specific axis mentioned, home all
        bool home_all = !has_x && !has_y && !has_z;

        std::string homed;
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            // Group all axis zeroing so a concurrent snapshot read never sees a
            // partially-homed position. Lock order is homed_axes_mutex_ ->
            // pos_mutex_ (pos_mutex_ is a leaf).
            std::lock_guard<std::mutex> pos_lock(pos_mutex_);

            if (home_all) {
                homed_axes_ = "xyz";
                pos_x_.store(0.0);
                pos_y_.store(0.0);
                pos_z_.store(0.0);
                spdlog::info("[MoonrakerClientMock] Homed all axes (G28), homed_axes='xyz'");
            } else {
                if (has_x) {
                    if (homed_axes_.find('x') == std::string::npos) {
                        homed_axes_ += 'x';
                    }
                    pos_x_.store(0.0);
                }
                if (has_y) {
                    if (homed_axes_.find('y') == std::string::npos) {
                        homed_axes_ += 'y';
                    }
                    pos_y_.store(0.0);
                }
                if (has_z) {
                    if (homed_axes_.find('z') == std::string::npos) {
                        homed_axes_ += 'z';
                    }
                    pos_z_.store(0.0);
                }
                spdlog::info("[MoonrakerClientMock] Homed axes: X={} Y={} Z={}, homed_axes='{}'",
                             has_x, has_y, has_z, homed_axes_);
            }
            homed = homed_axes_;
        }
        reset_idle_timeout();
        dispatch_status_update({{"toolhead", {{"homed_axes", homed}}}});
    }

    // Parse movement commands (G0/G1)
    // G0 X100 Y50 Z10 - Rapid move
    // G1 X100 Y50 Z10 E5 F3000 - Linear move (E and F ignored for now)
    if (gcode.find("G0") != std::string::npos || gcode.find("G1") != std::string::npos) {
        // Re-enable motors when moving
        motors_enabled_.store(true);
        bool is_relative = relative_mode_.load();

        // Position limits (typical Voron 2.4 350mm config)
        // Z allows slight negative for probe calibration
        constexpr double X_MIN = 0.0, X_MAX = 350.0;
        constexpr double Y_MIN = 0.0, Y_MAX = 350.0;
        constexpr double Z_MIN = -0.5, Z_MAX = 340.0;

        // Helper lambda to parse axis value from gcode string
        auto parse_axis = [&gcode](char axis) -> std::pair<bool, double> {
            // Look for the axis letter followed by a number
            size_t pos = gcode.find(axis);
            if (pos == std::string::npos) {
                // Try lowercase
                pos = gcode.find(static_cast<char>(axis + 32));
            }
            if (pos != std::string::npos && pos + 1 < gcode.length()) {
                // Skip any spaces after the axis letter
                size_t value_start = pos + 1;
                while (value_start < gcode.length() && gcode[value_start] == ' ') {
                    value_start++;
                }
                if (value_start < gcode.length()) {
                    try {
                        double value = std::stod(gcode.substr(value_start));
                        return {true, value};
                    } catch (...) {
                        // Parse error, ignore this axis
                    }
                }
            }
            return {false, 0.0};
        };

        auto [has_x, x_val] = parse_axis('X');
        auto [has_y, y_val] = parse_axis('Y');
        auto [has_z, z_val] = parse_axis('Z');

        // Calculate target positions
        double target_x = has_x ? (is_relative ? pos_x_.load() + x_val : x_val) : pos_x_.load();
        double target_y = has_y ? (is_relative ? pos_y_.load() + y_val : y_val) : pos_y_.load();
        double target_z = has_z ? (is_relative ? pos_z_.load() + z_val : z_val) : pos_z_.load();

        // Check limits (like real Klipper)
        bool out_of_range = false;
        std::string error_detail;
        if (target_x < X_MIN || target_x > X_MAX) {
            error_detail = "Move out of range: X=" + std::to_string(target_x);
            out_of_range = true;
        } else if (target_y < Y_MIN || target_y > Y_MAX) {
            error_detail = "Move out of range: Y=" + std::to_string(target_y);
            out_of_range = true;
        } else if (target_z < Z_MIN || target_z > Z_MAX) {
            error_detail = "Move out of range: Z=" + std::to_string(target_z);
            out_of_range = true;
        }

        if (out_of_range) {
            // The two channels carry the SAME rejection in different shapes, and
            // real Moonraker is the spec for both:
            //   - broadcast gcode-response stream: `!!`-prefixed (error_classify
            //     strips the prefix before the router sees it);
            //   - JSON-RPC error `message`: no prefix at all.
            // Storing the prefixed form in the latch made the RPC message
            // "!! Move out of range: ..." — a string the `!!` channel could never
            // match, silently defeating the cross-source toast dedup.
            const std::string broadcast_line = "!! " + error_detail;
            dispatch_gcode_response(broadcast_line);
            spdlog::warn("[MoonrakerClientMock] Move rejected - {}", broadcast_line);
            // Store error for RPC handler to return proper error response (like real Moonraker)
            {
                std::lock_guard<std::mutex> lock(gcode_error_mutex_);
                last_gcode_error_ = error_detail;
            }
        } else {
            // Apply the move as a group so the background simulation loop never
            // broadcasts a torn snapshot (e.g. X/Y updated but Z still stale).
            {
                std::lock_guard<std::mutex> pos_lock(pos_mutex_);
                if (has_x)
                    pos_x_.store(target_x);
                if (has_y)
                    pos_y_.store(target_y);
                if (has_z)
                    pos_z_.store(target_z);
            }

            if (has_x || has_y || has_z) {
                double sx, sy, sz;
                read_position_snapshot(sx, sy, sz);
                spdlog::debug("[MoonrakerClientMock] Move {} X={} Y={} Z={} (mode={})",
                              gcode.find("G0") != std::string::npos ? "G0" : "G1", sx, sy, sz,
                              is_relative ? "relative" : "absolute");
                // Reset idle timeout when moving
                reset_idle_timeout();
                // Dispatch immediate position update (matches real Moonraker)
                dispatch_status_update({{"toolhead", {{"position", {sx, sy, sz, 0.0}}}}});
            }
        }
    }

    // Parse Tn tool change commands (T0, T1, T2, ...)
    // Klipper maps Tn to ACTIVATE_EXTRUDER for multi-extruder setups;
    // toolchanger plugins override Tn with physical tool change logic.
    // The mock simulates the result: dispatch toolhead.extruder status update.
    if (gcode.length() >= 2 && gcode[0] == 'T' && std::isdigit(gcode[1])) {
        try {
            int tool_index = std::stoi(gcode.substr(1));
            // Build extruder name: T0 -> "extruder", T1 -> "extruder1", T2 -> "extruder2", ...
            std::string extruder_name =
                (tool_index == 0) ? "extruder" : ("extruder" + std::to_string(tool_index));
            spdlog::info("[MoonrakerClientMock] Tool change {} -> active extruder: {}", gcode,
                         extruder_name);

            // Dispatch toolhead.extruder update (same as real Klipper)
            json status = {{"toolhead", {{"extruder", extruder_name}}}};
            dispatch_status_update(status);
        } catch (...) {
            spdlog::warn("[MoonrakerClientMock] Failed to parse tool index from: {}", gcode);
        }
    }

    // Parse print job commands (delegate to unified internal handlers)
    // SDCARD_PRINT_FILE FILENAME=xxx - Start printing a file
    if (gcode.find("SDCARD_PRINT_FILE") != std::string::npos) {
        size_t filename_pos = gcode.find("FILENAME=");
        if (filename_pos != std::string::npos) {
            size_t start = filename_pos + 9;
            std::string filename;
            if (start < gcode.size() && gcode[start] == '"') {
                // Quoted value, as Klipper's shlex tokenizer expects whenever
                // the name contains spaces (Moonraker and the PLR resume path
                // both always quote). Runs to the closing quote; the quotes
                // themselves are not part of the name.
                size_t end = gcode.find('"', start + 1);
                filename = (end != std::string::npos) ? gcode.substr(start + 1, end - start - 1)
                                                      : gcode.substr(start + 1);
            } else {
                // Bare value: ends at the next whitespace.
                size_t end = gcode.find(' ', start);
                filename = (end != std::string::npos) ? gcode.substr(start, end - start)
                                                      : gcode.substr(start);
            }

            // Use unified internal handler
            start_print_internal(filename);
        }
    }
    // PAUSE - Pause current print
    else if (gcode == "PAUSE" || gcode.find("PAUSE ") == 0) {
        pause_print_internal();
    }
    // RESUME - Resume paused print
    else if (gcode == "RESUME" || gcode.find("RESUME ") == 0) {
        resume_print_internal();
    }
    // CANCEL_PRINT - Cancel current print
    else if (gcode == "CANCEL_PRINT" || gcode.find("CANCEL_PRINT ") == 0) {
        cancel_print_internal();
    }
    // M112 - Emergency stop
    else if (gcode.find("M112") != std::string::npos) {
        emergency_stop_internal();
    }

    // ========================================================================
    // UNIMPLEMENTED G-CODE STUBS - Log warnings for missing features
    // ========================================================================

    // Fan control - M106/M107/SET_FAN_SPEED
    // M106 P0 S128 - Set fan index 0 to 50% (S is 0-255, P is fan index)
    if (gcode.find("M106") != std::string::npos) {
        int fan_index = 0;
        int speed_value = 0;

        // Parse P parameter (fan index)
        auto p_pos = gcode.find('P');
        if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
            try {
                fan_index = std::stoi(gcode.substr(p_pos + 1));
            } catch (...) {
            }
        }

        // Parse S parameter (speed 0-255)
        auto s_pos = gcode.find('S');
        if (s_pos != std::string::npos && s_pos + 1 < gcode.length()) {
            try {
                speed_value = std::stoi(gcode.substr(s_pos + 1));
                speed_value = std::clamp(speed_value, 0, 255);
            } catch (...) {
            }
        }

        // Convert to normalized speed (0.0-1.0)
        double normalized_speed = speed_value / 255.0;

        // Fan index 0 = "fan", index 1+ = "fan1", "fan2", etc.
        std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
        set_fan_speed_internal(fan_name, normalized_speed);

        spdlog::trace("[MoonrakerClientMock] M106 P{} S{} -> {} speed={:.2f}", fan_index,
                      speed_value, fan_name, normalized_speed);
    }
    // M107 - Turn off fan
    else if (gcode.find("M107") != std::string::npos) {
        int fan_index = 0;

        auto p_pos = gcode.find('P');
        if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
            try {
                fan_index = std::stoi(gcode.substr(p_pos + 1));
            } catch (...) {
            }
        }

        std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
        set_fan_speed_internal(fan_name, 0.0);

        spdlog::info("[MoonrakerClientMock] M107 P{} -> {} off", fan_index, fan_name);
    }
    // SET_FAN_SPEED - Klipper extended fan control
    // SET_FAN_SPEED FAN=nevermore SPEED=0.5
    else if (gcode.find("SET_FAN_SPEED") != std::string::npos) {
        std::string fan_name;
        double speed = 0.0;

        // Parse FAN parameter
        auto fan_pos = gcode.find("FAN=");
        if (fan_pos != std::string::npos) {
            size_t start = fan_pos + 4;
            size_t end = gcode.find_first_of(" \t\n", start);
            fan_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }

        // Parse SPEED parameter (0.0-1.0)
        auto speed_pos = gcode.find("SPEED=");
        if (speed_pos != std::string::npos) {
            try {
                speed = std::stod(gcode.substr(speed_pos + 6));
                speed = std::clamp(speed, 0.0, 1.0);
            } catch (...) {
            }
        }

        if (!fan_name.empty()) {
            // Try to find matching fan in discovered fans list
            std::string full_fan_name = find_fan_by_suffix(fan_name);
            if (!full_fan_name.empty()) {
                set_fan_speed_internal(full_fan_name, speed);
                spdlog::info("[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f}",
                             full_fan_name, speed);
            } else {
                // Use short name if no match found
                set_fan_speed_internal(fan_name, speed);
                spdlog::info(
                    "[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f} (unmatched fan)",
                    fan_name, speed);
            }
        }
    }

    // Extrusion control (NOT IMPLEMENTED)
    if (gcode.find("G92") != std::string::npos && gcode.find('E') != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: G92 E (set extruder position) NOT IMPLEMENTED");
    }
    if ((gcode.find("G0") != std::string::npos || gcode.find("G1") != std::string::npos) &&
        gcode.find('E') != std::string::npos) {
        spdlog::debug("[MoonrakerClientMock] Note: Extrusion (E parameter) ignored in G0/G1");
    }

    // PID Calibration simulation
    if (gcode.find("PID_CALIBRATE") != std::string::npos) {
        // Parse HEATER= parameter
        std::string heater = "extruder";
        auto heater_pos = gcode.find("HEATER=");
        if (heater_pos != std::string::npos) {
            size_t start = heater_pos + 7;
            size_t end = gcode.find(' ', start);
            heater =
                gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }

        // Parse TARGET= parameter
        int target = 200;
        auto target_pos = gcode.find("TARGET=");
        if (target_pos != std::string::npos) {
            target = std::stoi(gcode.substr(target_pos + 7));
        }

        spdlog::info("[MoonrakerClientMock] PID_CALIBRATE: heater={} target={}°C", heater, target);

        // Simulate PID calibration with a background timer
        struct PIDSimState {
            MoonrakerClientMock* mock;
            std::string heater;
            int target;
            int cycle;
        };

        auto* sim = new PIDSimState{this, heater, target, 0};

        lv_timer_t* timer = lv_timer_create(
            [](lv_timer_t* t) {
                auto* s = static_cast<PIDSimState*>(lv_timer_get_user_data(t));
                s->cycle++;

                if (s->cycle <= 5) {
                    // Simulate Kalico PID sample output (matches pid_calibrate.py format)
                    char buf[128];
                    float pwm = 0.5f - (s->cycle * 0.02f);
                    float asymmetry = 0.3f - (s->cycle * 0.05f);
                    // First two samples have n/a tolerance, then converging values
                    if (s->cycle <= 2) {
                        snprintf(buf, sizeof(buf),
                                 "sample:%d pwm:%.3f asymmetry:%.3f tolerance:n/a", s->cycle, pwm,
                                 asymmetry);
                    } else {
                        float tolerance = 0.1f / s->cycle;
                        snprintf(buf, sizeof(buf),
                                 "sample:%d pwm:%.3f asymmetry:%.3f tolerance:%.4f", s->cycle, pwm,
                                 asymmetry, tolerance);
                    }
                    s->mock->dispatch_gcode_response(buf);
                } else {
                    // Emit final PID result matching real Klipper format
                    float kp, ki, kd;
                    if (s->heater == "heater_bed") {
                        kp = 73.517f;
                        ki = 1.132f;
                        kd = 1194.093f;
                    } else {
                        kp = 22.865f;
                        ki = 1.292f;
                        kd = 101.178f;
                    }

                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "PID parameters: pid_Kp=%.3f pid_Ki=%.3f pid_Kd=%.3f", kp, ki, kd);
                    s->mock->dispatch_gcode_response(buf);

                    delete s;
                    lv_timer_delete(t);
                    return;
                }
            },
            500, sim);                       // 500ms between cycles for quick mock
        lv_timer_set_repeat_count(timer, 6); // 5 progress + 1 result
        calibration_timers_.push_back({timer, [sim] { delete sim; }});

        return 0; // Success - results come asynchronously via gcode_response
    }

    // MPC Calibration simulation
    if (gcode.find("MPC_CALIBRATE") != std::string::npos) {
        std::string heater = "extruder";
        auto heater_pos = gcode.find("HEATER=");
        if (heater_pos != std::string::npos) {
            size_t start = heater_pos + 7;
            size_t end = gcode.find(' ', start);
            heater =
                gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }

        int fan_breakpoints = 3;
        auto fb_pos = gcode.find("FAN_BREAKPOINTS=");
        if (fb_pos != std::string::npos) {
            fan_breakpoints = std::stoi(gcode.substr(fb_pos + 16));
        }

        spdlog::info("[MoonrakerClientMock] MPC_CALIBRATE: heater={} fan_breakpoints={}", heater,
                     fan_breakpoints);

        struct MPCSimState {
            MoonrakerClientMock* mock;
            std::string heater;
            int fan_breakpoints;
            int phase;
            int total_phases;
        };

        int total_phases = 3 + fan_breakpoints; // settle + heatup + fan phases
        auto* sim = new MPCSimState{this, heater, fan_breakpoints, 0, total_phases};

        lv_timer_t* timer = lv_timer_create(
            [](lv_timer_t* t) {
                auto* s = static_cast<MPCSimState*>(lv_timer_get_user_data(t));
                s->phase++;

                if (s->phase == 1) {
                    s->mock->dispatch_gcode_response("Waiting for heater to settle near ambient");
                } else if (s->phase == 2) {
                    s->mock->dispatch_gcode_response("Performing heatup test");
                } else if (s->phase <= 2 + s->fan_breakpoints) {
                    int fan_pct = ((s->phase - 2) * 100) / s->fan_breakpoints;
                    char buf[128];
                    snprintf(buf, sizeof(buf), "measuring power usage with %d%% fan", fan_pct);
                    s->mock->dispatch_gcode_response(buf);
                } else {
                    // Final result
                    s->mock->dispatch_gcode_response("Finished MPC calibration");
                    s->mock->dispatch_gcode_response("block_heat_capacity=18.4321 [J/K]");
                    s->mock->dispatch_gcode_response("sensor_responsiveness=0.123456 [K/s/K]");
                    s->mock->dispatch_gcode_response("ambient_transfer=0.045678 [W/K]");
                    if (s->fan_breakpoints > 0) {
                        s->mock->dispatch_gcode_response(
                            "fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]");
                    }

                    delete s;
                    lv_timer_delete(t);
                    return;
                }
            },
            500, sim);
        lv_timer_set_repeat_count(timer, total_phases + 1);
        calibration_timers_.push_back({timer, [sim] { delete sim; }});

        return 0; // Success - results come asynchronously via gcode_response
    }

    // SAVE_CONFIG simulation.
    //
    // Klipper's cmd_SAVE_CONFIG writes printer.cfg and then calls
    // request_restart('restart') as its last act, so it never acks the command --
    // the connection it would ack through is already going down. Moonraker fails
    // the pending printer.gcode.script with 503 "Klippy Disconnected". A caller
    // therefore sees a FAILED rpc for a save that succeeded, and learns the truth
    // only from klippy returning READY.
    //
    // Modelling both halves matters. The previous stub dispatched "ok" (which real
    // Klipper never sends) and returned non-zero WITHOUT setting last_gcode_error_,
    // so the error carried an empty message and klippy never moved. That produced
    // the field symptom by accident and made it unreadable: --test showed the same
    // false failure toast users hit, for an unrelated reason, so no test could tell
    // a fixed panel from a broken one.
    if (gcode.find("SAVE_CONFIG") != std::string::npos) {
        spdlog::info("[MoonrakerClientMock] SAVE_CONFIG - config written; restarting, and its "
                     "RPC is dropped (as on real Klipper)");
        // Write the file BEFORE restarting, in that order, because the restart
        // reloads every runtime value from the durable stores. Committing after
        // would reload the pre-save values and throw the save away.
        commit_pending_config();
        trigger_restart(/*is_firmware=*/false);
        {
            std::lock_guard<std::mutex> lock(gcode_error_mutex_);
            last_gcode_error_ = "Klippy Disconnected";
        }
        return 1;
    }

    // Bed mesh commands
    if (gcode.find("BED_MESH_CALIBRATE") != std::string::npos) {
        // Parse optional PROFILE= parameter
        std::string profile_name = "default";
        auto profile_pos = gcode.find("PROFILE=");
        if (profile_pos != std::string::npos) {
            size_t start = profile_pos + 8; // Length of "PROFILE="
            size_t end = gcode.find_first_of(" \t\n", start);
            profile_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }

        // Regenerate mesh with slight random variation
        active_bed_mesh_.name = profile_name;
        generate_mock_bed_mesh_with_variation();

        // Add new profile to list if not already present
        if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
            bed_mesh_profiles_.end()) {
            bed_mesh_profiles_.push_back(profile_name);
        }
        // Store the calibrated mesh
        stored_bed_mesh_profiles_[profile_name] = active_bed_mesh_;

        spdlog::info(
            "[MoonrakerClientMock] BED_MESH_CALIBRATE: generated new mesh for profile '{}'",
            profile_name);

        // Dispatch bed mesh update notification
        dispatch_bed_mesh_update();

    } else if (gcode.find("BED_MESH_PROFILE") != std::string::npos) {
        // Parse LOAD= or SAVE= or REMOVE= parameter
        if (gcode.find("LOAD=") != std::string::npos) {
            auto load_pos = gcode.find("LOAD=");
            size_t start = load_pos + 5; // Length of "LOAD="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Check if profile exists in stored data
            auto it = stored_bed_mesh_profiles_.find(profile_name);
            if (it != stored_bed_mesh_profiles_.end()) {
                // Load stored mesh data
                active_bed_mesh_ = it->second;
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: loaded profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: profile '{}' not found",
                             profile_name);
            }
        } else if (gcode.find("SAVE=") != std::string::npos) {
            auto save_pos = gcode.find("SAVE=");
            size_t start = save_pos + 5; // Length of "SAVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Add new profile to list if not already present
            if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
                bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.push_back(profile_name);
            }
            // Store current mesh data under new name
            active_bed_mesh_.name = profile_name;
            stored_bed_mesh_profiles_[profile_name] = active_bed_mesh_;
            spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE SAVE: saved profile '{}'",
                         profile_name);
            dispatch_bed_mesh_update();
        } else if (gcode.find("REMOVE=") != std::string::npos) {
            auto remove_pos = gcode.find("REMOVE=");
            size_t start = remove_pos + 7; // Length of "REMOVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Remove profile from list and stored data
            auto it = std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name);
            if (it != bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.erase(it);
                stored_bed_mesh_profiles_.erase(profile_name);
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: removed profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn(
                    "[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: profile '{}' not found",
                    profile_name);
            }
        }
    } else if (gcode.find("BED_MESH_CLEAR") != std::string::npos) {
        // Clear the active bed mesh
        active_bed_mesh_.name = "";
        active_bed_mesh_.probed_matrix.clear();
        active_bed_mesh_.x_count = 0;
        active_bed_mesh_.y_count = 0;
        spdlog::info("[MoonrakerClientMock] BED_MESH_CLEAR: cleared active mesh");
        dispatch_bed_mesh_update();
    }

    // Z offset - SET_GCODE_OFFSET Z=0.2 or SET_GCODE_OFFSET Z_ADJUST=-0.05
    if (gcode.find("SET_GCODE_OFFSET") != std::string::npos) {
        // Parse Z parameter (absolute offset)
        auto z_pos = gcode.find(" Z=");
        if (z_pos != std::string::npos) {
            try {
                double z_offset = std::stod(gcode.substr(z_pos + 3));
                gcode_offset_z_.store(z_offset);
                spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z={:.3f}", z_offset);
                dispatch_gcode_move_update();
            } catch (...) {
            }
        }

        // Parse Z_ADJUST parameter (relative adjustment)
        auto z_adj_pos = gcode.find("Z_ADJUST=");
        if (z_adj_pos != std::string::npos) {
            try {
                double adjustment = std::stod(gcode.substr(z_adj_pos + 9));
                double new_offset = gcode_offset_z_.load() + adjustment;
                gcode_offset_z_.store(new_offset);
                spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z_ADJUST={:.3f} -> Z={:.3f}",
                             adjustment, new_offset);
                dispatch_gcode_move_update();
            } catch (...) {
            }
        }
    }

    // Per-tool z-offset - SET_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset VALUE=-0.05
    // Only gcode_z_offset is modelled; the real command takes any tool
    // parameter, but nothing else is read back anywhere in the app.
    if (gcode.find("SET_TOOL_PARAMETER") != std::string::npos &&
        gcode.find("PARAMETER=gcode_z_offset") != std::string::npos) {
        auto t_pos = gcode.find("T=");
        auto v_pos = gcode.find("VALUE=");
        if (t_pos != std::string::npos && v_pos != std::string::npos) {
            try {
                int tool = std::stoi(gcode.substr(t_pos + 2));
                double value = std::stod(gcode.substr(v_pos + 6));
                {
                    std::lock_guard<std::mutex> lock(tool_z_offsets_mutex_);
                    tool_z_offsets_[tool] = value;
                }
                spdlog::info("[MoonrakerClientMock] SET_TOOL_PARAMETER T={} gcode_z_offset={:.3f}",
                             tool, value);
                dispatch_tool_update(tool);
            } catch (...) {
            }
        }
    }

    // Per-tool z-offset, durable half - SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_z_offset
    //
    // klipper-toolchanger's Tool.save_parameter() is configfile.set(self.name,
    // name, self.params[name]): it persists whatever the tool ALREADY holds and
    // takes no VALUE=. So this stages the current runtime value and changes
    // nothing live - the change only lands when SAVE_CONFIG writes it out.
    if (gcode.find("SAVE_TOOL_PARAMETER") != std::string::npos &&
        gcode.find("PARAMETER=gcode_z_offset") != std::string::npos) {
        auto t_pos = gcode.find("T=");
        if (t_pos != std::string::npos) {
            try {
                int tool = std::stoi(gcode.substr(t_pos + 2));
                char value[32];
                std::snprintf(value, sizeof(value), "%.6g", tool_z_offset(tool));
                // Section is Klipper's config section verbatim, which for
                // [tool T1] is "tool T1" - the same key the status object uses.
                stage_config_change("tool T" + std::to_string(tool), "gcode_z_offset", value);
                spdlog::info("[MoonrakerClientMock] SAVE_TOOL_PARAMETER T={} gcode_z_offset={} "
                             "- staged, awaiting SAVE_CONFIG",
                             tool, value);
            } catch (...) {
            }
        }
    }

    // Input shaper calibration - SHAPER_CALIBRATE AXIS=X or AXIS=Y
    if (gcode.find("SHAPER_CALIBRATE") != std::string::npos) {
        char axis = 'X'; // Default to X
        if (gcode.find("AXIS=Y") != std::string::npos ||
            gcode.find("AXIS=y") != std::string::npos) {
            axis = 'Y';
        } else if (gcode.find("AXIS=X") != std::string::npos ||
                   gcode.find("AXIS=x") != std::string::npos) {
            axis = 'X';
        }
        spdlog::info("[MoonrakerClientMock] SHAPER_CALIBRATE AXIS={}", axis);
        dispatch_shaper_calibrate_response(axis);
    }

    // SET_INPUT_SHAPER - Apply shaper settings (command handled via execute_gcode success callback)
    if (gcode.find("SET_INPUT_SHAPER") != std::string::npos) {
        spdlog::info("[MoonrakerClientMock] SET_INPUT_SHAPER: {}", gcode);
        // No additional action needed - execute_gcode path already invokes success callback
    }

    // MEASURE_AXES_NOISE - Check accelerometer noise level
    if (gcode.find("MEASURE_AXES_NOISE") != std::string::npos) {
        spdlog::info("[MoonrakerClientMock] MEASURE_AXES_NOISE");
        dispatch_measure_axes_noise_response();
    }

    // Pressure advance (NOT IMPLEMENTED)
    if (gcode.find("SET_PRESSURE_ADVANCE") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: SET_PRESSURE_ADVANCE NOT IMPLEMENTED");
    }

    // LED control - SET_LED LED=<name> RED=<0-1> GREEN=<0-1> BLUE=<0-1> [WHITE=<0-1>]
    if (gcode.find("SET_LED") != std::string::npos) {
        // Parse LED name
        std::string led_name;
        auto led_pos = gcode.find("LED=");
        if (led_pos != std::string::npos) {
            size_t start = led_pos + 4;
            // Handle quoted LED names: LED="name" or LED=name
            if (start < gcode.size() && gcode[start] == '"') {
                start++;
                size_t end = gcode.find('"', start);
                if (end != std::string::npos) {
                    led_name = gcode.substr(start, end - start);
                }
            } else {
                size_t end = gcode.find_first_of(" \t\n", start);
                led_name = gcode.substr(start, end == std::string::npos ? end : end - start);
            }
        }

        // Parse color values (default to 0)
        auto parse_color = [&gcode](const std::string& param) -> double {
            auto pos = gcode.find(param + "=");
            if (pos != std::string::npos) {
                size_t start = pos + param.length() + 1;
                try {
                    return std::clamp(std::stod(gcode.substr(start)), 0.0, 1.0);
                } catch (...) {
                    return 0.0;
                }
            }
            return 0.0;
        };

        double red = parse_color("RED");
        double green = parse_color("GREEN");
        double blue = parse_color("BLUE");
        double white = parse_color("WHITE");

        // Find matching LED in our list (need to match by suffix since command uses short name)
        std::string full_led_name;
        for (const auto& led : discovery_.leds()) {
            // Match if LED name ends with the command's led_name
            // e.g., "neopixel chamber_light" matches "chamber_light"
            if (led.length() >= led_name.length()) {
                size_t suffix_start = led.length() - led_name.length();
                if (led.substr(suffix_start) == led_name) {
                    full_led_name = led;
                    break;
                }
            }
        }

        if (!full_led_name.empty()) {
            // Update LED state
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                led_states_[full_led_name] = LedColor{red, green, blue, white};
            }

            spdlog::info("[MoonrakerClientMock] SET_LED: {} R={:.2f} G={:.2f} B={:.2f} W={:.2f}",
                         full_led_name, red, green, blue, white);

            // Dispatch LED state update notification (like real Moonraker would)
            json led_status;
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                for (const auto& [name, color] : led_states_) {
                    led_status[name] = {
                        {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
                }
            }
            dispatch_status_update(led_status);
        } else {
            spdlog::warn("[MoonrakerClientMock] SET_LED: unknown LED '{}'", led_name);
        }
    }

    // Firmware/Klipper restart - simulates klippy_state transition
    // FIRMWARE_RESTART: Full firmware reset (~3s delay)
    // RESTART: Klipper service restart (~2s delay)
    if (gcode.find("FIRMWARE_RESTART") != std::string::npos) {
        trigger_restart(/*is_firmware=*/true);
    } else if (gcode.find("RESTART") != std::string::npos &&
               gcode.find("FIRMWARE") == std::string::npos) {
        trigger_restart(/*is_firmware=*/false);
    }

    // ========================================================================
    // Z-OFFSET CALIBRATION COMMANDS (manual probe mode)
    // ========================================================================

    // PROBE_CALIBRATE or Z_ENDSTOP_CALIBRATE - Start Z-offset calibration
    // - PROBE_CALIBRATE: For printers with probe (BLTouch, inductive, etc.)
    // - Z_ENDSTOP_CALIBRATE: For printers with only mechanical Z endstop
    // Both enter manual probe mode, home if needed, and work identically
    bool is_probe_calibrate = gcode.find("PROBE_CALIBRATE") != std::string::npos;
    bool is_endstop_calibrate = gcode.find("Z_ENDSTOP_CALIBRATE") != std::string::npos;

    if (is_probe_calibrate || is_endstop_calibrate) {
        const char* cmd_name = is_probe_calibrate ? "PROBE_CALIBRATE" : "Z_ENDSTOP_CALIBRATE";

        if (!manual_probe_active_.load()) {
            // Ensure we're homed first
            {
                std::lock_guard<std::mutex> lock(homed_axes_mutex_);
                if (homed_axes_.find("xyz") == std::string::npos) {
                    // Auto-home like real Klipper would. Group the axis zeroing
                    // so a concurrent snapshot read never sees a torn position.
                    std::lock_guard<std::mutex> pos_lock(pos_mutex_);
                    homed_axes_ = "xyz";
                    pos_x_.store(0.0);
                    pos_y_.store(0.0);
                    pos_z_.store(0.0);
                    spdlog::info("[MoonrakerClientMock] {}: Auto-homed all axes", cmd_name);
                }
            }

            // Enter manual probe mode at a starting Z height
            manual_probe_active_.store(true);
            manual_probe_z_.store(5.0); // Start 5mm above bed
            pos_z_.store(5.0);          // Sync toolhead Z

            spdlog::info("[MoonrakerClientMock] {}: Entered manual probe mode, Z={:.3f}", cmd_name,
                         manual_probe_z_.load());

            // Dispatch manual probe state change
            dispatch_manual_probe_update();
        } else {
            spdlog::warn("[MoonrakerClientMock] {}: Already in manual probe mode", cmd_name);
        }
    }

    // TESTZ Z=<value> - Adjust Z position during manual probe calibration
    // Z can be absolute (Z=0.1) or relative (Z=+0.1 or Z=-0.05)
    if (gcode.find("TESTZ") != std::string::npos) {
        if (!manual_probe_active_.load()) {
            spdlog::warn("[MoonrakerClientMock] TESTZ: Not in manual probe mode (ignored)");
            return 0;
        }
        size_t z_pos = gcode.find("Z=");
        if (z_pos != std::string::npos) {
            std::string z_str = gcode.substr(z_pos + 2);
            try {
                // Check for relative move (+/- prefix)
                bool is_relative = (z_str[0] == '+' || z_str[0] == '-');
                double z_value = std::stod(z_str);

                double new_z;
                if (is_relative) {
                    new_z = manual_probe_z_.load() + z_value;
                } else {
                    new_z = z_value;
                }

                // Clamp to reasonable range (0 to 10mm above bed)
                new_z = std::clamp(new_z, -0.5, 10.0);

                manual_probe_z_.store(new_z);
                pos_z_.store(new_z); // Sync toolhead Z

                spdlog::info("[MoonrakerClientMock] TESTZ: Z={:.3f} ({}) -> new Z={:.3f}", z_value,
                             is_relative ? "relative" : "absolute", new_z);

                // Dispatch Z position update
                dispatch_manual_probe_update();
            } catch (const std::exception& e) {
                spdlog::warn("[MoonrakerClientMock] TESTZ: Failed to parse Z value: {}", e.what());
            }
        }
    }

    // ACCEPT - Accept current Z position as the calibrated offset
    if (gcode == "ACCEPT" || gcode.find("ACCEPT ") == 0) {
        if (manual_probe_active_.load()) {
            double final_z = manual_probe_z_.load();
            manual_probe_active_.store(false);

            spdlog::info(
                "[MoonrakerClientMock] ACCEPT: Z-offset calibration complete, offset={:.3f}mm",
                final_z);

            // In real Klipper, this would update probe z_offset in config
            // User typically follows with SAVE_CONFIG to persist

            // Dispatch manual probe state change (is_active=false)
            dispatch_manual_probe_update();
        } else {
            spdlog::warn("[MoonrakerClientMock] ACCEPT: Not in manual probe mode");
        }
    }

    // ABORT - Cancel manual probe calibration
    if (gcode == "ABORT" || gcode.find("ABORT ") == 0) {
        if (manual_probe_active_.load()) {
            manual_probe_active_.store(false);
            spdlog::info("[MoonrakerClientMock] ABORT: Manual probe cancelled");

            // Dispatch manual probe state change (is_active=false)
            dispatch_manual_probe_update();
        }
    }

    // EXCLUDE_OBJECT - Track excluded objects during print
    // EXCLUDE_OBJECT NAME=Part_1
    // EXCLUDE_OBJECT NAME="Part With Spaces"
    if (gcode.find("EXCLUDE_OBJECT") != std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_DEFINE") == std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_START") == std::string::npos &&
        gcode.find("EXCLUDE_OBJECT_END") == std::string::npos) {
        // Parse NAME parameter
        size_t name_pos = gcode.find("NAME=");
        if (name_pos != std::string::npos) {
            size_t start = name_pos + 5;
            std::string object_name;

            // Handle quoted names (NAME="Part With Spaces")
            if (start < gcode.length() && gcode[start] == '"') {
                size_t end_quote = gcode.find('"', start + 1);
                if (end_quote != std::string::npos) {
                    object_name = gcode.substr(start + 1, end_quote - start - 1);
                }
            } else {
                // Unquoted name (ends at space or end of string)
                size_t end = gcode.find_first_of(" \t\n", start);
                object_name = (end != std::string::npos) ? gcode.substr(start, end - start)
                                                         : gcode.substr(start);
            }

            if (!object_name.empty()) {
                // Update shared state if available
                if (mock_state_) {
                    mock_state_->add_excluded_object(object_name);
                }
                // Also update local state for backward compatibility
                {
                    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                    excluded_objects_.insert(object_name);
                }
                spdlog::info("[MoonrakerClientMock] EXCLUDE_OBJECT: '{}' added to exclusion list",
                             object_name);

                // Dispatch status update (like real Klipper would via WebSocket)
                // Use local excluded_objects_ (always up-to-date) rather than mock_state_
                // which is only available in test fixtures
                {
                    json excluded_array = json::array();
                    {
                        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                        for (const auto& obj : excluded_objects_) {
                            excluded_array.push_back(obj);
                        }
                    }
                    json eo_status = {
                        {"exclude_object",
                         {{"excluded_objects", excluded_array}, {"current_object", nullptr}}}};
                    dispatch_status_update(eo_status);
                }
            }
        } else {
            spdlog::warn("[MoonrakerClientMock] EXCLUDE_OBJECT without NAME parameter ignored");
        }
    }

    // EXCLUDE_OBJECT_DEFINE - Register objects for the print
    if (gcode.find("EXCLUDE_OBJECT_DEFINE") != std::string::npos) {
        size_t name_pos = gcode.find("NAME=");
        if (name_pos != std::string::npos) {
            size_t start = name_pos + 5;
            std::string object_name;
            if (start < gcode.size() && gcode[start] == '"') {
                size_t end = gcode.find('"', start + 1);
                if (end != std::string::npos) {
                    object_name = gcode.substr(start + 1, end - start - 1);
                }
            } else {
                size_t end = gcode.find_first_of(" \t\r\n", start);
                object_name = gcode.substr(start, end - start);
            }
            if (!object_name.empty() && mock_state_) {
                mock_state_->add_object_name(object_name);
                spdlog::debug("[MoonrakerClientMock] EXCLUDE_OBJECT_DEFINE: registered '{}'",
                              object_name);
            }
        }
    }

    // SET_LED_EFFECT EFFECT=<name> - Enable an LED effect
    if (gcode.find("SET_LED_EFFECT") != std::string::npos) {
        size_t effect_pos = gcode.find("EFFECT=");
        if (effect_pos != std::string::npos) {
            size_t start = effect_pos + 7;
            size_t end = gcode.find_first_of(" \t\r\n", start);
            std::string effect_name = gcode.substr(start, end - start);

            spdlog::info("[MoonrakerClientMock] SET_LED_EFFECT: enabling '{}'", effect_name);

            // Build status update: enable the target effect, disable all others
            json effect_status = json::object();
            std::string full_name = "led_effect " + effect_name;

            // Known mock effects
            const std::vector<std::string> known_effects = {
                "led_effect breathing", "led_effect fire_comet", "led_effect rainbow",
                "led_effect static_white"};

            for (const auto& name : known_effects) {
                bool should_enable = (name == full_name);
                effect_status[name] = {{"enabled", should_enable}};
            }

            // Simulate LED color output: each effect has a characteristic color
            // In real Klipper, led_effect continuously updates the neopixel color_data
            struct EffectColor {
                double r, g, b, w;
            };
            static const std::unordered_map<std::string, EffectColor> effect_colors = {
                {"breathing", {0.6, 0.6, 1.0, 0.0}},    // Soft blue-white pulse
                {"fire_comet", {1.0, 0.3, 0.0, 0.0}},   // Orange/fire
                {"rainbow", {0.5, 0.0, 1.0, 0.0}},      // Purple (mid-rainbow)
                {"static_white", {1.0, 1.0, 1.0, 0.0}}, // Pure white
            };

            auto color_it = effect_colors.find(effect_name);
            if (color_it != effect_colors.end()) {
                const auto& c = color_it->second;
                // Update internal LED state and dispatch color_data for all LED strips
                {
                    std::lock_guard<std::mutex> lock(led_mutex_);
                    for (auto& [name, color] : led_states_) {
                        color = LedColor{c.r, c.g, c.b, c.w};
                    }
                }
                json led_status;
                {
                    std::lock_guard<std::mutex> lock(led_mutex_);
                    for (const auto& [name, color] : led_states_) {
                        led_status[name] = {
                            {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
                    }
                }
                // Merge LED color updates into the effect status dispatch
                effect_status.update(led_status);
            }

            dispatch_status_update(effect_status);
        }
    }

    // STOP_LED_EFFECTS - Disable all LED effects
    if (gcode.find("STOP_LED_EFFECTS") != std::string::npos) {
        spdlog::info("[MoonrakerClientMock] STOP_LED_EFFECTS: disabling all effects");

        json effect_status = json::object();
        const std::vector<std::string> known_effects = {
            "led_effect breathing", "led_effect fire_comet", "led_effect rainbow",
            "led_effect static_white"};

        for (const auto& name : known_effects) {
            effect_status[name] = {{"enabled", false}};
        }

        // Turn LEDs off when effects stop
        {
            std::lock_guard<std::mutex> lock(led_mutex_);
            for (auto& [name, color] : led_states_) {
                color = LedColor{0.0, 0.0, 0.0, 0.0};
            }
        }
        json led_status;
        {
            std::lock_guard<std::mutex> lock(led_mutex_);
            for (const auto& [name, color] : led_states_) {
                led_status[name] = {
                    {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
            }
        }
        effect_status.update(led_status);

        dispatch_status_update(effect_status);
    }

    // QGL / Z-tilt (NOT IMPLEMENTED)
    if (gcode.find("QUAD_GANTRY_LEVEL") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: QUAD_GANTRY_LEVEL NOT IMPLEMENTED");
    } else if (gcode.find("Z_TILT_ADJUST") != std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: Z_TILT_ADJUST NOT IMPLEMENTED");
    }

    // Probe (NOT IMPLEMENTED) - excludes PROBE_CALIBRATE which is handled above
    if (gcode.find("PROBE") != std::string::npos && gcode.find("BED_MESH") == std::string::npos &&
        gcode.find("PROBE_CALIBRATE") == std::string::npos) {
        spdlog::warn("[MoonrakerClientMock] STUB: PROBE command not fully implemented");
    }

    // Return error code if any error occurred (like real Moonraker)
    {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        if (!last_gcode_error_.empty()) {
            return 1; // Error - call get_last_gcode_error() for message
        }
    }
    return 0; // Success
}

std::string MoonrakerClientMock::get_print_state_string() const {
    switch (print_state_.load()) {
    case 0:
        return "standby";
    case 1:
        return "printing";
    case 2:
        return "paused";
    case 3:
        return "complete";
    case 4:
        return "cancelled";
    case 5:
        return "error";
    default:
        return "standby";
    }
}

// ============================================================================
// Unified Print Control (internal implementation)
// ============================================================================

bool MoonrakerClientMock::start_print_internal(const std::string& filename) {
    // Build path to test G-code file
    // Handle both bare filenames (e.g., "3DBenchy.gcode") and full paths
    std::string full_path;

    // For modified temp files (.helix_temp/modified_xxx_OriginalName.gcode),
    // extract the original filename to find the real test file for metadata
    std::string lookup_filename = filename;
    if (filename.find(".helix_temp/modified_") != std::string::npos) {
        // Extract original filename: .helix_temp/modified_123456789_OriginalName.gcode
        // -> OriginalName.gcode
        size_t underscore_pos = filename.find('_', filename.find("modified_") + 9);
        if (underscore_pos != std::string::npos) {
            lookup_filename = filename.substr(underscore_pos + 1);
            spdlog::debug("[MoonrakerClientMock] Modified temp file '{}' -> original '{}'",
                          filename, lookup_filename);
        }
    }

    if (lookup_filename.find(RuntimeConfig::TEST_GCODE_DIR) == 0) {
        // Already a full path, use as-is
        full_path = lookup_filename;
    } else {
        // Bare filename, prepend test directory
        full_path = std::string(RuntimeConfig::TEST_GCODE_DIR) + "/" + lookup_filename;
    }

    // Extract metadata from G-code file
    auto meta = helix::gcode::extract_header_metadata(full_path);

    // Populate simulation metadata
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        print_metadata_.estimated_time_seconds =
            (meta.estimated_time_seconds > 0) ? meta.estimated_time_seconds : 300.0;
        print_metadata_.layer_count = (meta.layer_count > 0) ? meta.layer_count : 100;
        print_metadata_.target_bed_temp =
            (meta.first_layer_bed_temp > 0) ? meta.first_layer_bed_temp : 60.0;
        print_metadata_.target_nozzle_temp =
            (meta.first_layer_nozzle_temp > 0) ? meta.first_layer_nozzle_temp : 210.0;
        print_metadata_.filament_mm =
            (meta.filament_used_mm > 0) ? meta.filament_used_mm : 5400.0; // Default: ~5.4m
        print_metadata_.filament_weights_g = meta.filament_used_per_tool_g;
    }

    // Compute dominant tool (highest per-tool weight) for the mock to expose
    // as the active gcode tool during simulated print. This is the moonraker
    // mock's equivalent of "Klipper saw a T-command" — production AMS backends
    // would read printer.mmu.tool / toolchanger.tool_number on real hardware.
    {
        int dominant = -1;
        double max_w = 0.0;
        const auto& weights = print_metadata_.filament_weights_g;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (weights[i] > max_w) {
                max_w = weights[i];
                dominant = static_cast<int>(i);
            }
        }

        // Cache per-tool slicer colors so observers can show the slicer's
        // intended color when the active tool isn't mapped to a real slot.
        {
            std::lock_guard<std::mutex> lock(active_gcode_tool_mutex_);
            active_gcode_tool_colors_.clear();
            for (const auto& hex : meta.tool_colors) {
                // tool_colors come as "#RRGGBB" or "RRGGBB" — parse defensively.
                const char* p = hex.c_str();
                if (*p == '#') {
                    ++p;
                }
                uint32_t rgb = 0;
                try {
                    rgb = static_cast<uint32_t>(std::stoul(p, nullptr, 16)) & 0xFFFFFF;
                } catch (...) {
                    rgb = 0;
                }
                active_gcode_tool_colors_.push_back(rgb);
            }
        }

        active_gcode_tool_.store(dominant);
        spdlog::debug(
            "[MoonrakerClientMock] Active gcode tool: T{} (from per-tool weights, max {:.2f}g)",
            dominant, max_w);
        notify_active_gcode_tool_observers(dominant);
    }

    // Set temperature targets for preheat
    double nozzle_target, bed_target;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        nozzle_target = print_metadata_.target_nozzle_temp;
        bed_target = print_metadata_.target_bed_temp;
    }
    extruder_target_.store(nozzle_target);
    bed_target_.store(bed_target);

    // Reset idle timeout when starting a print
    reset_idle_timeout();

    // Set print filename
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        print_filename_ = filename;
    }

    // Reset progress and timing
    print_progress_.store(0.0);
    total_pause_duration_sim_ = 0.0;
    preheat_start_time_ = std::chrono::steady_clock::now();
    printing_start_time_.reset();

    // Clear excluded objects from any previous print
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
        object_names_.clear();
    }

    // Scan gcode file for EXCLUDE_OBJECT_DEFINE to populate object names
    {
        std::ifstream gcode_file(full_path);
        if (gcode_file.is_open()) {
            std::string line;
            while (std::getline(gcode_file, line)) {
                if (line.find("EXCLUDE_OBJECT_DEFINE") != std::string::npos) {
                    size_t name_pos = line.find("NAME=");
                    if (name_pos != std::string::npos) {
                        size_t start = name_pos + 5;
                        std::string object_name;
                        if (start < line.size() && line[start] == '"') {
                            size_t end = line.find('"', start + 1);
                            if (end != std::string::npos) {
                                object_name = line.substr(start + 1, end - start - 1);
                            }
                        } else {
                            size_t end = line.find_first_of(" \t\r\n", start);
                            object_name = (end != std::string::npos)
                                              ? line.substr(start, end - start)
                                              : line.substr(start);
                        }
                        if (!object_name.empty()) {
                            if (mock_state_) {
                                mock_state_->add_object_name(object_name);
                            }
                            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                            if (std::find(object_names_.begin(), object_names_.end(),
                                          object_name) == object_names_.end()) {
                                object_names_.push_back(object_name);
                            }
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            if (!object_names_.empty()) {
                spdlog::info("[MoonrakerClientMock] Found {} EXCLUDE_OBJECT_DEFINE objects in '{}'",
                             object_names_.size(), full_path);
            }
        }
    }

    // Parse EXCLUDE_OBJECT_DEFINE lines from gcode to populate defined objects.
    // Extracts NAME, CENTER, and POLYGON to match real Moonraker behavior.
    {
        json objects_array = json::array();
        std::vector<std::string> defined_objects;
        std::ifstream gcode_file(full_path);
        if (gcode_file.is_open()) {
            std::string line;
            while (std::getline(gcode_file, line)) {
                if (line.find("EXCLUDE_OBJECT_DEFINE") != std::string::npos) {
                    // Extract NAME= parameter
                    auto name_pos = line.find("NAME=");
                    if (name_pos == std::string::npos)
                        continue;
                    std::string name;
                    size_t start = name_pos + 5;
                    if (start < line.size() && line[start] == '"') {
                        size_t end = line.find('"', start + 1);
                        if (end != std::string::npos)
                            name = line.substr(start + 1, end - start - 1);
                    } else if (start < line.size() && line[start] == '\'') {
                        size_t end = line.find('\'', start + 1);
                        if (end != std::string::npos)
                            name = line.substr(start + 1, end - start - 1);
                    } else {
                        size_t end = line.find(' ', start);
                        name = line.substr(start, end - start);
                    }
                    if (name.empty())
                        continue;

                    defined_objects.push_back(name);
                    if (mock_state_)
                        mock_state_->add_object_name(name);

                    // Build JSON object entry with CENTER and POLYGON if present
                    json obj_entry = {{"name", name}};

                    // Parse CENTER=x,y
                    auto center_pos = line.find("CENTER=");
                    if (center_pos != std::string::npos) {
                        size_t cs = center_pos + 7;
                        size_t ce = line.find(' ', cs);
                        std::string center_str = line.substr(cs, ce - cs);
                        auto comma = center_str.find(',');
                        if (comma != std::string::npos) {
                            try {
                                float cx = std::stof(center_str.substr(0, comma));
                                float cy = std::stof(center_str.substr(comma + 1));
                                obj_entry["center"] = {cx, cy};
                            } catch (...) {
                            }
                        }
                    }

                    // Parse POLYGON=[[x,y],[x,y],...]
                    auto poly_pos = line.find("POLYGON=");
                    if (poly_pos != std::string::npos) {
                        size_t ps = poly_pos + 8;
                        // Find matching closing bracket
                        int depth = 0;
                        size_t pe = ps;
                        for (; pe < line.size(); ++pe) {
                            if (line[pe] == '[')
                                depth++;
                            else if (line[pe] == ']') {
                                depth--;
                                if (depth == 0) {
                                    pe++;
                                    break;
                                }
                            }
                        }
                        std::string poly_str = line.substr(ps, pe - ps);

                        // Parse the polygon array: [[x,y],[x,y],...]
                        json polygon = json::array();
                        size_t pos = 0;
                        while ((pos = poly_str.find('[', pos)) != std::string::npos) {
                            size_t end = poly_str.find(']', pos);
                            if (end == std::string::npos)
                                break;
                            std::string pair = poly_str.substr(pos + 1, end - pos - 1);
                            auto c = pair.find(',');
                            if (c != std::string::npos) {
                                try {
                                    float px = std::stof(pair.substr(0, c));
                                    float py = std::stof(pair.substr(c + 1));
                                    polygon.push_back({px, py});
                                } catch (...) {
                                }
                            }
                            pos = end + 1;
                        }
                        if (!polygon.empty()) {
                            obj_entry["polygon"] = polygon;
                        }
                    }

                    objects_array.push_back(std::move(obj_entry));
                }
                // Stop scanning after first layer to avoid reading the entire file
                if (line.find(";LAYER_CHANGE") != std::string::npos ||
                    line.find("; LAYER_CHANGE") != std::string::npos) {
                    break;
                }
            }
        }

        if (!defined_objects.empty()) {
            spdlog::info("[MoonrakerClientMock] Found {} defined objects in '{}'",
                         defined_objects.size(), lookup_filename);
            if (mock_state_) {
                mock_state_->set_available_objects(defined_objects);
            }
            json eo_status = {{"exclude_object",
                               {{"objects", objects_array},
                                {"excluded_objects", json::array()},
                                {"current_object", nullptr}}}};
            dispatch_status_update(eo_status);
        }
    }

    // HELIX_MOCK_EXCLUDE_OBJECTS — replace whatever the G-code declared with a
    // synthetic multi-object plate. Published through the same
    // `exclude_object.objects` status update Klipper uses, so PrinterState,
    // the map view and the side list see nothing special about it.
    if (mock_exclude_object_count_ > 0) {
        const int total = mock_exclude_object_count_;
        std::vector<std::string> names;
        names.reserve(static_cast<size_t>(total));
        json objects_array = json::array();
        for (int i = 0; i < total; ++i) {
            names.emplace_back(MOCK_EXCLUDE_OBJECT_NAMES[i]);
            objects_array.push_back(mock_object_entry(names.back(), i, total));
        }

        {
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            object_names_ = names;
        }
        if (mock_state_) {
            mock_state_->set_available_objects(names);
        }

        json eo_status = {{"exclude_object",
                           {{"objects", objects_array},
                            {"excluded_objects", json::array()},
                            {"current_object", nullptr}}}};
        dispatch_status_update(eo_status);

        spdlog::info("[MoonrakerClientMock] Published {} synthetic exclude_object entries "
                     "(HELIX_MOCK_EXCLUDE_OBJECTS)",
                     total);
    }

    // Reset PRINT_START simulation phase tracking for new print
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Transition to PREHEAT phase
    print_phase_.store(MockPrintPhase::PREHEAT);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::debug("[MoonrakerClientMock] Starting print '{}': est_time={:.0f}s, layers={}, "
                  "nozzle={:.0f}°C, bed={:.0f}°C",
                  filename, meta.estimated_time_seconds, meta.layer_count, nozzle_target,
                  bed_target);

    dispatch_print_state_notification("printing");
    return true;
}

bool MoonrakerClientMock::pause_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can only pause from PRINTING or PREHEAT
    if (current_phase != MockPrintPhase::PRINTING && current_phase != MockPrintPhase::PREHEAT) {
        spdlog::warn("[MoonrakerClientMock] Cannot pause - not currently printing (phase={})",
                     static_cast<int>(current_phase));
        return false;
    }

    // Record pause start time
    pause_start_time_ = std::chrono::steady_clock::now();

    // Transition to PAUSED
    print_phase_.store(MockPrintPhase::PAUSED);
    print_state_.store(2); // "paused" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print paused at {:.1f}% progress",
                 print_progress_.load() * 100.0);

    dispatch_print_state_notification("paused");
    return true;
}

bool MoonrakerClientMock::resume_print_internal() {
    if (print_phase_.load() != MockPrintPhase::PAUSED) {
        spdlog::warn("[MoonrakerClientMock] Cannot resume - not currently paused");
        return false;
    }

    // Calculate pause duration and add to total
    auto pause_real = std::chrono::steady_clock::now() - pause_start_time_;
    double pause_sim = std::chrono::duration<double>(pause_real).count() * speedup_factor_.load();
    total_pause_duration_sim_ += pause_sim;

    // Resume to PRINTING phase (skip PREHEAT since temps should still be maintained)
    print_phase_.store(MockPrintPhase::PRINTING);
    print_state_.store(1); // "printing" for backward compatibility

    spdlog::info("[MoonrakerClientMock] Print resumed (pause duration: {:.1f}s simulated)",
                 pause_sim);

    dispatch_print_state_notification("printing");
    // Clear any pause-reason message Klipper had set (mirrors real firmware behavior).
    {
        json clear_msg;
        clear_msg["print_stats"]["message"] = "";
        dispatch_status_update(clear_msg);
    }
    return true;
}

bool MoonrakerClientMock::cancel_print_internal() {
    MockPrintPhase current_phase = print_phase_.load();

    // Can cancel from any non-idle phase
    if (current_phase == MockPrintPhase::IDLE) {
        spdlog::warn("[MoonrakerClientMock] Cannot cancel - no active print");
        return false;
    }

    // Set targets to 0 (begin cooldown)
    extruder_target_.store(0.0);
    bed_target_.store(0.0);
    chamber_target_.store(0.0);

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Transition to CANCELLED
    print_phase_.store(MockPrintPhase::CANCELLED);
    print_state_.store(4); // "cancelled" for backward compatibility

    spdlog::debug("[MoonrakerClientMock] Print cancelled at {:.1f}% progress",
                  print_progress_.load() * 100.0);

    dispatch_print_state_notification("cancelled");
    return true;
}

void MoonrakerClientMock::emergency_stop_internal() {
    spdlog::warn("[MoonrakerClientMock] Emergency stop executed!");

    // Zero all heater targets
    extruder_target_.store(0.0);
    bed_target_.store(0.0);
    chamber_target_.store(0.0);

    // Turn off all fans
    fan_speed_.store(0);
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        for (auto& [name, speed] : fan_speeds_) {
            speed = 0.0;
        }
    }

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Set print state to error (matches real Klipper M112 behavior)
    print_phase_.store(MockPrintPhase::ERROR);
    print_state_.store(5); // error
    dispatch_print_state_notification("error");

    // Set klippy state to SHUTDOWN (must defer to main thread)
    helix::ui::async_call(
        [](void*) { get_printer_state().set_klippy_state_sync(helix::KlippyState::SHUTDOWN); },
        nullptr);
}

bool MoonrakerClientMock::toggle_filament_runout() {
    // Find primary runout sensor from filament_sensors list
    std::string runout_sensor;
    for (const auto& sensor : discovery_.filament_sensors()) {
        if (sensor.find("runout") != std::string::npos) {
            runout_sensor = sensor;
            break;
        }
    }

    // Fallback to first sensor if no "runout" sensor found
    if (runout_sensor.empty() && !discovery_.filament_sensors().empty()) {
        runout_sensor = discovery_.filament_sensors()[0];
    }

    if (runout_sensor.empty()) {
        spdlog::warn("[MoonrakerClientMock] No filament sensor to toggle");
        return false;
    }

    // Toggle state
    bool new_state = !filament_runout_state_.load();
    filament_runout_state_.store(new_state);

    spdlog::info("[MoonrakerClientMock] Filament toggle on '{}': {} -> {}", runout_sensor,
                 new_state ? "empty" : "detected", new_state ? "detected" : "empty");

    // Dispatch status update through normal flow
    json status;
    status[runout_sensor]["filament_detected"] = new_state;
    dispatch_status_update(status);

    // Auto-pause if: runout detected + actively printing + runout modal enabled
    // This simulates Klipper's pause_on_runout behavior
    if (!new_state) { // new_state=false means filament NOT detected (runout)
        MockPrintPhase phase = print_phase_.load();
        if (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT) {
            if (get_runtime_config()->should_show_runout_modal()) {
                spdlog::info("[MoonrakerClientMock] Filament runout during print - auto-pausing");
                pause_print_internal();
                // Mirror Klipper's runout_helper: set print_stats.message so the
                // UI can surface the reason under the "Print Paused" badge.
                json msg_status;
                msg_status["print_stats"]["message"] =
                    "Filament Sensor " + runout_sensor + ": Runout Detected";
                dispatch_status_update(msg_status);
            }
        }
    }

    return true;
}

// ============================================================================
// Simulation Helpers
// ============================================================================

bool MoonrakerClientMock::is_temp_stable(double current, double target, double tolerance) const {
    return std::abs(current - target) <= tolerance;
}

void MoonrakerClientMock::advance_print_progress(double dt_simulated) {
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_time = print_metadata_.estimated_time_seconds;
    }

    if (total_time <= 0) {
        return;
    }

    double rate = 1.0 / total_time; // Progress per simulated second
    double current = print_progress_.load();
    print_progress_.store(std::min(1.0, current + rate * dt_simulated));
}

void MoonrakerClientMock::dispatch_print_state_notification(const std::string& state) {
    // Include filename in state notifications so observers can update immediately
    // This is critical for PrintStatusPanel to load the thumbnail when print starts
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    spdlog::debug(
        "[MoonrakerClientMock] dispatch_print_state_notification: state='{}' filename='{}'", state,
        filename);
    json notification_status = {{"print_stats", {{"state", state}, {"filename", filename}}}};
    dispatch_status_update(notification_status);
}

void MoonrakerClientMock::dispatch_enhanced_print_status() {
    double progress = print_progress_.load();
    int current_layer = get_current_layer();
    int total_layers;
    double total_time;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_layers = static_cast<int>(print_metadata_.layer_count);
        total_time = print_metadata_.estimated_time_seconds;
    }

    double elapsed = progress * total_time;

    std::string filename;
    double filament_total_mm;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        filament_total_mm = print_metadata_.filament_mm;
    }

    // Simulate filament consumption proportional to progress
    double filament_used = (filament_total_mm > 0) ? progress * filament_total_mm : 0.0;

    MockPrintPhase phase = print_phase_.load();
    bool is_active = (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT);

    json status = {{"print_stats",
                    {{"state", get_print_state_string()},
                     {"filename", filename},
                     {"print_duration", elapsed},
                     {"total_duration", elapsed},    // Wall-clock elapsed (matches real Moonraker)
                     {"estimated_time", total_time}, // Slicer estimate (for completion modal)
                     {"filament_used", filament_used},
                     {"message", ""},
                     {"info", {{"current_layer", current_layer}, {"total_layer", total_layers}}}}},
                   {"virtual_sdcard",
                    {{"file_path", filename}, {"progress", progress}, {"is_active", is_active}}}};

    // Build exclude_object status — only send excluded_objects + current_object
    // (objects list with geometry was sent once at print start from GCode parsing)
    {
        json excluded_array = json::array();
        std::string current;

        if (mock_state_) {
            auto names = mock_state_->get_object_names();
            auto excl = mock_state_->get_excluded_objects();
            for (const auto& obj : excl) {
                excluded_array.push_back(obj);
            }
            // Pick first non-excluded object as current during active printing
            if (!names.empty() && is_active) {
                for (const auto& n : names) {
                    if (excl.count(n) == 0) {
                        current = n;
                        break;
                    }
                }
            }
        } else {
            // Fallback to local state for backward compatibility
            std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
            for (const auto& obj : excluded_objects_) {
                excluded_array.push_back(obj);
            }
            if (!object_names_.empty() && is_active) {
                for (const auto& n : object_names_) {
                    if (excluded_objects_.count(n) == 0) {
                        current = n;
                        break;
                    }
                }
            }
        }

        // Only send excluded_objects + current_object in periodic updates
        // (objects list with geometry was sent once at print start)
        status["exclude_object"] = {
            {"excluded_objects", excluded_array},
            {"current_object", current.empty() ? json(nullptr) : json(current)}};
    }

    dispatch_status_update(status);
}

// ============================================================================
// Temperature Simulation
// ============================================================================

void MoonrakerClientMock::read_position_snapshot(double& x, double& y, double& z) const {
    std::lock_guard<std::mutex> pos_lock(pos_mutex_);
    x = pos_x_.load();
    y = pos_y_.load();
    z = pos_z_.load();
}

void MoonrakerClientMock::dispatch_initial_state() {
    // Build initial state JSON matching real Moonraker subscription response format
    // Uses current simulated values (room temp by default, or preset values if set)
    double ext_temp = extruder_temp_.load();
    double ext_target = extruder_target_.load();
    double bed_temp_val = bed_temp_.load();
    double bed_target_val = bed_target_.load();
    double x, y, z;
    read_position_snapshot(x, y, z);
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();
    int fan = fan_speed_.load();

    // Get homed_axes with thread safety
    std::string homed;
    {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        homed = homed_axes_;
    }

    // Get print state with thread safety
    std::string print_state_str = get_print_state_string();
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    double progress = print_progress_.load();

    // Convert probed_matrix to JSON 2D array
    json probed_matrix_json = json::array();
    for (const auto& row : active_bed_mesh_.probed_matrix) {
        json row_json = json::array();
        for (float val : row) {
            row_json.push_back(val);
        }
        probed_matrix_json.push_back(row_json);
    }

    // Build profiles object with full mesh data (Moonraker format)
    json profiles_json = json::object();
    for (const auto& [name, profile] : stored_bed_mesh_profiles_) {
        json points_json = json::array();
        for (const auto& row : profile.probed_matrix) {
            json row_json = json::array();
            for (float val : row) {
                row_json.push_back(val);
            }
            points_json.push_back(row_json);
        }
        profiles_json[name] = {{"points", points_json},
                               {"mesh_params",
                                {{"min_x", profile.mesh_min[0]},
                                 {"min_y", profile.mesh_min[1]},
                                 {"max_x", profile.mesh_max[0]},
                                 {"max_y", profile.mesh_max[1]},
                                 {"x_count", profile.x_count},
                                 {"y_count", profile.y_count}}}};
    }

    // Build LED state JSON
    json led_json = json::object();
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        for (const auto& [name, color] : led_states_) {
            led_json[name] = {{"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
        }
    }
    // Add output_pin status (uses {value:} format, not color_data)
    for (const auto& led : discovery_.leds()) {
        if (led.rfind("output_pin ", 0) == 0) {
            led_json[led] = {{"value", 0.75}}; // Mock: enclosure at 75% brightness
        }
    }

    // Get Z offset and klippy state
    double z_offset = gcode_offset_z_.load();
    KlippyState klippy = klippy_state_.load();
    std::string klippy_str = "ready";
    switch (klippy) {
    case KlippyState::STARTUP:
        klippy_str = "startup";
        break;
    case KlippyState::SHUTDOWN:
        klippy_str = "shutdown";
        break;
    case KlippyState::ERROR:
        klippy_str = "error";
        break;
    default:
        break;
    }

    json initial_status = {
        {"extruder", {{"temperature", ext_temp}, {"target", ext_target}}},
        {"heater_bed", {{"temperature", bed_temp_val}, {"target", bed_target_val}}},
        {[this]() {
             auto key = chamber_heater_status_key();
             return key.empty() ? "heater_generic chamber" : key;
         }(),
         {{"temperature", 42.3}, {"target", chamber_target_.load()}}},
        {"temperature_sensor chamber", {{"temperature", 42.3}}},
        {"toolhead",
         {{"position", {x, y, z, 0.0}},
          {"homed_axes", homed},
          {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
          {"axis_maximum",
           {persona_axis_maximum(printer_type_)[0], persona_axis_maximum(printer_type_)[1],
            persona_axis_maximum(printer_type_)[2], 0.0}},
          {"kinematics", discovery_.hardware().kinematics()}}},
        {"gcode_move",
         {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
          {"speed_factor", speed / 100.0},
          {"extrude_factor", flow / 100.0},
          {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
        {"fan", {{"speed", fan / 255.0}}},
        {"webhooks", {{"state", klippy_str}, {"state_message", "Printer is ready"}}},
        {"print_stats", {{"state", print_state_str}, {"filename", filename}}},
        {"virtual_sdcard", {{"progress", progress}}},
        {"bed_mesh",
         {{"profile_name", active_bed_mesh_.name},
          {"probed_matrix", probed_matrix_json},
          {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
          {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
          {"profiles", profiles_json},
          {"mesh_params", {{"algo", active_bed_mesh_.algo}}}}}};

    // Include exclude_object initial state (empty - no objects defined until print starts)
    initial_status["exclude_object"] = {{"objects", json::array()},
                                        {"excluded_objects", json::array()},
                                        {"current_object", nullptr}};

    // Merge LED states into initial_status (each LED is a top-level key)
    for (auto& [key, value] : led_json.items()) {
        initial_status[key] = value;
    }

    // Auto-controlled heater_fans trip at 50°C (Klipper default). Mirror here
    // so the initial subscription response reports the correct hotend fan speed.
    {
        double hotend_fan_speed = (ext_temp > 50.0) ? 1.0 : 0.0;
        for (const auto& fan_name : discovery_.fans()) {
            if (fan_name.rfind("heater_fan ", 0) == 0) {
                initial_status[fan_name] = {{"speed", hotend_fan_speed}};
            }
        }
    }

    // Override fan speeds with explicitly-set values from fan_speeds_ map
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        for (const auto& [name, spd] : fan_speeds_) {
            if (name == "fan") {
                initial_status["fan"] = {{"speed", spd}};
            } else {
                initial_status[name] = {{"speed", spd}};
            }
        }
    }

    // Add temperature sensor data for all sensors in the discovery sensors list
    for (const auto& s : discovery_.sensors()) {
        if (s.rfind("temperature_sensor ", 0) == 0) {
            std::string sensor_name = s.substr(19);
            double temp = 25.0;
            if (sensor_name.find("chamber") != std::string::npos) {
                temp = chamber_temp_.load();
            } else if (sensor_name.find("mcu") != std::string::npos) {
                temp = mcu_temp_.load();
            } else if (sensor_name.find("raspberry") != std::string::npos ||
                       sensor_name.find("host") != std::string::npos || sensor_name == "rpi") {
                temp = host_temp_.load();
            } else {
                temp = 30.0; // Generic sensor initial value
            }
            initial_status[s] = {{"temperature", temp}};
        } else if (s.rfind("temperature_fan ", 0) == 0) {
            initial_status[s] = {{"temperature", 35.0}, {"target", 40.0}, {"speed", 0.0}};
        } else if (s.rfind("tmc2240 ", 0) == 0 || s.rfind("tmc5160 ", 0) == 0) {
            // TMC stepper drivers with built-in temperature
            double temp = 55.0 + (std::hash<std::string>{}(s) % 20);
            initial_status[s] = {{"temperature", temp}};
        }
    }

    // Add filament sensor states
    // Check HELIX_MOCK_FILAMENT_STATE env var for initial state (default: detected)
    // Format: "sensor:state,sensor:state" e.g., "fsensor:empty" or "fsensor:detected,encoder:empty"
    bool default_detected = true;
    const char* state_env = std::getenv("HELIX_MOCK_FILAMENT_STATE");
    std::map<std::string, bool> sensor_states;

    if (state_env) {
        // Parse state overrides
        std::string states_str(state_env);
        size_t pos = 0;
        while ((pos = states_str.find(',')) != std::string::npos || !states_str.empty()) {
            std::string token = (pos != std::string::npos) ? states_str.substr(0, pos) : states_str;
            size_t colon = token.find(':');
            if (colon != std::string::npos) {
                std::string name = token.substr(0, colon);
                std::string state = token.substr(colon + 1);
                sensor_states[name] = (state != "empty" && state != "0" && state != "false");
            }
            if (pos == std::string::npos)
                break;
            states_str.erase(0, pos + 1);
        }
    }

    // Add state for each discovered filament sensor
    for (const auto& sensor : discovery_.filament_sensors()) {
        // Extract sensor name from "filament_switch_sensor fsensor" -> "fsensor"
        size_t space = sensor.rfind(' ');
        std::string short_name = (space != std::string::npos) ? sensor.substr(space + 1) : sensor;

        bool detected = default_detected;
        auto it = sensor_states.find(short_name);
        if (it != sensor_states.end()) {
            detected = it->second;
        }

        // Filament sensor state format from Klipper
        initial_status[sensor] = {{"filament_detected", detected}, {"enabled", true}};
    }

    // Add width sensor data (Hall-effect filament diameter measurement)
    initial_status["hall_filament_width_sensor"] = {
        {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};

    // Add probe sensor status data (matches objects added in populate_capabilities)
    {
        const char* probe_env = std::getenv("HELIX_MOCK_PROBE_TYPE");
        std::string mock_probe_type = (probe_env && probe_env[0]) ? probe_env : "cartographer";

        if (mock_probe_type == "cartographer") {
            initial_status["cartographer"] = {{"last_z_result", -0.425}, {"z_offset", 0.0}};
        } else if (mock_probe_type == "beacon") {
            initial_status["beacon"] = {{"last_z_result", -0.312}, {"z_offset", 0.0}};
        } else if (mock_probe_type == "bltouch") {
            initial_status["bltouch"] = {{"last_z_result", 0.130}, {"z_offset", -1.850}};
        } else if (mock_probe_type == "loadcell") {
            initial_status["probe"] = {{"last_z_result", 0.0}, {"z_offset", nullptr}};
        } else if (mock_probe_type != "none") {
            initial_status["probe"] = {{"last_z_result", 0.0}, {"z_offset", -0.250}};
        }
    }

    // Chamber backend diagnostics + filter pin (e.g. dragonbreath trio via
    // HELIX_MOCK_OBJECTS). Tail of the builder; keys are distinct from every
    // merge above (the LED loop only walks profile LEDs in discovery_.leds(),
    // which never contains env-materialized pins).
    append_chamber_backend_status(initial_status, 0.0);

    spdlog::debug("[MoonrakerClientMock] Dispatching initial state: extruder={}/{}°C, bed={}/{}°C, "
                  "homed_axes='{}', leds={}, filament_sensors={}",
                  ext_temp, ext_target, bed_temp_val, bed_target_val, homed, led_json.size(),
                  discovery_.filament_sensors().size());

    // Use the base class dispatch method (same as real client)
    dispatch_status_update(initial_status);
}

TemperatureStore MoonrakerClientMock::build_historical_temperature_store() const {
    // ~10 minutes of 1 Hz history emitted as Moonraker's flat per-key arrays.
    // Profile: heat-up ramp -> hold at target (PID ripple) -> exponential
    // cooldown. Deterministic (fixed pseudo-random seed) so the curve is stable
    // across runs. Ends near ambient, matching the idle live simulation so the
    // seeded history joins the live graph without a visible step (#944).
    constexpr int SAMPLES = 600;      // 10 min @ 1 Hz
    constexpr int HEAT_SAMPLES = 90;  // ~90 s ramp to target
    constexpr int HOLD_SAMPLES = 150; // ~2.5 min at target
    // remaining ~360 s: exponential cooldown back toward ambient
    constexpr double PEAK_EXTRUDER = 215.0;
    constexpr double PEAK_BED = 60.0;
    constexpr double EXT_TAU = 90.0;  // extruder cooldown time constant (s)
    constexpr double BED_TAU = 140.0; // bed cools slower

    // Deterministic pseudo-random noise in [-1, 1] (no std::random_device).
    uint32_t rng_state = 22221;
    auto noise = [&rng_state](double amplitude) -> double {
        rng_state = (rng_state * 1103515245u + 12345u) & 0x7fffffff;
        return ((static_cast<double>(rng_state) / 0x3fffffff) - 1.0) * amplitude;
    };

    TemperatureStore store;
    TemperatureStoreSeries& ext = store["extruder"];
    TemperatureStoreSeries& bed = store["heater_bed"];
    for (TemperatureStoreSeries* s : {&ext, &bed}) {
        s->temperatures.reserve(SAMPLES);
        s->targets.reserve(SAMPLES);
        s->powers.reserve(SAMPLES);
    }

    for (int i = 0; i < SAMPLES; ++i) {
        double ext_temp, bed_temp, ext_target, bed_target, ext_power, bed_power;
        if (i < HEAT_SAMPLES) {
            // Heating: linear ramp toward target, full heater power.
            double p = static_cast<double>(i) / HEAT_SAMPLES;
            ext_temp = ROOM_TEMP + (PEAK_EXTRUDER - ROOM_TEMP) * p;
            bed_temp = ROOM_TEMP + (PEAK_BED - ROOM_TEMP) * std::min(1.0, p * 1.2);
            ext_target = PEAK_EXTRUDER;
            bed_target = PEAK_BED;
            ext_power = 1.0;
            bed_power = 1.0;
        } else if (i < HEAT_SAMPLES + HOLD_SAMPLES) {
            // Hold: small PID oscillation around target.
            double o = i - HEAT_SAMPLES;
            ext_temp = PEAK_EXTRUDER + 0.8 * std::sin(o * 0.15) + 0.3 * std::cos(o * 0.31);
            bed_temp = PEAK_BED + 0.4 * std::sin(o * 0.12);
            ext_target = PEAK_EXTRUDER;
            bed_target = PEAK_BED;
            ext_power = 0.30 + 0.10 * std::sin(o * 0.15);
            bed_power = 0.25 + 0.08 * std::sin(o * 0.12);
        } else {
            // Cooldown: heaters off, exponential decay toward ambient.
            double ct = i - HEAT_SAMPLES - HOLD_SAMPLES; // seconds since cooldown start
            ext_temp = ROOM_TEMP + (PEAK_EXTRUDER - ROOM_TEMP) * std::exp(-ct / EXT_TAU);
            bed_temp = ROOM_TEMP + (PEAK_BED - ROOM_TEMP) * std::exp(-ct / BED_TAU);
            ext_target = 0.0;
            bed_target = 0.0;
            ext_power = 0.0;
            bed_power = 0.0;
        }

        ext.temperatures.push_back(static_cast<float>(ext_temp + noise(0.3)));
        ext.targets.push_back(static_cast<float>(ext_target));
        ext.powers.push_back(static_cast<float>(ext_power));
        bed.temperatures.push_back(static_cast<float>(bed_temp + noise(0.2)));
        bed.targets.push_back(static_cast<float>(bed_target));
        bed.powers.push_back(static_cast<float>(bed_power));
    }

    // Discovered temperature sensors: gentle sinusoids, no target/power.
    for (const auto& sensor : discovery_.sensors()) {
        if (sensor.rfind("temperature_sensor ", 0) != 0) {
            continue;
        }
        const std::string name = sensor.substr(19); // strip "temperature_sensor " prefix
        TemperatureStoreSeries& series = store[sensor];
        series.temperatures.reserve(SAMPLES);
        for (int i = 0; i < SAMPLES; ++i) {
            double t_sec = -(SAMPLES - i); // negative = in the past
            double temp;
            if (name.find("chamber") != std::string::npos) {
                temp = 35.0 + 8.0 * std::sin(2.0 * M_PI * t_sec / 300.0);
            } else if (name.find("mcu") != std::string::npos) {
                temp = 42.0 + 3.0 * std::sin(2.0 * M_PI * t_sec / 200.0);
            } else {
                temp = 30.0 + 2.0 * std::sin(2.0 * M_PI * t_sec / 180.0);
            }
            series.temperatures.push_back(static_cast<float>(temp + noise(0.4)));
        }
    }

    return store;
}

void MoonrakerClientMock::dispatch_historical_temperatures() {
    // Generate 2-3 minutes of synthetic temperature history
    // At 250ms intervals, that's ~600 data points for 2.5 minutes
    constexpr int HISTORY_DURATION_MS = 150000; // 2.5 minutes of history
    constexpr int SAMPLE_INTERVAL_MS = 250;     // Same as SIMULATION_INTERVAL_MS
    constexpr int HISTORY_SAMPLES = HISTORY_DURATION_MS / SAMPLE_INTERVAL_MS;

    spdlog::debug(
        "[MoonrakerClientMock] Dispatching {} historical temperature samples ({} seconds)",
        HISTORY_SAMPLES, HISTORY_DURATION_MS / 1000);

    // Simulate a realistic temperature profile: heating up to ~60°C then partial cooldown
    // This creates an interesting curve for debugging/visualization
    //
    // Profile: Start at room temp -> heat to 60°C (extruder) / 40°C (bed) -> partial cooldown
    // Timing: ~50s heating, ~30s hold, ~70s cooling (ends at ~35°C extruder, ~30°C bed)
    constexpr double PEAK_EXTRUDER_TEMP = 60.0;
    constexpr double PEAK_BED_TEMP = 40.0;
    constexpr int HEAT_PHASE_SAMPLES = 200; // ~50 seconds at 250ms = 200 samples
    constexpr int HOLD_PHASE_SAMPLES = 120; // ~30 seconds hold at peak
    // Cooling phase = remaining samples (~70s, cools extruder ~20°C to ~40°C)

    // Copy callbacks to avoid holding lock during dispatch
    std::vector<std::function<void(const json&)>> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy.reserve(notify_callbacks_.size());
        for (const auto& [id, cb] : notify_callbacks_) {
            callbacks_copy.push_back(cb);
        }
    }

    // If no callbacks registered yet, skip (caller should register before connect)
    if (callbacks_copy.empty()) {
        spdlog::warn(
            "[MoonrakerClientMock] No callbacks registered for historical temps - skipping");
        return;
    }

    // Generate and dispatch historical samples with realistic noise
    double ext_temp_hist = ROOM_TEMP;
    double bed_temp_hist = ROOM_TEMP;
    const double dt_sec = SAMPLE_INTERVAL_MS / 1000.0;

    // Simple pseudo-random number generator for deterministic noise
    // (Avoids std::random_device which could affect startup time)
    auto pseudo_random = [](int seed) -> double {
        // Linear congruential generator with normalized output [-1, 1]
        static uint32_t state = 12345;
        state = (state * 1103515245 + seed + 12345) & 0x7fffffff;
        return (static_cast<double>(state) / 0x3fffffff) - 1.0;
    };

    for (int i = 0; i < HISTORY_SAMPLES; i++) {
        // Calculate simulated timestamp (negative = in the past)
        double timestamp_sec = -((HISTORY_SAMPLES - i) * dt_sec);

        // Update base temperatures based on phase
        if (i < HEAT_PHASE_SAMPLES) {
            // Heating phase: ramp up to peak (slightly faster at start, slower near target)
            double progress = static_cast<double>(i) / HEAT_PHASE_SAMPLES;
            double rate_multiplier = 1.0 + 0.3 * (1.0 - progress); // Faster early, slower late
            ext_temp_hist += EXTRUDER_HEAT_RATE * dt_sec * rate_multiplier;
            if (ext_temp_hist > PEAK_EXTRUDER_TEMP)
                ext_temp_hist = PEAK_EXTRUDER_TEMP;

            bed_temp_hist += BED_HEAT_RATE * dt_sec * rate_multiplier;
            if (bed_temp_hist > PEAK_BED_TEMP)
                bed_temp_hist = PEAK_BED_TEMP;
        } else if (i < HEAT_PHASE_SAMPLES + HOLD_PHASE_SAMPLES) {
            // Hold phase: PID oscillation around target (realistic behavior)
            double offset = i - HEAT_PHASE_SAMPLES;
            ext_temp_hist =
                PEAK_EXTRUDER_TEMP + 0.8 * std::sin(offset * 0.15) + 0.3 * std::cos(offset * 0.31);
            bed_temp_hist =
                PEAK_BED_TEMP + 0.4 * std::sin(offset * 0.12) + 0.15 * std::cos(offset * 0.27);
        } else {
            // Cooling phase: exponential decay (more realistic than linear)
            int cool_sample = i - HEAT_PHASE_SAMPLES - HOLD_PHASE_SAMPLES;
            double cool_time = cool_sample * dt_sec;
            // Exponential decay: T(t) = T_ambient + (T_0 - T_ambient) * e^(-t/tau)
            double ext_tau = 40.0; // Extruder thermal time constant (seconds)
            double bed_tau = 80.0; // Bed thermal time constant (slower)
            ext_temp_hist =
                ROOM_TEMP + (PEAK_EXTRUDER_TEMP - ROOM_TEMP) * std::exp(-cool_time / ext_tau);
            bed_temp_hist =
                ROOM_TEMP + (PEAK_BED_TEMP - ROOM_TEMP) * std::exp(-cool_time / bed_tau);
        }

        // Add realistic sensor noise (±0.3°C for extruder, ±0.2°C for bed)
        double ext_noise = pseudo_random(i * 2) * 0.3;
        double bed_noise = pseudo_random(i * 2 + 1) * 0.2;

        double ext_with_noise = ext_temp_hist + ext_noise;
        double bed_with_noise = bed_temp_hist + bed_noise;

        // Build minimal status object (only temperature data needed for graphs)
        json status_obj = {{"extruder", {{"temperature", ext_with_noise}, {"target", 0.0}}},
                           {"heater_bed", {{"temperature", bed_with_noise}, {"target", 0.0}}}};

        // Add width sensor data (Hall-effect filament diameter measurement)
        // Always include to ensure WidthSensorManager receives updates
        status_obj["hall_filament_width_sensor"] = {
            {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};

        // Add historical temperature data for all temperature sensors
        for (const auto& s : discovery_.sensors()) {
            if (s.rfind("temperature_sensor ", 0) == 0) {
                std::string sensor_name = s.substr(19);
                double temp = 25.0;
                double noise = pseudo_random(i * 3) * 0.5;
                if (sensor_name.find("chamber") != std::string::npos) {
                    constexpr double CHAMBER_MIN = 25.0, CHAMBER_MAX = 45.0, CHAMBER_PERIOD = 120.0;
                    double mid = (CHAMBER_MIN + CHAMBER_MAX) / 2.0;
                    double amp = (CHAMBER_MAX - CHAMBER_MIN) / 2.0;
                    temp = mid + amp * std::sin(2.0 * M_PI * timestamp_sec / CHAMBER_PERIOD);
                } else if (sensor_name.find("mcu") != std::string::npos) {
                    temp = 42.0 + 3.0 * std::sin(2.0 * M_PI * timestamp_sec / 120.0);
                } else if (sensor_name.find("raspberry") != std::string::npos ||
                           sensor_name.find("host") != std::string::npos || sensor_name == "rpi") {
                    temp = 52.0 + 4.0 * std::sin(2.0 * M_PI * timestamp_sec / 75.0);
                } else {
                    temp = 30.0 + 2.0 * std::sin(2.0 * M_PI * timestamp_sec / 100.0);
                }
                status_obj[s] = {{"temperature", temp + noise}};
            } else if (s.rfind("temperature_fan ", 0) == 0) {
                double temp = 35.0 + 3.0 * std::sin(2.0 * M_PI * timestamp_sec / 80.0);
                status_obj[s] = {{"temperature", temp}, {"target", 40.0}, {"speed", 0.5}};
            }
        }

        json notification = {{"method", "notify_status_update"},
                             {"params", json::array({status_obj, timestamp_sec})}};

        // Dispatch to all callbacks
        for (const auto& cb : callbacks_copy) {
            if (cb) {
                cb(notification);
            }
        }
    }

    // Store final historical values as current temps
    extruder_temp_.store(ext_temp_hist);
    bed_temp_.store(bed_temp_hist);
    // Store chamber temp at midpoint for initial state
    if (has_chamber_sensor()) {
        chamber_temp_.store(35.0);
    }

    spdlog::debug("[MoonrakerClientMock] Historical temps dispatched: final extruder={:.1f}°C, "
                  "bed={:.1f}°C",
                  ext_temp_hist, bed_temp_hist);
}

void MoonrakerClientMock::set_extruder_target(double target) {
    extruder_target_.store(target);
}

void MoonrakerClientMock::set_bed_target(double target) {
    bed_target_.store(target);
}

void MoonrakerClientMock::set_chamber_target(double target) {
    chamber_target_.store(target);
}

void MoonrakerClientMock::dispatch_method_callback(const std::string& method, const json& msg) {
    std::vector<std::function<void(const json&)>> callbacks_to_invoke;

    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto method_it = method_callbacks_.find(method);
        if (method_it != method_callbacks_.end()) {
            for (auto& [handler_name, cb] : method_it->second) {
                callbacks_to_invoke.push_back(cb);
            }
        }
    }

    // Invoke callbacks outside the lock to prevent deadlocks
    for (auto& cb : callbacks_to_invoke) {
        cb(msg);
    }
}

void MoonrakerClientMock::start_temperature_simulation() {
    // Use exchange for atomic check-and-set - prevents race condition if called concurrently
    bool was_running = simulation_running_.exchange(true);
    spdlog::debug("[MoonrakerClientMock] start_temperature_simulation: was_running={}",
                  was_running);
    if (was_running) {
        spdlog::warn("[MoonrakerClientMock] Simulation already running, skipping thread start");
        return;
    }

    simulation_thread_ = std::thread(&MoonrakerClientMock::temperature_simulation_loop, this);
    spdlog::debug("[MoonrakerClientMock] Temperature simulation started");
}

void MoonrakerClientMock::stop_temperature_simulation(bool during_destruction) {
    // Use exchange for atomic check-and-clear - prevents double-join race condition
    // This ensures only one caller proceeds to join the thread
    if (!simulation_running_.exchange(false)) {
        return; // Was already stopped (or never started)
    }

    // Wake the simulation thread so it exits promptly instead of waiting for sleep
    sim_cv_.notify_one();

    if (simulation_thread_.joinable()) {
        simulation_thread_.join();
    }
    // Skip logging during static destruction - spdlog may already be destroyed
    if (!during_destruction) {
        spdlog::info("[MoonrakerClientMock] Temperature simulation stopped");
    }
}

void MoonrakerClientMock::temperature_simulation_loop() {
    spdlog::debug("[MoonrakerClientMock] temperature_simulation_loop ENTERED");
    const double base_dt = SIMULATION_INTERVAL_MS / 1000.0; // Base time step (0.5s)

    while (simulation_running_.load()) {
        uint32_t tick = tick_count_.fetch_add(1);

        // Get speedup factor and calculate effective time step
        double speedup = speedup_factor_.load();
        double effective_dt = base_dt * speedup; // Simulated time step

        // Get current temperature state
        double ext_temp = extruder_temp_.load();
        double ext_target = extruder_target_.load();
        double bed_temp_val = bed_temp_.load();
        double bed_target_val = bed_target_.load();

        // Continuous variation parameters for idle/room temp state
        // Uses sinusoidal waves with different periods to create natural-looking fluctuation
        // This ensures graphs always have data to display during testing
        constexpr double IDLE_VARIATION_AMPLITUDE = 1.5; // +/- 1.5°C variation
        constexpr double EXTRUDER_WAVE_PERIOD = 45.0;    // 45 second period for extruder
        constexpr double BED_WAVE_PERIOD = 60.0;         // 60 second period for bed
        constexpr double PHASE_OFFSET = 1.57;            // Phase offset between heaters (pi/2)

        double sim_time = tick * base_dt; // Simulated elapsed time in seconds

        // Simulate extruder temperature change (scaled by speedup)
        if (ext_target > 0) {
            if (ext_temp < ext_target) {
                ext_temp += EXTRUDER_HEAT_RATE * effective_dt;
                if (ext_temp > ext_target)
                    ext_temp = ext_target;
            } else if (ext_temp > ext_target) {
                ext_temp -= EXTRUDER_COOL_RATE * effective_dt;
                if (ext_temp < ext_target)
                    ext_temp = ext_target;
            }
        } else {
            // Cool toward room temp, then add continuous variation
            if (ext_temp > ROOM_TEMP + IDLE_VARIATION_AMPLITUDE) {
                ext_temp -= EXTRUDER_COOL_RATE * effective_dt;
            } else {
                // At room temp: apply sinusoidal variation for continuous graph updates
                double wave = std::sin(2.0 * M_PI * sim_time / EXTRUDER_WAVE_PERIOD);
                ext_temp = ROOM_TEMP + IDLE_VARIATION_AMPLITUDE * wave;
            }
        }
        extruder_temp_.store(ext_temp);

        // Simulate bed temperature change (scaled by speedup)
        if (bed_target_val > 0) {
            if (bed_temp_val < bed_target_val) {
                bed_temp_val += BED_HEAT_RATE * effective_dt;
                if (bed_temp_val > bed_target_val)
                    bed_temp_val = bed_target_val;
            } else if (bed_temp_val > bed_target_val) {
                bed_temp_val -= BED_COOL_RATE * effective_dt;
                if (bed_temp_val < bed_target_val)
                    bed_temp_val = bed_target_val;
            }
        } else {
            // Cool toward room temp, then add continuous variation
            if (bed_temp_val > ROOM_TEMP + IDLE_VARIATION_AMPLITUDE) {
                bed_temp_val -= BED_COOL_RATE * effective_dt;
            } else {
                // At room temp: apply sinusoidal variation (phase offset from extruder)
                double wave = std::sin(2.0 * M_PI * sim_time / BED_WAVE_PERIOD + PHASE_OFFSET);
                bed_temp_val = ROOM_TEMP + IDLE_VARIATION_AMPLITUDE * wave;
            }
        }
        bed_temp_.store(bed_temp_val);

        // Simulate chamber temperature change (scaled by speedup)
        // Chamber responds to target temperature like bed/extruder, but slower
        if (has_chamber_sensor()) {
            constexpr double CHAMBER_IDLE_VARIATION_AMPLITUDE = 1.5;
            constexpr double CHAMBER_WAVE_PERIOD = 90.0; // 90 second period for idle variation

            double chamber = chamber_temp_.load();
            double chamber_target = chamber_target_.load();

            if (chamber_target > 0) {
                // Heating/cooling toward target
                if (chamber < chamber_target) {
                    chamber += CHAMBER_HEAT_RATE * effective_dt;
                    if (chamber > chamber_target)
                        chamber = chamber_target;
                } else if (chamber > chamber_target) {
                    chamber -= CHAMBER_COOL_RATE * effective_dt;
                    if (chamber < chamber_target)
                        chamber = chamber_target;
                }
            } else {
                // Cool toward room temp, then add continuous variation
                if (chamber > ROOM_TEMP + CHAMBER_IDLE_VARIATION_AMPLITUDE) {
                    chamber -= CHAMBER_COOL_RATE * effective_dt;
                } else {
                    // At room temp: apply sinusoidal variation
                    double wave = std::sin(2.0 * M_PI * sim_time / CHAMBER_WAVE_PERIOD + 2.0);
                    chamber = ROOM_TEMP + CHAMBER_IDLE_VARIATION_AMPLITUDE * wave;
                }
            }
            chamber_temp_.store(chamber);
        }

        // Simulate MCU temperature (stable 40-55°C, slight load correlation)
        {
            constexpr double MCU_BASE = 42.0;
            constexpr double MCU_WAVE_PERIOD = 120.0;
            constexpr double MCU_AMPLITUDE = 3.0;
            constexpr double MCU_PRINT_OFFSET = 5.0; // Higher during printing

            double mcu = MCU_BASE;
            MockPrintPhase current_phase = print_phase_.load();
            if (current_phase == MockPrintPhase::PRINTING ||
                current_phase == MockPrintPhase::PREHEAT) {
                mcu += MCU_PRINT_OFFSET;
            }
            double wave = std::sin(2.0 * M_PI * sim_time / MCU_WAVE_PERIOD);
            mcu += MCU_AMPLITUDE * wave;
            mcu_temp_.store(mcu);
        }

        // Simulate host/RPi temperature (45-65°C, higher under load)
        {
            constexpr double HOST_BASE = 52.0;
            constexpr double HOST_WAVE_PERIOD = 75.0;
            constexpr double HOST_AMPLITUDE = 4.0;
            constexpr double HOST_PRINT_OFFSET = 8.0;

            double host = HOST_BASE;
            MockPrintPhase current_phase = print_phase_.load();
            if (current_phase == MockPrintPhase::PRINTING ||
                current_phase == MockPrintPhase::PREHEAT) {
                host += HOST_PRINT_OFFSET;
            }
            double wave = std::sin(2.0 * M_PI * sim_time / HOST_WAVE_PERIOD + 1.0);
            host += HOST_AMPLITUDE * wave;
            host_temp_.store(host);
        }

        // ========== Phase-Based Print Simulation ==========
        MockPrintPhase phase = print_phase_.load();

        switch (phase) {
        case MockPrintPhase::IDLE: {
            // Check idle timeout (only when not printing)
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_time_.load())
                    .count();

            if (!idle_timeout_triggered_.load() &&
                elapsed >= static_cast<int64_t>(idle_timeout_seconds_.load())) {
                idle_timeout_triggered_.store(true);
                motors_enabled_.store(false);
                spdlog::info("[MoonrakerClientMock] Idle timeout triggered after {}s", elapsed);

                // Dispatch stepper_enable update
                json stepper_status = {{"stepper_enable",
                                        {{"steppers",
                                          {{"stepper_x", false},
                                           {"stepper_y", false},
                                           {"stepper_z", false},
                                           {"extruder", false}}}}}};
                dispatch_status_update(stepper_status);
            }
            break;
        }

        case MockPrintPhase::PREHEAT:
            // Advance PRINT_START simulation (dispatches G-code responses)
            advance_print_start_simulation();

            // Check if both extruder and bed have reached target temps
            if (is_temp_stable(ext_temp, ext_target) &&
                is_temp_stable(bed_temp_val, bed_target_val)) {
                // Dispatch layer 1 marker before transitioning to PRINTING
                uint8_t current_sim_phase = simulated_print_start_phase_.load();
                if (current_sim_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::LAYER_1)) {
                    dispatch_gcode_response("SET_PRINT_STATS_INFO CURRENT_LAYER=1");
                    dispatch_gcode_response("// Layer 1 starting");
                    simulated_print_start_phase_.store(
                        static_cast<uint8_t>(SimulatedPrintStartPhase::LAYER_1));
                }

                // Transition to PRINTING phase
                print_phase_.store(MockPrintPhase::PRINTING);
                printing_start_time_ = std::chrono::steady_clock::now();
                spdlog::debug("[MoonrakerClientMock] Preheat complete - starting print");
            }
            break;

        case MockPrintPhase::PRINTING:
            // Advance print progress based on file-estimated duration
            advance_print_progress(effective_dt);

            // Check for completion
            if (print_progress_.load() >= 1.0) {
                print_phase_.store(MockPrintPhase::COMPLETE);
                print_state_.store(3); // "complete" for backward compatibility
                extruder_target_.store(0.0);
                bed_target_.store(0.0);
                spdlog::info("[MoonrakerClientMock] Print complete!");
                dispatch_print_state_notification("complete");
            }
            break;

        case MockPrintPhase::PAUSED:
            // Temps maintained (targets unchanged), no progress advance
            break;

        case MockPrintPhase::COMPLETE:
        case MockPrintPhase::CANCELLED:
            // Cooling down - transition to IDLE when cool enough
            if (ext_temp < 50.0 && bed_temp_val < 35.0) {
                print_phase_.store(MockPrintPhase::IDLE);
                print_state_.store(0); // "standby" for backward compatibility
                {
                    std::lock_guard<std::mutex> lock(print_mutex_);
                    print_filename_.clear();
                }
                print_progress_.store(0.0);
                {
                    std::lock_guard<std::mutex> lock(metadata_mutex_);
                    print_metadata_.reset();
                }
                spdlog::info("[MoonrakerClientMock] Cooldown complete - returning to idle");
                dispatch_print_state_notification("standby");
            }
            break;

        case MockPrintPhase::ERROR:
            // Stay in error state until explicitly cleared (via new print start)
            break;
        }

        // ========== Position and Motion State ==========
        double x, y, z;
        read_position_snapshot(x, y, z);

        std::string homed;
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            homed = homed_axes_;
        }

        // Simulate speed/flow oscillation (90-110%) - only during printing
        int speed = 100;
        int flow = 100;
        if (phase == MockPrintPhase::PRINTING) {
            speed = 100 + static_cast<int>(10.0 * std::sin(tick / 20.0));
            flow = 100 + static_cast<int>(5.0 * std::cos(tick / 30.0));
        }
        speed_factor_.store(speed);
        flow_factor_.store(flow);

        // Part fan at ~30% during printing (typical for PLA)
        int fan = 0;
        if (phase == MockPrintPhase::PRINTING) {
            fan = 77; // ~30% of 255
        }
        fan_speed_.store(fan);

        // Klipper [heater_fan] auto-trips at heater_temp (default 50°C). Mirror that
        // here so the mock's hotend fan reads 100% whenever the extruder is hot
        // (printing, preheating, or cooling down from a recent print).
        double hotend_fan_speed = (ext_temp > 50.0) ? 1.0 : 0.0;

        // ========== Build and Dispatch Status Notification ==========
        // Only dispatch notifications every NOTIFICATION_INTERVAL_TICKS to reduce log spam
        // Physics still runs every tick for smooth temperature changes
        if (tick % NOTIFICATION_INTERVAL_TICKS != 0) {
            // Sleep and continue without dispatching
            std::unique_lock<std::mutex> lock(sim_mutex_);
            sim_cv_.wait_for(lock, std::chrono::milliseconds(SIMULATION_INTERVAL_MS),
                             [this] { return !simulation_running_.load(); });
            continue;
        }

        std::string print_state_str = get_print_state_string();
        std::string filename;
        {
            std::lock_guard<std::mutex> lock(print_mutex_);
            filename = print_filename_;
        }

        // Get layer info for enhanced status
        int current_layer = get_current_layer();
        int total_layers = get_total_layers();
        double total_time;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            total_time = print_metadata_.estimated_time_seconds;
        }
        double progress = print_progress_.load();
        double elapsed = progress * total_time;

        // Simulate filament consumption proportional to progress
        double filament_total_mm;
        {
            std::lock_guard<std::mutex> lock(metadata_mutex_);
            filament_total_mm = print_metadata_.filament_mm;
        }
        double filament_used = (filament_total_mm > 0) ? progress * filament_total_mm : 0.0;

        // Get Z offset for gcode_move
        double z_offset = gcode_offset_z_.load();

        // Build notification JSON (enhanced Moonraker format with layer info)
        json status_obj = {
            {"extruder", {{"temperature", ext_temp}, {"target", ext_target}}},
            {"heater_bed", {{"temperature", bed_temp_val}, {"target", bed_target_val}}},
            {"toolhead",
             {{"position", {x, y, z, 0.0}},
              {"homed_axes", homed},
              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
              {"axis_maximum",
               {persona_axis_maximum(printer_type_)[0], persona_axis_maximum(printer_type_)[1],
                persona_axis_maximum(printer_type_)[2], 0.0}},
              {"kinematics", discovery_.hardware().kinematics()}}},
            {"gcode_move",
             {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
              {"speed_factor", speed / 100.0},
              {"extrude_factor", flow / 100.0},
              {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
            {"fan", {{"speed", fan / 255.0}}},
            {"print_stats",
             {{"state", print_state_str},
              {"filename", filename},
              {"print_duration", elapsed},
              {"total_duration", elapsed},    // Wall-clock elapsed (matches real Moonraker)
              {"estimated_time", total_time}, // Slicer estimate (for completion modal)
              {"filament_used", filament_used},
              {"message", ""},
              {"info", {{"current_layer", current_layer}, {"total_layer", total_layers}}}}},
            {"virtual_sdcard",
             {{"file_path", filename},
              {"progress", progress},
              {"is_active",
               phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT}}},
            // display_status: M73 slicer progress + M117 display message.
            // A user-set M117 message (even cleared to "") always wins over the
            // canned phase strings below - those only apply before the user has
            // ever sent an M117.
            {"display_status",
             {{"progress", progress},
              {"message",
               [&]() -> json {
                   {
                       std::lock_guard<std::mutex> lock(display_message_mutex_);
                       if (display_message_set_)
                           return display_message_;
                   }
                   if (phase == MockPrintPhase::PREHEAT)
                       return "Heating...";
                   if (phase == MockPrintPhase::PRINTING && progress < 0.02)
                       return "Purging nozzle";
                   if (phase == MockPrintPhase::PRINTING)
                       return nullptr; // Most of print has no active message
                   return nullptr;
               }()}}},
            // stepper_enable tracks actual motor driver state (immediate response to M84)
            {"stepper_enable",
             {{"steppers",
               {{"stepper_x", motors_enabled_.load()},
                {"stepper_y", motors_enabled_.load()},
                {"stepper_z", motors_enabled_.load()},
                {"extruder", motors_enabled_.load()}}}}},
            // idle_timeout tracks activity state: "Printing", "Ready", or "Idle" (after timeout)
            {"idle_timeout", {{"state", [&]() -> std::string {
                                   if (phase == MockPrintPhase::PRINTING ||
                                       phase == MockPrintPhase::PREHEAT) {
                                       return "Printing";
                                   } else if (idle_timeout_triggered_.load()) {
                                       return "Idle";
                                   } else {
                                       return "Ready";
                                   }
                               }()}}}};

        // Add width sensor data (Hall-effect filament diameter measurement)
        // Always include to ensure WidthSensorManager receives updates
        status_obj["hall_filament_width_sensor"] = {
            {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};

        // Toolchanger mock mode: keep the 3 extra extruder temps on the live
        // subscription stream so they don't go stale. Static values (matching the
        // initial query response) chosen for easy heating-state color eyeballing:
        // extruder1 HEATING (150/250), extruder2 AT-TEMP (248/250),
        // extruder3 COOLING (200/0). Not wired into the ramping atomics by design.
        if (is_mock_toolchanger() || is_mock_medusahc()) {
            status_obj["extruder1"] = {{"temperature", 150.0}, {"target", 250.0}};
            status_obj["extruder2"] = {{"temperature", 248.0}, {"target", 250.0}};
            status_obj["extruder3"] = {{"temperature", 200.0}, {"target", 0.0}};
        }

        // MedusaHC swap simulation. gcode_script() starts a swap by arming the
        // phase; this advances it one step per notification (~1s) so the phases
        // reach the UI as separate frames the way the real controller reports
        // them, instead of the whole swap collapsing into one update.
        if (is_mock_medusahc()) {
            advance_medusa_swap();
            if (auto medusa = medusa_status_json(); !medusa.empty()) {
                status_obj["medusahc"] = medusa;
            }
            if (auto pw = pin_watch_status_json(); !pw.empty()) {
                status_obj["pin_watch io"] = pw;
            }
        }

        // Add klippy state if not ready (only send when abnormal)
        KlippyState klippy = klippy_state_.load();
        if (klippy != KlippyState::READY) {
            std::string state_str;
            switch (klippy) {
            case KlippyState::STARTUP:
                state_str = "startup";
                break;
            case KlippyState::SHUTDOWN:
                state_str = "shutdown";
                break;
            case KlippyState::ERROR:
                state_str = "error";
                break;
            default:
                state_str = "ready";
                break;
            }
            status_obj["webhooks"] = {{"state", state_str}};
        }

        // Snapshot under the lock rather than iterating discovery_ directly: the
        // list can be reassigned on the main thread mid-iteration. Copy is cheap
        // (a handful of short names) and keeps the lock off the JSON building below.
        std::vector<std::string> fans_snapshot;
        {
            std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
            fans_snapshot = discovery_.fans();
        }

        // Auto-controlled heater_fans follow extruder temperature, matching
        // Klipper behavior. Apply BEFORE the explicit-override loop so users
        // can still pin a fan speed via M106 for testing.
        for (const auto& fan_name : fans_snapshot) {
            if (fan_name.rfind("heater_fan ", 0) == 0) {
                status_obj[fan_name] = {{"speed", hotend_fan_speed}};
            }
        }

        // Override fan speeds with explicitly-set values from fan_speeds_ map
        {
            std::lock_guard<std::mutex> lock(fan_mutex_);
            for (const auto& [name, spd] : fan_speeds_) {
                if (name == "fan") {
                    status_obj["fan"] = {{"speed", spd}};
                } else {
                    status_obj[name] = {{"speed", spd}};
                }
            }
        }

        // Add exclude_object status (excluded + current only, objects sent at start)
        {
            json excluded_array = json::array();
            std::string current_obj;

            if (mock_state_) {
                auto names = mock_state_->get_object_names();
                auto excl = mock_state_->get_excluded_objects();
                for (const auto& obj : excl) {
                    excluded_array.push_back(obj);
                }
                if (!names.empty() &&
                    (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT)) {
                    for (const auto& n : names) {
                        if (excl.count(n) == 0) {
                            current_obj = n;
                            break;
                        }
                    }
                }
            } else {
                std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                for (const auto& obj : excluded_objects_) {
                    excluded_array.push_back(obj);
                }
                if (!object_names_.empty() &&
                    (phase == MockPrintPhase::PRINTING || phase == MockPrintPhase::PREHEAT)) {
                    for (const auto& n : object_names_) {
                        if (excluded_objects_.count(n) == 0) {
                            current_obj = n;
                            break;
                        }
                    }
                }
            }

            // Only send excluded_objects + current_object (objects sent at start)
            status_obj["exclude_object"] = {
                {"excluded_objects", excluded_array},
                {"current_object", current_obj.empty() ? json(nullptr) : json(current_obj)}};
        }

        // Snapshot under the lock — same reasoning as fans_snapshot above.
        std::vector<std::string> sensors_snapshot;
        {
            std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
            sensors_snapshot = discovery_.sensors();
        }

        // Add temperature sensor data for all sensors in the discovery sensors list
        for (const auto& s : sensors_snapshot) {
            if (s.rfind("temperature_sensor ", 0) == 0) {
                std::string sensor_name = s.substr(19);
                double temp = 25.0;
                if (sensor_name.find("chamber") != std::string::npos) {
                    temp = chamber_temp_.load();
                } else if (sensor_name.find("mcu") != std::string::npos) {
                    temp = mcu_temp_.load();
                } else if (sensor_name.find("raspberry") != std::string::npos ||
                           sensor_name.find("host") != std::string::npos || sensor_name == "rpi") {
                    temp = host_temp_.load();
                } else {
                    // Generic sensor: slow drift around 30°C
                    temp = 30.0 + 2.0 * std::sin(2.0 * M_PI * sim_time / 100.0);
                }
                status_obj[s] = {{"temperature", temp}};
            } else if (s.rfind("temperature_fan ", 0) == 0) {
                // Temperature fans have temp, target, and speed
                double temp;
                double target;
                if (s.find("chamber") != std::string::npos) {
                    temp = chamber_temp_.load();
                    target = chamber_target_.load();
                } else {
                    temp = 35.0 + 3.0 * std::sin(2.0 * M_PI * sim_time / 80.0);
                    target = 40.0;
                }
                double speed = target > 0 ? 0.5 : 0.0;
                status_obj[s] = {{"temperature", temp}, {"target", target}, {"speed", speed}};
            } else if (s.rfind("tmc2240 ", 0) == 0 || s.rfind("tmc5160 ", 0) == 0) {
                // TMC stepper drivers: drift around a base temp per-driver
                double base = 55.0 + (std::hash<std::string>{}(s) % 20);
                double temp = base + 3.0 * std::sin(2.0 * M_PI * sim_time / 120.0);
                status_obj[s] = {{"temperature", temp}};
            }
        }

        // Humidity sensor data (BME280 has humidity, temperature, pressure; HTU21D has
        // humidity, temperature)
        {
            // BME280 chamber sensor: humidity varies 40-50%, slow sinusoidal drift
            constexpr double HUMIDITY_WAVE_PERIOD = 180.0; // 3 minute period
            double humidity_wave = std::sin(2.0 * M_PI * sim_time / HUMIDITY_WAVE_PERIOD);
            double chamber_humidity = 45.0 + 5.0 * humidity_wave; // 40-50%
            double chamber_h_temp = chamber_temp_.load();         // Use chamber temperature
            double chamber_pressure = 1013.25 + 2.0 * std::sin(2.0 * M_PI * sim_time / 300.0);
            status_obj["bme280 chamber"] = {{"humidity", chamber_humidity},
                                            {"temperature", chamber_h_temp},
                                            {"pressure", chamber_pressure}};

            // HTU21D dryer sensor: lower humidity (dryer enclosure), 10-20%
            double dryer_humidity = 15.0 + 5.0 * std::sin(2.0 * M_PI * sim_time / 150.0);
            double dryer_temp = 55.0 + 3.0 * std::sin(2.0 * M_PI * sim_time / 200.0);
            status_obj["htu21d dryer"] = {{"humidity", dryer_humidity},
                                          {"temperature", dryer_temp}};
        }

        // Chamber backend diagnostics + filter pin (e.g. dragonbreath trio via
        // HELIX_MOCK_OBJECTS) — drifts with the chamber sim like the sensors above.
        append_chamber_backend_status(status_obj, sim_time);

        json notification = {{"method", "notify_status_update"},
                             {"params", json::array({status_obj, tick * base_dt})}};

        // Push notification through all registered callbacks
        // Two-phase: copy under lock, invoke outside to avoid deadlock
        std::vector<std::function<void(const json&)>> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex_);
            callbacks_copy.reserve(notify_callbacks_.size());
            for (const auto& [id, cb] : notify_callbacks_) {
                callbacks_copy.push_back(cb);
            }
        }
        for (const auto& cb : callbacks_copy) {
            if (cb) {
                cb(notification);
            }
        }

        // Log every 40 ticks (~10 seconds) to confirm loop is running
        if (tick % 40 == 0) {
            spdlog::trace("[MoonrakerClientMock] Simulation tick {} - callbacks={}", tick,
                          callbacks_copy.size());
        }

        // Sleep wall-clock interval with early-exit support for clean shutdown
        // Uses condition_variable wait instead of raw sleep so stop_temperature_simulation()
        // can wake the thread immediately instead of waiting for the full interval
        {
            std::unique_lock<std::mutex> lock(sim_mutex_);
            sim_cv_.wait_for(lock, std::chrono::milliseconds(SIMULATION_INTERVAL_MS),
                             [this] { return !simulation_running_.load(); });
        }
    }
    spdlog::debug("[MoonrakerClientMock] temperature_simulation_loop EXITED");
}

// ============================================================================
// Fan Control Helper Methods
// ============================================================================

void MoonrakerClientMock::set_fan_speed_internal(const std::string& fan_name, double speed) {
    {
        std::lock_guard<std::mutex> lock(fan_mutex_);
        fan_speeds_[fan_name] = speed;
    }

    // Also update the legacy fan_speed_ atomic for backward compatibility
    // (only for part cooling fan "fan")
    if (fan_name == "fan") {
        fan_speed_.store(static_cast<int>(speed * 255.0));
    }

    // Dispatch fan status update
    json fan_status;
    if (fan_name == "fan") {
        // Part cooling fan uses simple format
        fan_status["fan"] = {{"speed", speed}};
    } else {
        // Generic/heater fans use full name as key
        fan_status[fan_name] = {{"speed", speed}};
    }
    dispatch_status_update(fan_status);
}

std::string MoonrakerClientMock::find_fan_by_suffix(const std::string& suffix) const {
    for (const auto& fan : discovery_.fans()) {
        // Match if fan name ends with the suffix (e.g., "nevermore" matches "fan_generic
        // nevermore")
        if (fan.length() >= suffix.length()) {
            size_t suffix_start = fan.length() - suffix.length();
            if (fan.substr(suffix_start) == suffix) {
                return fan;
            }
        }
    }
    return "";
}

// ============================================================================
// G-code Offset Helper Methods
// ============================================================================

void MoonrakerClientMock::dispatch_gcode_move_update() {
    double z_offset = gcode_offset_z_.load();
    int speed = speed_factor_.load();
    int flow = flow_factor_.load();
    double x, y, z;
    read_position_snapshot(x, y, z);

    json gcode_move = {{"gcode_move",
                        {{"gcode_position", {x, y, z, 0.0}},
                         {"speed_factor", speed / 100.0},
                         {"extrude_factor", flow / 100.0},
                         {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}}};
    dispatch_status_update(gcode_move);
}

void MoonrakerClientMock::dispatch_tool_update(int tool) {
    double value = 0.0;
    {
        std::lock_guard<std::mutex> lock(tool_z_offsets_mutex_);
        auto it = tool_z_offsets_.find(tool);
        if (it == tool_z_offsets_.end()) {
            return;
        }
        value = it->second;
    }
    // Only the field that changed, matching Moonraker: it republishes just the
    // deltas, and code that assumes a full object here is code that would break
    // against a real printer.
    json update = {{"tool T" + std::to_string(tool), {{"gcode_z_offset", value}}}};
    dispatch_status_update(update);
}

double MoonrakerClientMock::tool_z_offset(int tool) const {
    std::lock_guard<std::mutex> lock(tool_z_offsets_mutex_);
    auto it = tool_z_offsets_.find(tool);
    if (it != tool_z_offsets_.end()) {
        return it->second;
    }
    // Distinct per-tool seed. All-zero would make "every tool shows the same
    // number" — the characteristic per-tool display bug — look correct.
    return -0.025 * tool;
}

bool MoonrakerClientMock::save_config_pending() const {
    std::lock_guard<std::mutex> lock(pending_config_mutex_);
    return !pending_config_items_.empty();
}

json MoonrakerClientMock::save_config_pending_items() const {
    std::lock_guard<std::mutex> lock(pending_config_mutex_);
    json items = json::object();
    for (const auto& [section, options] : pending_config_items_) {
        json opts = json::object();
        for (const auto& [option, value] : options) {
            opts[option] = value;
        }
        items[section] = opts;
    }
    return items;
}

void MoonrakerClientMock::stage_config_change(const std::string& section, const std::string& option,
                                              const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(pending_config_mutex_);
        pending_config_items_[section][option] = value;
    }
    dispatch_configfile_update();
}

void MoonrakerClientMock::commit_pending_config() {
    std::map<std::string, std::map<std::string, std::string>> pending;
    {
        std::lock_guard<std::mutex> lock(pending_config_mutex_);
        pending.swap(pending_config_items_);
    }
    if (pending.empty()) {
        return;
    }
    for (const auto& [section, options] : pending) {
        // "tool T<n>" is the only section anything here stages. Others are
        // accepted and cleared without a durable store, which is enough to
        // model the pending flag for features that only care a save is owed.
        if (section.rfind("tool T", 0) != 0) {
            continue;
        }
        auto opt = options.find("gcode_z_offset");
        if (opt == options.end()) {
            continue;
        }
        try {
            int tool = std::stoi(section.substr(6));
            std::lock_guard<std::mutex> lock(tool_z_offsets_mutex_);
            tool_z_offsets_saved_[tool] = std::stod(opt->second);
        } catch (...) {
        }
    }
    spdlog::info("[MoonrakerClientMock] SAVE_CONFIG committed {} pending section(s)",
                 pending.size());
    dispatch_configfile_update();
}

void MoonrakerClientMock::dispatch_configfile_update() {
    json update = {{"configfile",
                    {{"save_config_pending", save_config_pending()},
                     {"save_config_pending_items", save_config_pending_items()}}}};
    dispatch_status_update(update);
}

// ============================================================================
// Manual Probe Helper Methods (Z-offset calibration)
// ============================================================================

void MoonrakerClientMock::dispatch_manual_probe_update() {
    bool is_active = manual_probe_active_.load();
    double z_position = manual_probe_z_.load();

    // Build manual_probe status matching Klipper's format:
    // {
    //   "manual_probe": {
    //     "is_active": true/false,
    //     "z_position": float,
    //     "z_position_lower": float (optional),
    //     "z_position_upper": float (optional)
    //   }
    // }
    json manual_probe_status = {
        {"manual_probe",
         {{"is_active", is_active},
          {"z_position", z_position},
          {"z_position_lower", nullptr}, // Not tracking bisection search in mock
          {"z_position_upper", nullptr}}}};

    dispatch_status_update(manual_probe_status);

    spdlog::debug("[MoonrakerClientMock] Dispatched manual_probe update: is_active={}, z={:.3f}",
                  is_active, z_position);
}

// ============================================================================
// G-code Response Simulation (for PRINT_START progress tracking)
// ============================================================================

void MoonrakerClientMock::dispatch_gcode_response(const std::string& line) {
    // Build notify_gcode_response message format:
    // {"method": "notify_gcode_response", "params": ["<line>"]}
    json notification = {{"method", "notify_gcode_response"}, {"params", json::array({line})}};

    // Collect callbacks while holding lock, invoke outside
    std::vector<std::function<void(const json&)>> callbacks_to_invoke;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        auto method_it = method_callbacks_.find("notify_gcode_response");
        if (method_it != method_callbacks_.end()) {
            for (auto& [handler_name, cb] : method_it->second) {
                callbacks_to_invoke.push_back(cb);
            }
        }
    }

    // Invoke callbacks outside lock to prevent deadlock
    for (auto& cb : callbacks_to_invoke) {
        cb(notification);
    }

    spdlog::trace("[MoonrakerClientMock] Dispatched G-code response: {}", line);
}

namespace {

/**
 * @brief Write a mock Klipper-format shaper calibration CSV
 *
 * Generates ~50 frequency bins from 5-200 Hz with a realistic spectrum:
 * base noise floor, a resonance peak, and shaper attenuation curves.
 */
void write_mock_shaper_csv(const std::string& path, char axis) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        spdlog::warn("[MoonrakerClientMock] Failed to write mock CSV to {}", path);
        return;
    }

    // Shaper definitions: name, fitted frequency
    struct ShaperDef {
        const char* name;
        float freq;
    };
    static const ShaperDef shapers[] = {
        {"zv", 59.0f}, {"mzv", 53.8f}, {"ei", 56.2f}, {"2hump_ei", 71.8f}, {"3hump_ei", 89.6f},
    };
    constexpr int num_shapers = 5;

    // Write header line
    ofs << "freq,psd_x,psd_y,psd_z,psd_xyz";
    for (int i = 0; i < num_shapers; ++i) {
        ofs << "," << shapers[i].name << "(" << std::fixed << std::setprecision(1)
            << shapers[i].freq << ")";
    }
    ofs << "\n";

    // RNG for noise variation
    std::mt19937 rng(42 + static_cast<unsigned>(axis)); // Deterministic per-axis
    std::uniform_real_distribution<float> noise_dist(0.8f, 1.2f);

    // Frequency bins the PSD rows (and the shaper curves below) are sampled at
    std::vector<double> bins;
    for (float freq = 5.0f; freq <= 200.0f; freq += 4.0f) {
        bins.push_back(freq);
    }

    // Real transfer curves per shaper (the same math the firmware uses to
    // write these columns), so the shaped-PSD overlays and any client-side
    // re-scoring of the mock data see physically consistent notches rather
    // than a toy quadratic approximation.
    std::vector<std::vector<double>> shaper_curves;
    shaper_curves.reserve(num_shapers);
    for (int i = 0; i < num_shapers; ++i) {
        shaper_curves.push_back(helix::calibration::shaper_transfer_curve(
            shapers[i].name, shapers[i].freq, helix::calibration::SHAPER_DEFAULT_DAMPING_RATIO,
            bins));
    }

    // Resonance peak parameters — should agree with optimal shaper frequencies above
    const float peak_freq = (axis == 'x' || axis == 'X') ? 53.8f : 48.2f;
    const float peak_width = 8.0f; // Hz bandwidth of resonance
    const float peak_amp = 0.02f;  // Peak amplitude
    const float noise_floor = 5e-4f;

    // Generate ~50 bins from 5 to 200 Hz (step ~4 Hz)
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const float freq = static_cast<float>(bins[bin]);

        // Raw PSD: noise floor + Lorentzian resonance peak
        float df = freq - peak_freq;
        float resonance = peak_amp / (1.0f + (df * df) / (peak_width * peak_width));
        float base_psd = noise_floor * noise_dist(rng) + resonance;

        // High-frequency rolloff above 120 Hz
        if (freq > 120.0f) {
            base_psd *= std::exp(-(freq - 120.0f) / 60.0f);
        }

        // PSD for each axis direction (main axis gets full signal)
        float psd_main = base_psd;
        float psd_cross = base_psd * 0.15f * noise_dist(rng); // Cross-axis coupling
        float psd_z = base_psd * 0.08f * noise_dist(rng);
        float psd_xyz = psd_main + psd_cross + psd_z;

        float psd_x = (axis == 'x' || axis == 'X') ? psd_main : psd_cross;
        float psd_y = (axis == 'y' || axis == 'Y') ? psd_main : psd_cross;

        ofs << std::scientific << std::setprecision(3) << freq << "," << psd_x << "," << psd_y
            << "," << psd_z << "," << psd_xyz;

        // Write transfer function coefficients (0-1), matching real Klipper CSV
        // format. The CSV parser multiplies by raw PSD to get shaped PSD for
        // charting. Shapers without a ported curve degrade to a flat passband.
        for (int i = 0; i < num_shapers; ++i) {
            const double attenuation =
                (bin < shaper_curves[i].size()) ? shaper_curves[i][bin] : 1.0;
            ofs << "," << std::fixed << std::setprecision(3) << attenuation;
        }
        ofs << "\n";
    }

    ofs.close();
    spdlog::info("[MoonrakerClientMock] Wrote mock shaper CSV to {}", path);
}

} // anonymous namespace

json MoonrakerClientMock::build_input_shaper_config() const {
    char freq_x[16];
    char freq_y[16];
    snprintf(freq_x, sizeof(freq_x), "%.1f", shaper_freq_x_);
    snprintf(freq_y, sizeof(freq_y), "%.1f", shaper_freq_y_);
    return json{
        {"shaper_type_x", shaper_type_x_}, {"shaper_freq_x", freq_x},
        {"shaper_type_y", shaper_type_y_}, {"shaper_freq_y", freq_y},
        {"damping_ratio_x", "0.1"},        {"damping_ratio_y", "0.1"},
    };
}

void MoonrakerClientMock::dispatch_shaper_calibrate_response(char axis) {
    // Timer-based dispatch for realistic progress animation
    // Matches PID_CALIBRATE timer pattern (line 1389)
    char axis_lower = static_cast<char>(std::tolower(static_cast<unsigned char>(axis)));

    // Build the whole console transcript up front, then play it back one line
    // per tick. Every line here is the wording Klipper and Kalico actually
    // emit — an invented marker line would let the collector's parsing pass in
    // tests while failing against real firmware.
    std::vector<std::string> lines;
    char buf[256];

    // Phase 1: frequency sweep across the configured [resonance_tester] range,
    // ending exactly on max_freq the way a real sweep does.
    constexpr int SWEEP_STEPS = 20;
    const double min_freq = resonance_min_freq_;
    const double max_freq = resonance_max_freq_;
    for (int i = 0; i < SWEEP_STEPS; ++i) {
        const double freq =
            min_freq + (max_freq - min_freq) * i / static_cast<double>(SWEEP_STEPS - 1);
        snprintf(buf, sizeof(buf), "Testing frequency %.0f Hz", freq);
        lines.emplace_back(buf);
    }

    // Phase 2: analysis heartbeats, the sweep-finished marker, then one fit +
    // max_accel per shaper. Creality's K1C build prints "Wait for calculations.."
    // every ~5s through its (compressed here) analysis window before the
    // "Calculating the best" line; the collector treats the repeats as liveness
    // heartbeats, not new events. Ten lines give the phase ~1s of transcript on
    // its own so the panel's spinner + elapsed-seconds label has a window to
    // be observed in.
    for (int i = 0; i < 10; ++i) {
        lines.emplace_back("Wait for calculations..");
    }
    snprintf(buf, sizeof(buf), "Calculating the best input shaper parameters for %c axis",
             axis_lower);
    lines.emplace_back(buf);

    struct ShaperData {
        const char* type;
        float freq;
        float vibrations;
        float smoothing;
        int max_accel;
    };
    static const ShaperData shapers[] = {
        {"zv", 59.0f, 5.2f, 0.045f, 13400},      {"mzv", 53.8f, 1.6f, 0.130f, 4000},
        {"ei", 56.2f, 0.7f, 0.120f, 4600},       {"2hump_ei", 71.8f, 0.0f, 0.076f, 8800},
        {"3hump_ei", 89.6f, 0.0f, 0.076f, 8800},
    };
    for (const auto& sh : shapers) {
        snprintf(buf, sizeof(buf),
                 "Fitted shaper '%s' frequency = %.1f Hz (vibrations = %.1f%%, "
                 "smoothing ~= %.3f)",
                 sh.type, sh.freq, sh.vibrations, sh.smoothing);
        lines.emplace_back(buf);
        snprintf(buf, sizeof(buf),
                 "To avoid too much smoothing with '%s' (scv: 25), suggested max_accel "
                 "<= %d mm/sec^2",
                 sh.type, sh.max_accel);
        lines.emplace_back(buf);
    }

    // Phase 3: recommendation, then the CSV path that terminates the collector.
    snprintf(buf, sizeof(buf), "Recommended shaper_type_%c = mzv, shaper_freq_%c = 53.8 Hz",
             axis_lower, axis_lower);
    lines.emplace_back(buf);
    snprintf(buf, sizeof(buf),
             "Shaper calibration data written to /tmp/calibration_data_%c_mock.csv file",
             axis_lower);
    const std::string csv_line(buf);

    struct ShaperSimState {
        MoonrakerClientMock* mock;
        char axis_lower;
        std::vector<std::string> lines;
        std::string csv_line;
        size_t index;
    };

    auto* sim = new ShaperSimState{this, axis_lower, std::move(lines), csv_line, 0};
    const int total_steps = static_cast<int>(sim->lines.size()) + 1; // + the CSV line

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* s = static_cast<ShaperSimState*>(lv_timer_get_user_data(t));

            if (s->index < s->lines.size()) {
                s->mock->dispatch_gcode_response(s->lines[s->index]);
                s->index++;
                return;
            }

            // Write actual CSV file so frequency response chart has data.
            // When shaper_csv_writable_ is false, simulate Klipper's /tmp
            // output being unreadable (e.g. PrivateTmp) by removing any
            // stale file at the path instead of writing it.
            std::string csv_path =
                std::string("/tmp/calibration_data_") + s->axis_lower + std::string("_mock.csv");
            if (s->mock->shaper_csv_writable_) {
                write_mock_shaper_csv(csv_path, s->axis_lower);
            } else {
                std::remove(csv_path.c_str());
            }
            s->mock->dispatch_gcode_response(s->csv_line);

            spdlog::info(
                "[MoonrakerClientMock] Dispatched SHAPER_CALIBRATE response for axis {}",
                static_cast<char>(std::toupper(static_cast<unsigned char>(s->axis_lower))));
            auto& timers = s->mock->calibration_timers_;
            timers.erase(std::remove_if(timers.begin(), timers.end(),
                                        [t](const CalibrationTimer& ct) { return ct.timer == t; }),
                         timers.end());
            delete s;
            lv_timer_delete(t);
        },
        100, sim); // 100ms between lines for snappy animation

    lv_timer_set_repeat_count(timer, total_steps);
    calibration_timers_.push_back({timer, [sim] { delete sim; }});

    spdlog::info("[MoonrakerClientMock] Started SHAPER_CALIBRATE timer for axis {} ({:.0f}-{:.0f} "
                 "Hz sweep)",
                 axis, min_freq, max_freq);
}

void MoonrakerClientMock::dispatch_measure_axes_noise_response() {
    // Check if accelerometer is available
    if (!accelerometer_available_) {
        // Dispatch error response simulating missing accelerometer
        dispatch_gcode_response(
            "!! Unknown command:\"MEASURE_AXES_NOISE\". Check [adxl345] config.");
        spdlog::info(
            "[MoonrakerClientMock] Dispatched MEASURE_AXES_NOISE error (no accelerometer)");
        return;
    }

    // Dispatch realistic noise measurement response matching Klipper output format
    // Real Klipper format: "Axes noise for xy-axis accelerometer: 57.956 (x), 103.543 (y), 45.396
    // (z)"
    dispatch_gcode_response(
        "Axes noise for xy-axis accelerometer: 12.345678 (x), 15.678901 (y), 8.234567 (z)");

    spdlog::info("[MoonrakerClientMock] Dispatched MEASURE_AXES_NOISE response");
}

void MoonrakerClientMock::advance_print_start_simulation() {
    // Get current temperatures and targets
    double ext_temp = extruder_temp_.load();
    double ext_target = extruder_target_.load();
    double bed_temp = bed_temp_.load();
    double bed_target = bed_target_.load();

    // Get current simulated phase
    uint8_t current_phase = simulated_print_start_phase_.load();

    // Progress through phases based on temperature state
    // Each phase is dispatched once per print job

    // Phase 1: PRINT_START marker (immediately when print starts)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::PRINT_START_MARKER)) {
        dispatch_gcode_response(
            "PRINT_START BED_TEMP=" + std::to_string(static_cast<int>(bed_target)) +
            " EXTRUDER_TEMP=" + std::to_string(static_cast<int>(ext_target)));
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::PRINT_START_MARKER));
        return; // One phase per tick to spread out messages
    }

    // Phase 2: Homing (a few ticks after start)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HOMING)) {
        dispatch_gcode_response("G28");
        dispatch_gcode_response("Homing X Y Z");
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::HOMING));
        return;
    }

    // Phase 3: Heating bed (when bed starts warming, ~10% toward target)
    double bed_progress =
        (bed_target > ROOM_TEMP) ? (bed_temp - ROOM_TEMP) / (bed_target - ROOM_TEMP) : 1.0;
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_BED) &&
        bed_progress > 0.05) {
        dispatch_gcode_response("M190 S" + std::to_string(static_cast<int>(bed_target)));
        dispatch_gcode_response("Heating bed to " + std::to_string(static_cast<int>(bed_target)) +
                                "C");
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_BED));
        return;
    }

    // Phase 4: Heating nozzle (when extruder starts warming, ~10% toward target)
    double ext_progress =
        (ext_target > ROOM_TEMP) ? (ext_temp - ROOM_TEMP) / (ext_target - ROOM_TEMP) : 1.0;
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_NOZZLE) &&
        ext_progress > 0.05) {
        dispatch_gcode_response("M109 S" + std::to_string(static_cast<int>(ext_target)));
        dispatch_gcode_response("Heating extruder to " +
                                std::to_string(static_cast<int>(ext_target)) + "C");
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::HEATING_NOZZLE));
        return;
    }

    // Phase 5: QGL (when bed is ~50% heated - simulate while heating)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::QGL) && bed_progress > 0.4) {
        dispatch_gcode_response("QUAD_GANTRY_LEVEL");
        dispatch_gcode_response("// Gantry leveling complete");
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::QGL));
        return;
    }

    // Phase 6: Bed mesh (when bed is ~70% heated)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::BED_MESH) &&
        bed_progress > 0.65) {
        dispatch_gcode_response("BED_MESH_CALIBRATE");
        dispatch_gcode_response("// Bed mesh calibration complete");
        simulated_print_start_phase_.store(
            static_cast<uint8_t>(SimulatedPrintStartPhase::BED_MESH));
        return;
    }

    // Phase 7: Purge line (when temps are nearly ready, ~90%)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::PURGING) &&
        bed_progress > 0.85 && ext_progress > 0.85) {
        dispatch_gcode_response("VORON_PURGE");
        dispatch_gcode_response("// Purge complete");
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::PURGING));
        return;
    }

    // Phase 8: Layer 1 marker (when transitioning to PRINTING phase)
    // This is handled in the simulation loop when temps are stable
}

// ============================================================================
// Restart Simulation Helper Methods
// ============================================================================

void MoonrakerClientMock::trigger_restart(bool is_firmware) {
    // Set klippy_state to "startup"
    klippy_state_.store(KlippyState::STARTUP);

    // Clear any active print state
    if (print_phase_.load() != MockPrintPhase::IDLE) {
        print_phase_.store(MockPrintPhase::IDLE);
        print_state_.store(0); // standby
        {
            std::lock_guard<std::mutex> lock(print_mutex_);
            print_filename_.clear();
        }
        print_progress_.store(0.0);
    }

    // Set temperature targets to 0 (heaters off) - temps will naturally cool
    extruder_target_.store(0.0);
    bed_target_.store(0.0);

    // Clear excluded objects list (restart clears Klipper state)
    if (mock_state_) {
        mock_state_->clear_excluded_objects();
    }
    {
        std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
        excluded_objects_.clear();
    }

    // Reset PRINT_START simulation phase
    simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::NONE));

    // Per-tool z-offsets come back from printer.cfg, so anything SET_TOOL_PARAMETER
    // wrote but SAVE_TOOL_PARAMETER + SAVE_CONFIG never committed is lost here -
    // as on a real printer. Tools with no saved entry fall back to the distinct
    // seed in tool_z_offset().
    {
        std::lock_guard<std::mutex> lock(tool_z_offsets_mutex_);
        tool_z_offsets_ = tool_z_offsets_saved_;
    }

    // Dispatch klippy state change notification
    json status = {{"webhooks",
                    {{"state", "startup"},
                     {"state_message", is_firmware ? "Firmware restart in progress"
                                                   : "Klipper restart in progress"}}}};
    dispatch_status_update(status);

    spdlog::info("[MoonrakerClientMock] {} triggered - klippy_state='startup'",
                 is_firmware ? "FIRMWARE_RESTART" : "RESTART");

    // Schedule return to ready state using tracked thread
    // IMPORTANT: Must track and join - detached threads cause use-after-free during destruction
    double delay_sec = is_firmware ? 3.0 : 2.0;

    // Apply speedup factor to delay
    double effective_delay = delay_sec / speedup_factor_.load();

    // Cancel and wait for any existing restart thread (under lock to prevent race with destructor)
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        restart_pending_.store(false);
        if (restart_thread_.joinable()) {
            restart_thread_.join();
        }

        // Launch new restart thread (still under lock to prevent race on assignment)
        restart_pending_.store(true);
        restart_thread_ = std::thread([this, effective_delay, is_firmware]() {
            // Sleep in small increments to allow early exit on destruction
            int total_ms = static_cast<int>(effective_delay * 1000);
            int elapsed_ms = 0;
            // Use small interval (10ms) so high-speedup tests don't overshoot
            constexpr int SLEEP_INTERVAL_MS = 10;

            while (elapsed_ms < total_ms && restart_pending_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_INTERVAL_MS));
                elapsed_ms += SLEEP_INTERVAL_MS;
            }

            // Check if we were cancelled
            if (!restart_pending_.load()) {
                return;
            }

            // Return to ready state
            klippy_state_.store(KlippyState::READY);

            // Dispatch ready notification
            json ready_status = {
                {"webhooks", {{"state", "ready"}, {"state_message", "Printer is ready"}}}};
            dispatch_status_update(ready_status);

            spdlog::info("[MoonrakerClientMock] {} complete - klippy_state='ready'",
                         is_firmware ? "FIRMWARE_RESTART" : "RESTART");

            restart_pending_.store(false);
        });
    } // End of restart_mutex_ lock scope
}
