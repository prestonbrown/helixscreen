// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mock_scenarios.h"

// The scenario machinery drives the mock AMS backend, which is compiled out
// of the size-constrained device builds (ENABLE_MOCKS=no); those builds get
// an empty registry and find_scenario() always misses.
#ifdef HELIX_ENABLE_MOCKS

#include "ams_backend_mock.h"
#include "ams_state.h"
#include "http_executor.h"
#include "thumbnail_processor.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <lvgl.h>
#include <thread>

// LVGL XML subject lookup
#include "helix-xml/src/xml/lv_xml.h"

namespace helix {

// Helper: set an INT subject by name. Logs warning if not found.
static void set_int(const char* name, int value) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    if (s) {
        lv_subject_set_int(s, value);
    } else {
        spdlog::warn("[Scenario] Subject not found: {}", name);
    }
}

// Helper: set a STRING subject by name. Logs warning if not found.
static void set_string(const char* name, const char* value) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    if (s) {
        lv_subject_copy_string(s, value);
    } else {
        spdlog::warn("[Scenario] Subject not found: {}", name);
    }
}

/// Apply a clog/flow state to the mock AMS backend and push it through the
/// real derivation, so the meter shows what a printer reporting this would.
///
/// `as_mock()` is AmsBackend's RTTI-free stand-in for a dynamic_cast; on a real
/// backend it returns nullptr and the scenario is a no-op, which is correct —
/// there is nothing to fake on hardware that is telling us the truth.
static void apply_clog_state(const std::function<void(AmsBackendMock&)>& mutate) {
    auto* backend = AmsState::instance().get_backend();
    auto* mock = backend ? backend->as_mock() : nullptr;
    if (!mock) {
        spdlog::warn("[Scenario] No mock AMS backend — clog scenario skipped");
        return;
    }
    mutate(*mock);
    // The subjects are derived, not stored: re-read the backend so the whole
    // chain (source precedence, thresholds, label text) runs exactly as it does
    // when a printer pushes an update.
    AmsState::instance().sync_from_backend();
    // What a backend event does after its sync, so surfaces that re-read the
    // backend on a revision tick (the Buffer Status modal) see it too.
    AmsState::instance().bump_data_revision();
}

/// Encoder state at a given headroom, in the shape Happy Hare reports it.
///
/// `detection_length` is the window the encoder measures over and `headroom` is
/// what is left of it, so clog% is (length - headroom) / length. `min_headroom`
/// is the worst point reached this print, which is both the peak marker and,
/// when it drops under `desired_headroom`, the warning latch.
static EncoderClogInfo encoder_at(float headroom, float min_headroom) {
    EncoderClogInfo info;
    info.enabled = true;
    info.detection_mode = 2; // auto
    info.detection_length = 12.4f;
    info.desired_headroom = 5.0f;
    info.headroom = headroom;
    info.min_headroom = min_headroom;
    // Flow falls off as the path blocks; not derived by Happy Hare, but the
    // two move together on a real machine and a fixed 85% next to a red bar
    // reads as a bug.
    info.flow_rate = static_cast<int>(std::clamp(headroom / 12.4f, 0.0f, 1.0f) * 100.0f);
    return info;
}

/// Clear every clog source, so a scenario can select exactly one.
static void clear_clog_sources(AmsBackendMock& mock) {
    mock.set_encoder_clog_info(EncoderClogInfo{}, /*detection_mode_flag=*/0);
    mock.set_flowguard_info(FlowguardInfo{});
    mock.set_sync_feedback_bias(-2.0f);
    for (int u = 0; u < 4; ++u) {
        mock.set_unit_buffer_health(u, std::nullopt);
    }
}

/// A filament pressure sensor on unit 0 reading `pressure` against
/// `set_point` (-1: none published), and nothing else measuring. Read as such
/// by every simulated type but Happy Hare, whose buffer is system-level.
static void set_fps(AmsBackendMock& mock, float pressure, float set_point = 0.5f) {
    clear_clog_sources(mock);
    BufferHealth h;
    h.fps_value = h.smoothed_fps = pressure;
    h.fps_set_point = set_point;
    h.fps_reported = true;
    mock.set_unit_buffer_health(0, h);
}

static std::vector<MockScenario> clog_scenarios() {
    std::vector<MockScenario> s;

    s.push_back({"clog_healthy", "Encoder clog detection, full headroom", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         m.set_encoder_clog_info(encoder_at(11.8f, 11.2f), 2);
                     });
                 }});

    s.push_back({"clog_warning", "Encoder headroom dipped below the desired minimum", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         // min_headroom under desired_headroom is what latches
                         // EncoderClogInfo::is_warning().
                         m.set_encoder_clog_info(encoder_at(7.5f, 4.2f), 2);
                     });
                 }});

    s.push_back({"clog_blocked", "Encoder headroom nearly gone — clog imminent", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         m.set_encoder_clog_info(encoder_at(0.8f, 0.6f), 2);
                     });
                 }});

    s.push_back({"flowguard_neutral", "Flowguard armed and centred", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         FlowguardInfo fg;
                         fg.enabled = true;
                         fg.active = true;
                         fg.level = 0.02f;
                         fg.max_clog = 0.18f;
                         fg.max_tangle = -0.12f;
                         m.set_flowguard_info(fg);
                     });
                 }});

    s.push_back({"flowguard_tangle", "Flowguard leaning to the tangle end", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         FlowguardInfo fg;
                         fg.enabled = true;
                         fg.active = true;
                         fg.level = -0.55f; // over-feeding
                         fg.max_clog = 0.20f;
                         fg.max_tangle = -0.62f;
                         m.set_flowguard_info(fg);
                     });
                 }});

    s.push_back({"flowguard_clog", "Flowguard tripped at the clog end", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         FlowguardInfo fg;
                         fg.enabled = true;
                         fg.active = true;
                         fg.level = 0.82f;
                         // A non-empty trigger is what sets warning=1, which is
                         // what turns the indicator red whatever the mode.
                         fg.trigger = "CLOG";
                         fg.max_clog = 0.86f;
                         fg.max_tangle = -0.10f;
                         m.set_flowguard_info(fg);
                     });
                 }});

    s.push_back({"buffer_safe", "AFC buffer fault detection armed, nothing to report", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         BufferHealth h;
                         h.fault_detection_enabled = true;
                         h.error_sensitivity = 7.0f;
                         h.state = "Neutral";
                         // Negative means the fault timer is stopped, which
                         // AmsState reads as "nothing to report" (value 0).
                         h.distance_to_fault = -1.0f;
                         m.set_unit_buffer_health(0, h);
                     });
                 }});

    s.push_back({"buffer_fault", "AFC buffer counting down to a fault", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         BufferHealth h;
                         h.fault_detection_enabled = true;
                         h.error_sensitivity = 7.0f;
                         h.state = "Trailing";
                         h.distance_to_fault = 1.5f; // close to the threshold
                         m.set_unit_buffer_health(0, h);
                     });
                 }});

    s.push_back({"buffer_fps", "Filament pressure sensor below its set point (pulling tight)",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.32f); }); }});

    s.push_back({"buffer_fps_loose", "Filament pressure sensor above its set point (loose)",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.71f); }); }});

    s.push_back({"buffer_fps_on_target", "Filament pressure sensor on its set point",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.52f); }); }});

    s.push_back({"buffer_fps_danger", "Filament pressure sensor pinned near the tight end",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.08f); }); }});

    s.push_back({"buffer_fps_no_target", "Filament pressure sensor with no set point",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.32f, -1.0f); }); }});

    s.push_back({"buffer_fps_with_clog",
                 "Filament pressure sensor and AFC fault detection both reporting", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         set_fps(m, 0.32f);
                         BufferHealth h;
                         h.fps_value = h.smoothed_fps = 0.32f;
                         h.fps_set_point = 0.5f;
                         h.fps_reported = true;
                         h.fault_detection_enabled = true;
                         h.error_sensitivity = 7.0f;
                         h.state = "Trailing";
                         h.distance_to_fault = 1.5f;
                         m.set_unit_buffer_health(0, h);
                     });
                 }});

    s.push_back({"sync_feedback_tight", "Happy Hare sync feedback leaning to tension", []() {
                     apply_clog_state([](AmsBackendMock& m) {
                         clear_clog_sources(m);
                         m.set_sync_feedback_bias(-0.45f);
                     });
                 }});

    s.push_back({"clog_off", "No clog detection hardware — the meter hides itself",
                 []() { apply_clog_state([](AmsBackendMock& m) { clear_clog_sources(m); }); }});

    return s;
}

static std::vector<MockScenario> build_scenarios() {
    std::vector<MockScenario> scenarios;

    // --- idle ---
    scenarios.push_back({"idle", "Default idle state — connected, no print", []() {
                             set_int("printer_connection_state", 2); // connected
                             set_int("klippy_state", 0);             // ready
                             set_int("print_state_enum", 0);         // standby
                             set_string("print_state", "standby");
                             set_int("print_progress", 0);
                             set_int("print_active", 0);
                             set_int("extruder_temp", 250); // 25.0°C (ambient)
                             set_int("extruder_target", 0);
                             set_int("bed_temp", 230); // 23.0°C (ambient)
                             set_int("bed_target", 0);
                             set_int("fan_speed", 0);
                             set_int("print_start_phase", 0); // IDLE
                         }});

    // --- printing ---
    scenarios.push_back({"printing", "Mid-print with temps, progress, filename", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 1); // printing
                             set_string("print_state", "printing");
                             set_int("print_active", 1);
                             set_int("print_progress", 42);
                             set_string("print_filename", "benchy.gcode");
                             set_string("print_display_filename", "benchy.gcode");
                             set_int("print_layer_current", 84);
                             set_int("print_layer_total", 200);
                             set_int("print_elapsed", 3600);   // 1 hour
                             set_int("print_time_left", 4800); // 80 min left
                             set_int("print_duration", 3500);
                             set_int("extruder_temp", 2100); // 210.0°C
                             set_int("extruder_target", 2100);
                             set_int("bed_temp", 600); // 60.0°C
                             set_int("bed_target", 600);
                             set_int("fan_speed", 255);
                             set_int("print_start_phase", 10); // COMPLETE (past start)
                             set_int("print_show_progress", 1);
                         }});

    // --- paused ---
    scenarios.push_back({"paused", "Paused mid-print", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 2); // paused
                             set_string("print_state", "paused");
                             set_int("print_active", 1);
                             set_int("print_progress", 42);
                             set_string("print_filename", "benchy.gcode");
                             set_string("print_display_filename", "benchy.gcode");
                             set_int("print_layer_current", 84);
                             set_int("print_layer_total", 200);
                             set_int("extruder_temp", 2100);
                             set_int("extruder_target", 2100);
                             set_int("bed_temp", 600);
                             set_int("bed_target", 600);
                             set_int("fan_speed", 0);
                             set_int("print_start_phase", 10);
                         }});

    // --- error ---
    scenarios.push_back({"error", "Klippy error state", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 3);     // error
                             set_int("print_state_enum", 5); // error
                             set_string("print_state", "error");
                             set_int("print_active", 0);
                             set_int("print_progress", 0);
                             set_int("extruder_temp", 0);
                             set_int("extruder_target", 0);
                             set_int("bed_temp", 0);
                             set_int("bed_target", 0);
                         }});

    // --- disconnected ---
    scenarios.push_back({"disconnected", "No printer connection", []() {
                             set_int("printer_connection_state", 0); // disconnected
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 0);
                             set_string("print_state", "standby");
                             set_int("print_active", 0);
                             set_int("nav_buttons_enabled", 0);
                         }});

    // --- heating ---
    scenarios.push_back({"heating", "Print start — heating nozzle phase", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 1); // printing
                             set_string("print_state", "printing");
                             set_int("print_active", 1);
                             set_int("print_progress", 0);
                             set_int("print_show_progress", 0);
                             set_int("print_start_phase", 4); // HEATING_NOZZLE
                             set_int("print_start_progress", 65);
                             set_string("print_start_message", "Heating Nozzle...");
                             set_string("print_start_time_left", "~2 min left");
                             set_int("extruder_temp", 1650); // 165.0°C (rising)
                             set_int("extruder_target", 2100);
                             set_int("bed_temp", 600);
                             set_int("bed_target", 600);
                             set_string("print_filename", "benchy.gcode");
                             set_string("print_display_filename", "benchy.gcode");
                         }});

    // --- ams_hh_4gate ---
    scenarios.push_back({"ams_hh_4gate", "Happy Hare 4-gate setup with filament loaded", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 0);
                             set_int("ams_filament_loaded", 1);
                             set_string("ams_system_name", "Happy Hare");
                             set_string("ams_action_detail", "Idle");
                             set_string("ams_current_material_text", "PLA");
                             set_string("ams_current_slot_text", "Gate 0");
                             set_int("ams_supports_bypass", 1);
                             set_int("ams_bypass_active", 0);
                         }});

    // --- ams_afc_8lane ---
    scenarios.push_back({"ams_afc_8lane", "AFC 8-lane with mixed sensor states", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("print_state_enum", 0);
                             set_int("ams_filament_loaded", 1);
                             set_string("ams_system_name", "AFC");
                             set_string("ams_action_detail", "Idle");
                             set_string("ams_current_material_text", "PETG");
                             set_string("ams_current_slot_text", "Lane 2");
                             set_int("ams_supports_bypass", 0);
                         }});

    // --- ams_loading ---
    scenarios.push_back({"ams_loading", "AMS mid-load operation", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_int("ams_filament_loaded", 0);
                             set_string("ams_system_name", "AFC");
                             set_string("ams_action_detail", "Loading Lane 3...");
                             set_string("ams_current_material_text", "ABS");
                             set_string("ams_current_slot_text", "Lane 3");
                         }});

    // --- ams_error ---
    scenarios.push_back({"ams_error", "AMS filament jam error", []() {
                             set_int("printer_connection_state", 2);
                             set_int("klippy_state", 0);
                             set_string("ams_system_name", "Happy Hare");
                             set_string("ams_action_detail",
                                        "ERROR: Filament jam detected on Gate 1");
                             set_int("ams_filament_loaded", 0);
                         }});

    // --- http_busy ---
    // Not a printer state — a synthetic HTTP-lane busy condition for testing
    // wait_idle()'s "which counter was busy" reporting deterministically.
    // A burst of subject changes makes UpdateQueue's `pending_` nonzero for
    // at most one drain period (LV_DEF_REFR_PERIOD, 33 ms): process_pending()
    // drains the entire batch each time, so there's no way to widen that window
    // from the subject side. HttpExecutor::inflight() instead stays incremented for
    // the full wall-clock duration of the submitted job body (see
    // http_executor.h's `submit()` — incremented there, decremented only on
    // completion), so a deliberately slow job gives wait_idle(timeout=0.0) a
    // multi-hundred-millisecond window to land in instead of a sub-tick race.
    // 2s comfortably exceeds subprocess-spawn latency even under heavy load
    // (confirmed via this test's own investigation: ~100-200ms round trips
    // at load average 60+, nowhere near 2s).
    scenarios.push_back({"http_busy",
                         "Synthetic HttpExecutor busy state (test-only, not a printer condition)",
                         []() {
                             helix::http::HttpExecutor::fast().submit([]() {
                                 std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                             });
                         }});

    // --- thumbnail_busy ---
    // Same purpose as http_busy above, for ThumbnailProcessor's worker pool
    // instead of HttpExecutor's. submit_test_task() bypasses PNG decoding
    // entirely, so the 2s window is exact rather than however long a real
    // thumbnail happens to take on this machine.
    scenarios.push_back(
        {"thumbnail_busy",
         "Synthetic ThumbnailProcessor busy state (test-only, not a printer condition)", []() {
             helix::ThumbnailProcessor::instance().submit_test_task(
                 []() { std::this_thread::sleep_for(std::chrono::milliseconds(2000)); });
         }});

    // --- clog / flow detection -------------------------------------------
    //
    // These drive the MOCK BACKEND, not the clog_meter_* subjects. Those
    // subjects are re-derived from the backend on every AMS refresh
    // (AmsState::sync_from_backend -> sync_clog_meter_from_info), so a scenario
    // that set them directly would be overwritten within a poll or two - which
    // is why driving the meter by hand needs `ctl freeze` first.
    //
    // Source precedence in sync_clog_meter_from_info() is flowguard > encoder >
    // AFC buffer, so each scenario disables the sources above the one it wants.
    for (const auto& c : clog_scenarios()) {
        scenarios.push_back(c);
    }

    return scenarios;
}

const std::vector<MockScenario>& get_mock_scenarios() {
    static auto scenarios = build_scenarios();
    return scenarios;
}

const MockScenario* find_scenario(const std::string& name) {
    for (const auto& s : get_mock_scenarios()) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

} // namespace helix

#else // !HELIX_ENABLE_MOCKS

namespace helix {

const std::vector<MockScenario>& get_mock_scenarios() {
    static const std::vector<MockScenario> kEmpty;
    return kEmpty;
}

const MockScenario* find_scenario(const std::string&) {
    return nullptr;
}

} // namespace helix

#endif // HELIX_ENABLE_MOCKS
