// SPDX-License-Identifier: GPL-3.0-or-later

// The mock's simulation thread and the heater state it advances. The loop ticks
// every kSimulationIntervalMs, moves temperatures and print state, and publishes
// status frames; set_*_target() and the aux, chamber and secondary extruder
// helpers are what it and the gcode handlers share.

#include "../tests/mocks/mock_printer_state.h"
#include "chamber_heater_backend.h"
#include "helix_thread.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"
#include "simulated_clock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace helix;
using mock_internal::mock_heater_duty;

void MoonrakerClientMock::append_aux_heater_status(json& status_obj, double dt) {
    constexpr double kAmbient = 25.0;
    constexpr double kRateCPerS = 1.0;
    // The parsed hardware, not the persona's heater list: HELIX_MOCK_OBJECTS
    // heaters reach only the former.
    const std::vector<std::string> heaters = discovery_.hardware().heaters();
    const std::string chamber_key = chamber_heater_status_key();
    std::lock_guard<std::mutex> lock(aux_heaters_mutex_);
    for (const auto& name : heaters) {
        if (name.rfind("heater_generic ", 0) != 0 || name == chamber_key)
            continue;
        auto& [temp, target] = aux_heaters_.try_emplace(name, kAmbient, 0.0).first->second;
        const double goal = target > 0.0 ? target : kAmbient;
        const double step = kRateCPerS * dt;
        temp = temp < goal ? std::min(goal, temp + step) : std::max(goal, temp - step);
        status_obj[name] = {
            {"temperature", temp}, {"target", target}, {"power", mock_heater_duty(temp, target)}};
    }
}

bool MoonrakerClientMock::set_aux_heater_target(const std::string& bare_name, double target) {
    const std::string name = "heater_generic " + bare_name;
    // The parsed hardware, not the persona's heater list: HELIX_MOCK_OBJECTS
    // heaters reach only the former.
    const std::vector<std::string> heaters = discovery_.hardware().heaters();
    if (name == chamber_heater_status_key() ||
        std::find(heaters.begin(), heaters.end(), name) == heaters.end())
        return false;
    std::lock_guard<std::mutex> lock(aux_heaters_mutex_);
    aux_heaters_.try_emplace(name, 25.0, 0.0).first->second.second = target;
    return true;
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
            // The pin is only a request: while the device heats it runs the
            // filter fan itself and reports why, leaving our pin untouched.
            const bool device_fan = !filter_on && chamber_target > 0.0;
            const bool mock_fault = dragonbreath_fault_.load();
            const bool mock_offline = dragonbreath_offline_.load();
            // The appliance holding its own target with neither our lease nor
            // a klipper source: the only frame shape that raises the External
            // marker (heating && !ours in the backend parse).
            const bool mock_external = dragonbreath_external_.load();
            // PTC element rides a few degrees above chamber air, drifting
            // with the same slow sine the other mock sensors use.
            const double ptc_temp =
                chamber_temp + 4.0 + 2.0 * std::sin(2.0 * M_PI * sim_time / 75.0);
            status_obj[diag] = {
                {"temperature", chamber_temp},
                {"target", chamber_target},
                {"fault", mock_fault},
                {"inhibited", false},
                {"fault_reason", mock_fault ? json("ptc_overtemp") : json(nullptr)},
                {"ptc_temp", ptc_temp},
                {"fan_percent", (filter_on || device_fan) ? 100 : 0},
                {"fan_reason", filter_on    ? "requested"
                               : device_fan ? "heater"
                                            : "off"},
                {"mode", (chamber_target > 0.0 || mock_external) ? "power_on" : "off"},
                {"source", mock_external ? "device" : "klipper"},
                {"lease_owned", !mock_external && chamber_target > 0.0},
                {"connected", !mock_offline}};
            // The rejection fields only once a target has been refused, as a
            // glue that has rejected nothing yet reports none.
            if (const int seq = dragonbreath_reject_seq_.load(); seq > 0) {
                const bool outstanding = dragonbreath_rejection_outstanding_.load();
                const std::string code = outstanding ? dragonbreath_reject_code_ : std::string();
                status_obj[diag]["protocol_error"] = code;
                status_obj[diag]["protocol_error_code"] = code;
                status_obj[diag]["protocol_error_seq"] = seq;
                status_obj[diag]["protocol_error_target"] = dragonbreath_reject_target_.load();
            }
        } else if (backend->id() == "panda_breath") {
            // VENDOR_OK: mirrors the stock Panda Breath binding's status
            // object as captured live on the U1 rig (issue #1290).
            const double chamber_temp = chamber_temp_.load();
            const double chamber_target = chamber_target_.load();
            const bool mock_offline = panda_breath_offline_.load();
            // Test hook: the appliance holding its own auto target while our
            // target reads 0 — the state the rig sits in at rest, and the
            // only one that raises the External badge.
            const bool mock_auto = panda_breath_auto_.load();
            const bool klipper_driving = chamber_target > 0.0;
            // A drying cycle counts down on the simulated clock and ends by
            // itself, the way the appliance's own timer does.
            const int dry_hours = chamber_dry_hours_.load();
            const int dry_remaining =
                chamber_drying_.load()
                    ? std::max(0, static_cast<int>(dry_hours * 3600 - (mock_sim_time_.load() -
                                                                       chamber_dry_start_.load())))
                    : 0;
            const bool drying = dry_remaining > 0;
            // work_mode latches at its last value once the output stops, so a
            // Klipper target that has been set and cleared still reads 2.
            const int work_mode = drying ? 3 : klipper_driving ? 2 : (mock_auto ? 1 : 2);
            // The device reports whole degrees.
            const double reported_temp = std::round(chamber_temp);
            status_obj[diag] = {{"temperature", reported_temp},
                                {"target", chamber_target},
                                {"smoothed_temp", reported_temp},
                                {"connected", !mock_offline},
                                {"work_mode", work_mode},
                                {"work_on", drying || klipper_driving || mock_auto},
                                {"device_target", klipper_driving ? chamber_target
                                                  : mock_auto     ? 60.0
                                                                  : 0.0},
                                {"auto_enabled", mock_auto && !klipper_driving},
                                {"auto_target", 45},
                                {"auto_filtertemp", 30},
                                {"auto_hotbedtemp", 80},
                                {"filament_temp", drying ? chamber_dry_temp_.load() : 60},
                                {"filament_timer", drying ? dry_hours : 12},
                                {"remaining_seconds", dry_remaining},
                                {"filament_drying_active", drying}};
        }
    }

    const std::string pin(backend->filter_fan_pin());
    if (!pin.empty() && present(pin)) {
        status_obj[pin] = {{"value", chamber_filter_value_.load()}};
    }
}

double MoonrakerClientMock::secondary_extruder_temp(int index) const {
    std::lock_guard<std::mutex> lock(secondary_extruders_mutex_);
    if (index < 1 || index >= static_cast<int>(secondary_extruders_.size())) {
        return kRoomTemp;
    }
    return secondary_extruders_[static_cast<size_t>(index)].temperature;
}

double MoonrakerClientMock::secondary_extruder_target(int index) const {
    std::lock_guard<std::mutex> lock(secondary_extruders_mutex_);
    if (index < 1 || index >= static_cast<int>(secondary_extruders_.size())) {
        return 0.0;
    }
    return secondary_extruders_[static_cast<size_t>(index)].target;
}

void MoonrakerClientMock::advance_secondary_extruders(double dt) {
    std::lock_guard<std::mutex> lock(secondary_extruders_mutex_);
    for (size_t i = 1; i < secondary_extruders_.size(); ++i) {
        SecondaryExtruder& e = secondary_extruders_[i];
        const double goal = e.target > 0 ? e.target : kRoomTemp;
        if (e.temperature < goal) {
            e.temperature = std::min(goal, e.temperature + kExtruderHeatRate * dt);
        } else {
            e.temperature = std::max(goal, e.temperature - kExtruderCoolRate * dt);
        }
    }
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

json MoonrakerClientMock::chamber_route_set_target(double target) {
    const std::string key = chamber_heater_status_key();
    if (!dragonbreath_reject_code_.empty()) {
        const int seq = ++dragonbreath_reject_seq_;
        dragonbreath_reject_target_.store(target);
        dragonbreath_rejection_outstanding_.store(true);
        set_chamber_target(0.0);
        spdlog::info("[MoonrakerClientMock] Chamber target {}°C refused ({}), seq {}", target,
                     dragonbreath_reject_code_, seq);
        json status = {{"dragonbreath",
                        {{"protocol_error", dragonbreath_reject_code_},
                         {"protocol_error_code", dragonbreath_reject_code_},
                         {"protocol_error_seq", seq},
                         {"protocol_error_target", target}}}};
        if (!key.empty()) {
            status[key] = {{"target", 0.0}};
        }
        dispatch_status_update(status);
        return {{"accepted", false},
                {"pending", false},
                {"reason", dragonbreath_reject_code_},
                {"seq", seq},
                {"target", target}};
    }
    set_chamber_target(target);
    reset_idle_timeout();
    const bool cleared = dragonbreath_rejection_outstanding_.exchange(false);
    json status = json::object();
    if (!key.empty()) {
        status[key] = {{"target", target}};
    }
    if (cleared) {
        status["dragonbreath"] = {{"protocol_error", ""}, {"protocol_error_code", ""}};
    }
    dispatch_status_update(status);
    return {{"accepted", true},
            {"pending", false},
            {"reason", nullptr},
            {"seq", dragonbreath_reject_seq_.load()},
            {"target", target}};
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

    simulation_thread_ =
        helix::make_thread(&MoonrakerClientMock::temperature_simulation_loop, this);
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
    const double base_dt = kSimulationIntervalMs / 1000.0; // Base time step (0.5s)

    while (simulation_running_.load()) {
        // Parked by set_simulation_paused(): no physics, no tick, no push, so a
        // value a caller wrote into a subject by hand stays written. The wait is
        // bounded like the one at the bottom of the loop, so a notify that races
        // the predicate costs one interval rather than wedging shutdown.
        if (simulation_paused_.load()) {
            std::unique_lock<std::mutex> lock(sim_mutex_);
            sim_cv_.wait_for(lock, std::chrono::milliseconds(kSimulationIntervalMs), [this] {
                return !simulation_running_.load() || !simulation_paused_.load();
            });
            continue;
        }

        uint32_t tick = tick_count_.fetch_add(1);

        // Fire any due mock pressure-advance console lines
        service_pending_pa_lines();
        service_openams_late_links(tick);

        // Simulated time step covered by one real tick
        double effective_dt = sim_speed().accelerate_progress(base_dt);
        if (is_mock_openams()) {
            service_openams_dryers(effective_dt);
        }

        // Get current temperature state
        double ext_temp = extruder_temp_.load();
        double ext_target = extruder_target_.load();
        double bed_temp_val = bed_temp_.load();
        double bed_target_val = bed_target_.load();

        // Continuous variation parameters for idle/room temp state
        // Uses sinusoidal waves with different periods to create natural-looking fluctuation
        // This ensures graphs always have data to display during testing
        constexpr double kIdleVariationAmplitude = 1.5; // +/- 1.5°C variation
        constexpr double kExtruderWavePeriod = 45.0;    // 45 second period for extruder
        constexpr double kBedWavePeriod = 60.0;         // 60 second period for bed
        constexpr double kPhaseOffset = 1.57;           // Phase offset between heaters (pi/2)

        double sim_time = tick * base_dt; // Simulated elapsed time in seconds
        // The dryer clock runs at the simulation speed, so --sim-speed plays a
        // multi-hour cycle through in minutes.
        mock_sim_time_.store(mock_sim_time_.load() + sim_speed().accelerate_progress(base_dt));

        // Simulate extruder temperature change (scaled by speedup)
        if (ext_target > 0) {
            if (ext_temp < ext_target) {
                ext_temp += kExtruderHeatRate * effective_dt;
                if (ext_temp > ext_target)
                    ext_temp = ext_target;
            } else if (ext_temp > ext_target) {
                ext_temp -= kExtruderCoolRate * effective_dt;
                if (ext_temp < ext_target)
                    ext_temp = ext_target;
            }
        } else {
            // Cool toward room temp, then add continuous variation
            if (ext_temp > kRoomTemp + kIdleVariationAmplitude) {
                ext_temp -= kExtruderCoolRate * effective_dt;
            } else {
                // At room temp: apply sinusoidal variation for continuous graph updates
                double wave = std::sin(2.0 * M_PI * sim_time / kExtruderWavePeriod);
                ext_temp = kRoomTemp + kIdleVariationAmplitude * wave;
            }
        }
        extruder_temp_.store(ext_temp);
        advance_secondary_extruders(effective_dt);

        // Simulate bed temperature change (scaled by speedup)
        if (bed_target_val > 0) {
            if (bed_temp_val < bed_target_val) {
                bed_temp_val += kBedHeatRate * effective_dt;
                if (bed_temp_val > bed_target_val)
                    bed_temp_val = bed_target_val;
            } else if (bed_temp_val > bed_target_val) {
                bed_temp_val -= kBedCoolRate * effective_dt;
                if (bed_temp_val < bed_target_val)
                    bed_temp_val = bed_target_val;
            }
        } else {
            // Cool toward room temp, then add continuous variation
            if (bed_temp_val > kRoomTemp + kIdleVariationAmplitude) {
                bed_temp_val -= kBedCoolRate * effective_dt;
            } else {
                // At room temp: apply sinusoidal variation (phase offset from extruder)
                double wave = std::sin(2.0 * M_PI * sim_time / kBedWavePeriod + kPhaseOffset);
                bed_temp_val = kRoomTemp + kIdleVariationAmplitude * wave;
            }
        }
        bed_temp_.store(bed_temp_val);

        // Simulate chamber temperature change (scaled by speedup)
        // Chamber responds to target temperature like bed/extruder, but slower
        if (simulates_chamber_temp()) {
            constexpr double kChamberIdleVariationAmplitude = 1.5;
            constexpr double kChamberWavePeriod = 90.0; // 90 second period for idle variation

            double chamber = chamber_temp_.load();
            double chamber_target = chamber_target_.load();

            if (chamber_target > 0) {
                // Heating/cooling toward target
                if (chamber < chamber_target) {
                    chamber += kChamberHeatRate * effective_dt;
                    if (chamber > chamber_target)
                        chamber = chamber_target;
                } else if (chamber > chamber_target) {
                    chamber -= kChamberCoolRate * effective_dt;
                    if (chamber < chamber_target)
                        chamber = chamber_target;
                }
            } else {
                // Cool toward room temp, then add continuous variation
                if (chamber > kRoomTemp + kChamberIdleVariationAmplitude) {
                    chamber -= kChamberCoolRate * effective_dt;
                } else {
                    // At room temp: apply sinusoidal variation
                    double wave = std::sin(2.0 * M_PI * sim_time / kChamberWavePeriod + 2.0);
                    chamber = kRoomTemp + kChamberIdleVariationAmplitude * wave;
                }
            }
            chamber_temp_.store(chamber);
        }

        // Simulate MCU temperature (stable 40-55°C, slight load correlation)
        {
            constexpr double kMcuBase = 42.0;
            constexpr double kMcuWavePeriod = 120.0;
            constexpr double kMcuAmplitude = 3.0;
            constexpr double kMcuPrintOffset = 5.0; // Higher during printing

            double mcu = kMcuBase;
            MockPrintPhase current_phase = print_phase_.load();
            if (current_phase == MockPrintPhase::PRINTING ||
                current_phase == MockPrintPhase::PREHEAT) {
                mcu += kMcuPrintOffset;
            }
            double wave = std::sin(2.0 * M_PI * sim_time / kMcuWavePeriod);
            mcu += kMcuAmplitude * wave;
            mcu_temp_.store(mcu);
        }

        // Simulate host/RPi temperature (45-65°C, higher under load)
        {
            constexpr double kHostBase = 52.0;
            constexpr double kHostWavePeriod = 75.0;
            constexpr double kHostAmplitude = 4.0;
            constexpr double kHostPrintOffset = 8.0;

            double host = kHostBase;
            MockPrintPhase current_phase = print_phase_.load();
            if (current_phase == MockPrintPhase::PRINTING ||
                current_phase == MockPrintPhase::PREHEAT) {
                host += kHostPrintOffset;
            }
            double wave = std::sin(2.0 * M_PI * sim_time / kHostWavePeriod + 1.0);
            host += kHostAmplitude * wave;
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

        const std::string homed = get_homed_axes();

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
        // Only dispatch notifications every kNotificationIntervalTicks to reduce log spam
        // Physics still runs every tick for smooth temperature changes
        if (tick % kNotificationIntervalTicks != 0) {
            // Sleep and continue without dispatching
            std::unique_lock<std::mutex> lock(sim_mutex_);
            sim_cv_.wait_for(lock, std::chrono::milliseconds(kSimulationIntervalMs),
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

        // A console line per layer while printing, so the console has live
        // output to show in --test.
        if (print_state_str == "printing") {
            layer_pause_on_layer(layer_pause_last_layer_, current_layer);
        }
        layer_pause_last_layer_ = current_layer;
        if (print_state_str == "printing" && current_layer > 0 &&
            current_layer != last_console_layer_) {
            last_console_layer_ = current_layer;
            dispatch_gcode_response("// Layer " + std::to_string(current_layer) + "/" +
                                    std::to_string(total_layers));
        }

        // Get Z offset for gcode_move
        double z_offset = gcode_offset_z_.load();

        // Commanded feed rate swings between perimeter and infill speeds while
        // printing. The extruder feeds 0.2 x 0.45 mm lines of 1.75 mm filament
        // at the overridden speed and flow, as motion_report would measure it.
        const bool extruding = print_state_str == "printing";
        const double feed_mm_s = extruding ? 105.0 + 45.0 * std::sin(elapsed / 7.0) : 0.0;
        const double extruder_mm_s =
            feed_mm_s * (speed / 100.0) * (0.2 * 0.45 / 2.405) * (flow / 100.0);

        // Build notification JSON (enhanced Moonraker format with layer info)
        json status_obj = {
            {"extruder",
             {{"temperature", ext_temp},
              {"target", ext_target},
              {"power", mock_heater_duty(ext_temp, ext_target)}}},
            {"heater_bed",
             {{"temperature", bed_temp_val},
              {"target", bed_target_val},
              {"power", mock_heater_duty(bed_temp_val, bed_target_val)}}},
            {"toolhead",
             {{"position", {x, y, z, 0.0}},
              {"homed_axes", homed},
              {"axis_minimum",
               mock_internal::toolhead_axis_minimum(helix::mock::descriptor(printer_type_))},
              {"axis_maximum",
               mock_internal::toolhead_axis_maximum(helix::mock::descriptor(printer_type_))},
              {"kinematics", kinematics()}}},
            {"gcode_move",
             {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
              {"speed", feed_mm_s},
              {"speed_factor", speed / 100.0},
              {"extrude_factor", flow / 100.0},
              {"homing_origin", {gcode_offset_x_.load(), gcode_offset_y_.load(), z_offset, 0.0}}}},
            {"motion_report",
             {{"live_position", {x, y, z + z_offset, 0.0}},
              {"live_velocity", feed_mm_s * (speed / 100.0)},
              {"live_extruder_velocity", extruder_mm_s}}},
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
            {"display_status", {{"progress", progress}, {"message", current_display_message()}}},
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

        // Add width sensor data (Hall-effect filament diameter measurement) on
        // every frame of a persona that has one, so WidthSensorManager receives updates
        if (helix::mock::inherits_default(printer_type_,
                                          helix::mock::default_object::kWidthSensor)) {
            status_obj["hall_filament_width_sensor"] = {
                {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
        }

        // Toolchanger mock mode: the extra extruders on the live subscription
        // stream, each following its own target (secondary_extruder_temp()).
        if (is_mock_toolchanger() || is_mock_medusahc()) {
            const int count = is_mock_medusahc() ? medusa_tool_count() : 4;
            for (int t = 1; t < count; ++t) {
                status_obj["extruder" + std::to_string(t)] = {
                    {"temperature", secondary_extruder_temp(t)},
                    {"target", secondary_extruder_target(t)}};
            }
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
            if (mock_medusa_variant() == MedusaVariant::CONTROLLER_KTC) {
                status_obj["toolchanger"] = {{"tool_number", sync_medusa_toolchanger_tool()}};
            }
            const auto medusa_macros = medusa_macro_status_json();
            for (const auto& [name, value] : medusa_macros.items()) {
                status_obj[name] = value;
            }
        }

        // Standalone IFS module mock: the module's pushed objects. The lane
        // sensors and toolhead switch ride the same notification so the
        // backend's head-presence path sees what the board would report.
        if (is_mock_ifs_module()) {
            status_obj["ifs"] = ifs_module_status_json();
            status_obj["ifs_materials"] = ifs_module_materials_json();
            status_obj["save_variables"] = ifs_module_vars_json();
            const uint8_t presence = ifs_module_presence_.load();
            for (int i = 0; i < 4; ++i) {
                status_obj["filament_switch_sensor lane" + std::to_string(i + 1)] = {
                    {"filament_detected", (presence & (1u << i)) != 0}};
            }
            status_obj["filament_switch_sensor toolhead"] = {
                {"filament_detected", ifs_module_head_loaded_.load()}};
        }

        // CFS mock: the stock `box` frame AmsBackendCfs parses (bay states,
        // map, and the vendor/color/material arrays).
        if (is_mock_cfs()) {
            status_obj["box"] = cfs_box_status_json();
        }
        // multiACE mock: the engine-shaped `ace` frame the production
        // AmsBackendMultiAce parses. The filament_feed frames ride the initial
        // status and each ACE_* command's publish, not this loop.
        if (is_mock_multiace()) {
            status_obj["ace"] = multiace_status_json();
        }
        if (printer_type_ == PrinterType::CREALITY_K2_PLUS) {
            append_k2_status(status_obj);
        }
        if (is_mock_openams()) {
            status_obj["oams_manager"] = openams_status_json();
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
                    // A chamber thermostat parks at its configured resting
                    // target whenever no M141 set is live.
                    const double set = chamber_target_.load();
                    target = set > 0.0 ? set : mock_internal::kMockK2ChamberFanRestingC;
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
            constexpr double kHumidityWavePeriod = 180.0; // 3 minute period
            double humidity_wave = std::sin(2.0 * M_PI * sim_time / kHumidityWavePeriod);
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

        // The chamber heater object itself. The chamber reading is taken from
        // this object whenever a heater exists, so without it here the chamber
        // freezes at whatever the first frame carried. A temperature_fan
        // chamber is emitted by the sensor loop above with its speed, and
        // re-emitting it as a heater would drop that.
        if (const std::string chamber_key = chamber_heater_status_key();
            chamber_key.rfind("heater_generic ", 0) == 0) {
            const double chamber_now = chamber_temp_.load();
            const double chamber_tgt = chamber_target_.load();
            status_obj[chamber_key] = {{"temperature", chamber_now},
                                       {"target", chamber_tgt},
                                       {"power", mock_heater_duty(chamber_now, chamber_tgt)}};
        }

        // Chamber backend diagnostics + filter pin (e.g. dragonbreath trio via
        // HELIX_MOCK_OBJECTS) — drifts with the chamber sim like the sensors above.
        append_chamber_backend_status(status_obj, sim_time);
        append_aux_heater_status(status_obj, effective_dt);

        json notification = {{"method", "notify_status_update"},
                             {"params", json::array({status_obj, tick * base_dt})}};

        // Push notification through all registered callbacks
        // Two-phase: copy under lock, invoke outside to avoid deadlock
        std::vector<helix::SharedCallback<void(const json&)>> callbacks_copy;
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
        // The live WebSocket path delivers notify_status_update to method-callback
        // registrants too (plugin subscriptions among them), so the simulated frames
        // reach the same listeners the real ones would.
        dispatch_method_callback("notify_status_update", notification);

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
            sim_cv_.wait_for(lock, std::chrono::milliseconds(kSimulationIntervalMs),
                             [this] { return !simulation_running_.load(); });
        }
    }
    spdlog::debug("[MoonrakerClientMock] temperature_simulation_loop EXITED");
}
