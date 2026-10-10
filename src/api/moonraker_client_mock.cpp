// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock.h"

#include "ui_filename_utils.h"
#include "ui_update_queue.h"

#include "../tests/mocks/mock_printer_state.h"
#include "app_globals.h"
#include "chamber_heater_backend.h"
#include "env_knobs.h"
#include "gcode_parser.h"
#include "helix_thread.h"
#include "macro_param_cache.h"
#include "mock_persona.h"
#include "mock_planted_gcodes.h"
#include "moonraker_client_mock_internal.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "shaper_response.h"
#include "simulated_clock.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <unistd.h>
#include <unordered_set>

using namespace helix;

// Generate mock geometry for exclude_object status updates.
// Spreads objects in a grid inset from the edges of the persona's bed, matching
// the `center` + `polygon` shape Moonraker reports for EXCLUDE_OBJECT_DEFINE.
static json mock_object_entry(const std::string& name, int index, int total,
                              const helix::mock::AxisMax& bed) {
    constexpr float INSET = 20.0f; // keep the plate margin visible in the map
    const float bed_w = static_cast<float>(bed.x - mock_internal::MOCK_BED_X_MIN) - 2.0f * INSET;
    const float bed_h = static_cast<float>(bed.y - mock_internal::MOCK_BED_Y_MIN) - 2.0f * INSET;
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

// webhooks.state_message as Klipper reports it after M112 (klippy.py's
// message_shutdown appended to the reason).
constexpr const char* kM112ShutdownMessage =
    "Shutdown due to M112 command\n"
    "Once the underlying issue is corrected, use the\n"
    "\"FIRMWARE_RESTART\" command to reset the firmware, reload the\n"
    "config, and restart the host software.\n"
    "Printer is shutdown\n";

} // namespace

// Delegating constructor - uses default speedup of 1.0
std::vector<WebcamInfo> MoonrakerClientMock::parse_mock_webcams(const char* spec) {
    std::vector<WebcamInfo> cams;
    if (!spec || !*spec)
        return cams;
    std::string rest = spec;
    size_t index = 0;
    while (!rest.empty()) {
        size_t comma = rest.find(',');
        std::string item = rest.substr(0, comma);
        rest = comma == std::string::npos ? "" : rest.substr(comma + 1);
        if (item.empty())
            continue;
        WebcamInfo cam;
        size_t colon = item.find(':');
        cam.name = item.substr(0, colon);
        cam.service = colon == std::string::npos ? "mjpegstreamer" : item.substr(colon + 1);
        std::string path = index == 0 ? "/webcam/" : "/webcam" + std::to_string(index + 1) + "/";
        cam.stream_url = path + "?action=stream";
        cam.snapshot_url = path + "?action=snapshot";
        cams.push_back(std::move(cam));
        ++index;
    }
    return cams;
}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type) : MoonrakerClientMock(type, 1.0) {}

MoonrakerClientMock::MoonrakerClientMock(PrinterType type, double speedup_factor)
    : printer_type_(type) {
    if (const char* kin_env = std::getenv("HELIX_MOCK_KINEMATICS"); kin_env && kin_env[0]) {
        kinematics_override_ = kin_env;
    }
    // Creality firmware declares the bed in a macro's variables; detection reads
    // it to tell the K2 Plus from the K2 Pro, whose stepper travel overshoots alike.
    if (type == PrinterType::CREALITY_K2_PLUS) {
        set_config_settings_section("gcode_macro product_param", {{"variable_bed_size_x", "350"},
                                                                  {"variable_bed_size_y", "350"}});
    }
    dragonbreath_fault_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_FAULT");
    dragonbreath_offline_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_OFFLINE");
    dragonbreath_external_ = helix::env_flag("HELIX_MOCK_DRAGONBREATH_EXTERNAL");
    panda_breath_offline_ = helix::env_flag("HELIX_MOCK_PANDA_BREATH_OFFLINE");
    panda_breath_auto_ = helix::env_flag("HELIX_MOCK_PANDA_BREATH_AUTO");

    // Initialize idle timeout tracking
    last_activity_time_ = std::chrono::steady_clock::now();

    // Set speedup factor (clamped)
    speedup_factor_.store(helix::sim::clamp_speed(speedup_factor));

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

    // MedusaHC drives the real AmsBackendToolChanger, and klipper-toolchanger
    // reports no material, colour, brand or weight - the override store is the
    // whole of filament identity there. Without these records every lane renders
    // at AMS_DEFAULT_SLOT_COLOR, which makes colour and ghost bugs invisible.
    // Outer key style is T<n> (lane_key_style_for), inner "lane" is 0-based.
    if (mock_medusa_variant() != MedusaVariant::NONE) {
        // Each lane mirrors a spool from the mock Spoolman inventory - id,
        // vendor, material, colour and weights - so the active-spool card and
        // the lane it names cannot describe different filament.
        struct Lane {
            int spoolman_id;
            const char* material;
            const char* brand;
            const char* spool_name;
            const char* color;
            double remaining_g;
        };
        static constexpr Lane kLanes[] = {
            {2, "Silk PLA", "eSUN", "Silk Blue", "#26DCD9", 750.0},
            {4, "ABS", "Flashforge", "Fire Engine Red", "#D20000", 100.0},
            {13, "PETG", "Bambu Lab", "Translucent Green PETG", "#29A261", 1000.0},
            {5, "PETG", "Kingroon", "Signal Yellow", "#F4E111", 1000.0},
        };
        for (int i = 0; i < static_cast<int>(std::size(kLanes)); ++i) {
            const Lane& l = kLanes[i];
            mock_db_set("lane_data",
                        "T" + std::to_string(i), // DISPLAY_NUMBERING_OK: database key mirrors
                                                 // the T<n> wire format, not a display label
                        json{{"lane", std::to_string(i)},
                             {"spoolman_id", l.spoolman_id},
                             {"material", l.material},
                             {"brand", l.brand},
                             {"spool_name", l.spool_name},
                             {"color", l.color},
                             {"remaining_weight_g", l.remaining_g},
                             {"total_weight_g", 1000.0}});
        }
    }

    // OpenAMS reads no identity off its spools: color and material come from the
    // override store, which reads the shared lane_data namespace (laneN keys,
    // N = 1-based global slot; inner "lane" is 0-based).
    if (is_mock_openams() && openams_plugin_units() && !openams_fleet_lane_units()) {
        openams_loaded_slot_ = 0;
        openams_fault_active_ = std::getenv("HELIX_MOCK_OPENAMS_FAULT") != nullptr;
        // Every spool is known to the override store as it would be on a real
        // printer; the shared-lane shape seeds the first three.
        static const char* const COLORS[] = {"#E8E8E8", "#3CE05A", "#303030"};
        static const char* const NAMES[] = {"White", "Green", "Dark Gray"};
        for (int i = 0; i < 3; ++i) {
            mock_db_set("lane_data", "lane" + std::to_string(i + 1),
                        json{{"lane", std::to_string(i)},
                             {"color", COLORS[i]},
                             {"color_name", NAMES[i]},
                             {"material", "ASA"},
                             {"helix_material", "ASA"},
                             {"helix_locked_color", true},
                             {"helix_locked_material", true}});
        }
    } else if (is_mock_openams() && openams_fleet_lane_units()) {
        // One bay loaded, on the third unit (slot 5); its spool is known to the store.
        openams_loaded_slot_ = 5;
        openams_fault_active_ = std::getenv("HELIX_MOCK_OPENAMS_FAULT") != nullptr;
        for (int i = 0; i < kOpenAmsMaxUnits; ++i) {
            openams_dryers_[i].chamber_c = openams_ambient_c(i);
        }
        // The sixth unit is mid-cycle: 55 C, 3 h 25 min left.
        openams_dryers_[5].target_c = 55.0;
        openams_dryers_[5].remaining_s = 12300.0;
        openams_dryers_[5].chamber_c = 48.0;
        mock_db_set("lane_data", "lane6",
                    json{{"lane", "5"},
                         {"color", "#3CE05A"},
                         {"color_name", "Green"},
                         {"material", "PETG"},
                         {"helix_material", "PETG"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
    } else if (is_mock_openams()) {
        mock_db_set("lane_data", "lane3",
                    json{{"lane", "2"},
                         {"color", "#3CE05A"},
                         {"color_name", "Green"},
                         {"material", "ASA"},
                         {"helix_material", "ASA"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
        mock_db_set("lane_data", "lane4",
                    json{{"lane", "3"},
                         {"color", "#303030"},
                         {"color_name", "Dark Gray"},
                         {"material", "ASA"},
                         {"helix_material", "ASA"},
                         {"vendor", "Polymaker"},
                         {"vendor_name", "Polymaker"},
                         {"helix_locked_color", true},
                         {"helix_locked_material", true}});
    }

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

    // HELIX_MOCK_WEBCAMS="Name[:service],Name[:service],..." — the webcam list
    // discovery publishes, so the camera widget's source picker has something
    // to offer. Unset: one unnamed MJPEG feed, as a printer with a single
    // stock webcam presents. Each entry gets its own /webcamN/ path so a log
    // line shows which one a view is streaming. A service other than an MJPEG
    // family ("webrtc-go2rtc", ...) makes that entry snapshot-only.
    mock_webcams_ = parse_mock_webcams(std::getenv("HELIX_MOCK_WEBCAMS"));
    if (!mock_webcams_.empty()) {
        spdlog::info("[MoonrakerClientMock] {} mock webcam(s) via HELIX_MOCK_WEBCAMS",
                     mock_webcams_.size());
    }

    // HELIX_MOCK_BELT_A_HZ / HELIX_MOCK_BELT_B_HZ — the two belt paths'
    // simulated resonance peaks. Defaults (110/98) sit in "Poor match"
    // territory so the tuning loop is visible from the first sweep.
    if (const char* a_env = std::getenv("HELIX_MOCK_BELT_A_HZ")) {
        if (auto v = text_io::parse_leading<float>(a_env); v && *v > 0.0f) {
            belt_peaks_hz_[0] = *v;
        }
    }
    if (const char* b_env = std::getenv("HELIX_MOCK_BELT_B_HZ")) {
        if (auto v = text_io::parse_leading<float>(b_env); v && *v > 0.0f) {
            belt_peaks_hz_[1] = *v;
        }
    }

    // HELIX_MOCK_BELT_RANGE=<min>-<max> — the [resonance_tester] sweep range
    // the mock reports and sweeps, so a real capture fed through
    // HELIX_MOCK_BELT_CSV_A/_B is analysed over the band it was measured in.
    if (const char* range_env = std::getenv("HELIX_MOCK_BELT_RANGE")) {
        const std::string range(range_env);
        const auto dash = range.find('-', 1);
        const auto lo = text_io::parse_leading<double>(range.substr(0, dash));
        const auto hi = dash == std::string::npos
                            ? std::nullopt
                            : text_io::parse_leading<double>(range.substr(dash + 1));
        if (lo && hi && *lo > 0.0 && *hi > *lo) {
            resonance_min_freq_ = *lo;
            resonance_max_freq_ = *hi;
        }
    }

    // HELIX_MOCK_BELT_FAIL — reproduce a run that stalls, loses its CSV, uses
    // per-chip output, dies on an adxl345 error, or speaks Kalico's dialect.
    if (const char* fail_env = std::getenv("HELIX_MOCK_BELT_FAIL")) {
        const std::string fail(fail_env);
        if (fail == "stall") {
            belt_failure_ = BeltMockFailure::STALL;
        } else if (fail == "nofile") {
            belt_failure_ = BeltMockFailure::NO_FILE;
        } else if (fail == "multichip") {
            belt_failure_ = BeltMockFailure::MULTICHIP;
        } else if (fail == "error") {
            belt_failure_ = BeltMockFailure::ERROR;
        } else if (fail == "kalico") {
            belt_failure_ = BeltMockFailure::KALICO;
        }
        if (belt_failure_ != BeltMockFailure::NONE) {
            spdlog::info("[MoonrakerClientMock] TEST_RESONANCES failure mode '{}' via "
                         "HELIX_MOCK_BELT_FAIL",
                         fail);
        }
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
    double clamped = helix::sim::clamp_speed(factor);
    speedup_factor_.store(clamped);
    spdlog::info("[MoonrakerClientMock] Simulation speedup set to {}x", clamped);
}

double MoonrakerClientMock::get_simulation_speedup() const {
    return speedup_factor_.load();
}

helix::sim::SimSpeed MoonrakerClientMock::sim_speed() const {
    return helix::sim::SimSpeed::of(speedup_factor_.load());
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
    const auto now = std::chrono::steady_clock::now();
    if (now < replay_start_) {
        return;
    }
    // The capture's timestamps are simulated time, so the cursor into it is how
    // much simulated time the real wait so far bought.
    const auto due = sim_speed().accelerate_progress(now - replay_start_);
    const uint64_t due_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(due).count());

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

bool MoonrakerClientMock::simulates_chamber_temp() const {
    {
        std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
        for (const auto& s : discovery_.sensors()) {
            if (s == "temperature_sensor chamber_temp") {
                return true;
            }
        }
    }
    return has_chamber_sensor();
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

namespace {
/// Duty a real heater would be running at, 0.0-1.0, as Klipper reports it: flat
/// out while far below target, tapering in, and a trickle once holding. The
/// mock publishes this on every heater so a power readout has something to show.
double mock_heater_duty(double temperature, double target) {
    if (target <= 0.0) {
        return 0.0;
    }
    const double gap = target - temperature;
    if (gap <= 0.0) {
        return 0.05;
    }
    return std::clamp(gap / 10.0, 0.1, 1.0);
}
} // namespace

void MoonrakerClientMock::append_aux_heater_status(json& status_obj, double dt) {
    constexpr double AMBIENT = 25.0;
    constexpr double RATE_C_PER_S = 1.0;
    // The parsed hardware, not the persona's heater list: HELIX_MOCK_OBJECTS
    // heaters reach only the former.
    const std::vector<std::string> heaters = discovery_.hardware().heaters();
    const std::string chamber_key = chamber_heater_status_key();
    std::lock_guard<std::mutex> lock(aux_heaters_mutex_);
    for (const auto& name : heaters) {
        if (name.rfind("heater_generic ", 0) != 0 || name == chamber_key)
            continue;
        auto& [temp, target] = aux_heaters_.try_emplace(name, AMBIENT, 0.0).first->second;
        const double goal = target > 0.0 ? target : AMBIENT;
        const double step = RATE_C_PER_S * dt;
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

    // Small delay to simulate a realistic connection
    auto delay_ms = sim_speed().shorten_wait_ms(250);
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
    sim_link_down_ = false;

    // Dispatch historical temperature data first (fills graph with 2-3 min of data)
    dispatch_historical_temperatures();

    // Start live temperature simulation
    start_temperature_simulation();

    // Initial state for objects the printer.objects.subscribe handler does not
    // answer (probes, filament sensors, exclude_object, bed_mesh, chamber
    // sensor, mcu). It also repeats the subscribed heaters, print_stats and
    // toolhead with the same values the discovery subscription delivers, which
    // the status consumers treat as an unchanged frame.
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

    // Store disconnect callback (never invoked in mock, but stored for consistency)
    // Note: Not needed for this simple mock implementation

    return 0; // Success
}

std::string MoonrakerClientMock::kinematics() const {
    return kinematics_override_.empty()
               ? std::string(helix::mock::descriptor(printer_type_).kinematics)
               : kinematics_override_;
}

void MoonrakerClientMock::populate_capabilities() {
    // Held for the whole body: the simulation thread may already be running (connect()
    // starts it before discover_printer() calls this) and iterates these same lists.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Create mock Klipper object list for capabilities parsing
    json mock_objects = json::array();

    // Every persona inherits the default objects below unless its descriptor omits them.
    const auto persona = helix::mock::descriptor(printer_type_);
    auto inherits = [&persona](helix::mock::DefaultObjects object) {
        return (persona.omit & object) == 0;
    };
    using namespace helix::mock::default_object;

    // Add common objects
    mock_objects.push_back("heater_bed");
    mock_objects.push_back("extruder");
    // The second hub lane feeds a second extruder.
    if (is_mock_openams() && (openams_two_lane_units() || openams_fleet_lane_units())) {
        mock_objects.push_back("extruder1");
    }
    mock_objects.push_back("bed_mesh");

    // Add capabilities for UI testing (speaker for M300, firmware retraction for G10/G11)
    mock_objects.push_back("output_pin beeper");   // Triggers has_speaker_ capability
    mock_objects.push_back("firmware_retraction"); // Triggers has_firmware_retraction_ capability

    // Chamber temperature sensor for UI testing
    if (inherits(CHAMBER_SENSOR)) {
        mock_objects.push_back("temperature_sensor chamber");
    }

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
    case PrinterType::FLASHFORGE_CREATOR5:
        // Creator 5 Pro fingerprint. The database entry keys off [ff_toolchange]
        // (99%) and the head-dock grab sensor (95%); see the
        // flashforge_creator_5_pro heuristics in printer_database.json. Without
        // these in printer.objects.list the persona would be detected as a
        // generic CoreXY.
        mock_objects.push_back("ff_toolchange");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("gcode_button extruder_grab" + std::to_string(i));
        }
        // The chamber heater object separates the Pro from the heater-free
        // Creator 5, so a persona modelling the Pro must publish it.
        mock_objects.push_back("heater_generic chamber_heater");
        mock_objects.push_back("gcode_macro TOOLCHANGE_PARK");
        mock_objects.push_back("gcode_macro BED_MESH_CALIBRATE");
        break;
    case PrinterType::FLASHFORGE_CREATOR5_ZMOD:
        // Z-Mod's own objects. Its carriage buttons are 1-based, unlike the
        // Reforge persona's grab0..3 above; zmod_c5_detect in
        // toolchanger_addon keys off exactly these names.
        mock_objects.push_back("zmod");
        mock_objects.push_back("zmod_color");
        mock_objects.push_back("save_variables");
        for (int i = 1; i <= 4; ++i) {
            mock_objects.push_back("gcode_button extruder_pos" + std::to_string(i));
            mock_objects.push_back("gcode_button extruder_grab" + std::to_string(i));
        }
        // Same Pro-separating chamber heater as the Reforge persona above.
        mock_objects.push_back("heater_generic chamber_heater");
        break;
    case PrinterType::ELEGOO_CC1:
        // The CC1 capture's own objects. _COSMOS_SETTINGS and ELEGOO_PURGE are
        // what name the machine; its hostname and M191 match an enclosed QIDI too.
        for (const char* obj : {"configfile",
                                "print_stats",
                                "virtual_sdcard",
                                "pause_resume",
                                "display_status",
                                "screws_tilt_adjust",
                                "gcode_macro _COSMOS_SETTINGS",
                                "gcode_macro SAVE_CONFIG",
                                "gcode_macro _KAMP_Settings",
                                "gcode_macro LINE_PURGE",
                                "gcode_macro ELEGOO_PURGE",
                                "gcode_macro SMART_PARK",
                                "gcode_macro PRINT_START",
                                "gcode_macro M191",
                                "gcode_macro PRINT_END",
                                "gcode_macro CLEAN_NOZZLE",
                                "gcode_macro MOVE_TO_TRAY",
                                "gcode_macro CUT_FILAMENT",
                                "gcode_macro LOADCELL_Z_HOME",
                                "gcode_macro CALIBRATE_Z_OFFSET",
                                "gcode_macro _UPDATE_COSMOS"}) {
            mock_objects.push_back(obj);
        }
        break;
    case PrinterType::FLASHFORGE_AD5X:
        // The stock AD5X's public IFS macro, which names the machine. None of
        // the IFS module's own objects: those make discovery stand up the
        // production AD5X IFS backend, and this persona runs the mock IFS.
        mock_objects.push_back("gcode_macro SET_EXTRUDER_SLOT");
        break;
    case PrinterType::CREALITY_K2_PLUS:
        // The K2 Plus capture's identifying objects. `box` rides on is_mock_cfs().
        for (const char* obj :
             {"motor_control", "fan_feedback", "load_ai", "filament_rack",
              "output_pin extruder_fan", "output_pin power", "output_pin ptc_power",
              "temperature_sensor mcu_temp", "heater_fan chamber_heater_fan"}) {
            mock_objects.push_back(obj);
        }
        break;
    case PrinterType::SNAPMAKER_U1:
        // The U1 capture's identifying objects. Not `filament_detect`, which would
        // make discovery stand up the production Snapmaker backend; this persona
        // runs the mock one.
        for (const char* obj : {"tool", "fm175xx_reader", "tmc2240 stepper_x", "purifier", "camera",
                                "gcode_macro FILAMENT_DT_UPDATE", "gcode_macro FILAMENT_DT_QUERY",
                                "gcode_macro EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL"}) {
            mock_objects.push_back(obj);
        }
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("filament_motion_sensor e" + std::to_string(i) + "_filament");
        }
        break;
    default:
        // Other printers may not have these features
        break;
    }

    // Add LED effects (klipper-led_effect plugin objects)
    if (inherits(LED_EFFECTS)) {
        mock_objects.push_back("led_effect breathing");
        mock_objects.push_back("led_effect fire_comet");
        mock_objects.push_back("led_effect rainbow");
        mock_objects.push_back("led_effect static_white");
    }

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
    for (const auto& name : helix::sim::mock_padded_macro_names()) {
        mock_objects.push_back("gcode_macro " + name);
    }

    // Add LED-related macros (auto-detected by printer_discovery via LED keywords)
    if (inherits(LED_EFFECTS)) {
        mock_objects.push_back("gcode_macro LIGHTS_ON");
        mock_objects.push_back("gcode_macro LIGHTS_OFF");
        mock_objects.push_back("gcode_macro LIGHTS_TOGGLE");
        mock_objects.push_back("gcode_macro LED_PARTY");
        mock_objects.push_back("gcode_macro LED_NIGHTLIGHT");
    }

    // MCU objects for discovery
    mock_objects.push_back("mcu");
    if (inherits(EBB_CAN_MCU)) {
        mock_objects.push_back("mcu EBBCan");
    }

    // Humidity sensors (BME280/HTU21D for enclosure monitoring)
    if (inherits(BME280_CHAMBER)) {
        mock_objects.push_back("bme280 chamber");
    }
    if (inherits(HTU21D_DRYER)) {
        mock_objects.push_back("htu21d dryer");
    }

    // Width sensor (filament diameter measurement via Hall effect sensor)
    if (inherits(WIDTH_SENSOR)) {
        mock_objects.push_back("hall_filament_width_sensor");
    }

    // Moonraker plugins
    mock_objects.push_back("timelapse"); // Moonraker-Timelapse plugin

    // MMU/AMS system - Happy Hare uses "mmu" object name.
    // Suppressed in the MedusaHC modes, the standalone IFS module mode, the OpenAMS
    // mode and any persona that omits it: "mmu" detects Happy Hare (priority over every
    // other filament system) and would stand the wrong backend up.
    if (mmu_enabled_ && !is_mock_medusahc() && !is_mock_ifs_module() && !is_mock_openams() &&
        inherits(HAPPY_HARE_MMU)) {
        mock_objects.push_back("mmu");
    }

    // Probe objects: HELIX_MOCK_PROBE_TYPE, else the persona's own probe
    // (mock_internal::mock_probe_type).
    const json probe_status = helix::sim::mock_probe_status(printer_type_);
    for (auto it = probe_status.begin(); it != probe_status.end(); ++it) {
        mock_objects.push_back(it.key());
    }
    spdlog::debug("[MoonrakerClientMock] Mock probe: {}",
                  mock_internal::mock_probe_type(printer_type_));

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
    } else if (printer_type_ == PrinterType::FLASHFORGE_CREATOR5 ||
               printer_type_ == PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
        // One runout switch per head, named as in assets/config/presets/creator5_pro.json.
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("filament_switch_sensor fd_ex" + std::to_string(i));
        }
        // Z-Mod pairs each switch with a motion sensor.
        if (printer_type_ == PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
            for (int i = 0; i < 4; ++i) {
                mock_objects.push_back("filament_motion_sensor fm_ex" + std::to_string(i));
            }
        }
        spdlog::debug("[MoonrakerClientMock] Creator 5 filament sensors: fd_ex0..fd_ex3");
    } else if (printer_type_ == PrinterType::ELEGOO_CC1) {
        // The chassis runout switch, named as in assets/config/presets/cc1.json.
        mock_objects.push_back("filament_switch_sensor filament_sensor");
    } else if (printer_type_ == PrinterType::FLASHFORGE_AD5X) {
        // The toolhead switch, named as in assets/config/presets/ad5x.json.
        mock_objects.push_back("filament_switch_sensor head_switch_sensor");
    } else if (printer_type_ == PrinterType::CREALITY_K2_PLUS) {
        // The toolhead switch, named as in assets/config/presets/k2.json.
        mock_objects.push_back("filament_switch_sensor filament_sensor");
    } else if (inherits(RUNOUT_SENSOR)) {
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
        // The object that marks a firmware able to measure pressure advance
        // (the U1's flow calibrator depends on it), so the PA calibration
        // screen is reachable in --test. Stock Klipper has none.
        mock_objects.push_back("filament_parameters");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("tool T" + std::to_string(i));
        }
        // klipper-toolchanger's example config: the nozzle-contact probe and
        // the all-in-one calibration macro it drives. The macro's presence is
        // what makes the Tool Offsets calibration screen reachable.
        mock_objects.push_back("tools_calibrate");
        mock_objects.push_back("gcode_macro CALIBRATE_TOOL_OFFSETS");
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

    // Standalone IFS module mock mode (HELIX_MOCK_AMS=ifs-module): the
    // module's own objects plus its stock-named sensors, so real discovery
    // sets AmsType::AD5X_IFS and the production AmsBackendAd5xIfs runs against
    // the frames the simulation loop pushes. try_create_mock() declines this
    // value (it matches none of its spellings), exactly like the MedusaHC
    // modes above. save_variables is pushed too — the module's ifs_loaded
    // record rides it and the backend always subscribes it.
    if (is_mock_ifs_module()) {
        mock_objects.push_back("ifs");
        mock_objects.push_back("ifs_materials");
        mock_objects.push_back("save_variables");
        for (int i = 1; i <= 4; ++i) {
            mock_objects.push_back("filament_switch_sensor lane" + std::to_string(i));
        }
        mock_objects.push_back("filament_switch_sensor toolhead");
        spdlog::info("[MoonrakerClientMock] Standalone IFS module mock: ifs + ifs_materials + "
                     "4 lane sensors + toolhead");
    }

    // CFS mock mode (HELIX_MOCK_AMS=cfs): the stock K1 `box` status object the
    // production AmsBackendCfs subscribes and parses. try_create_mock()
    // declines this value, so real discovery plus the real backend run; pair
    // with HELIX_MOCK_PRINTER=k1 to latch the K1 stock dialect.
    if (is_mock_cfs()) {
        mock_objects.push_back("box");
        spdlog::info("[MoonrakerClientMock] CFS mock: box status object");
    }

    // OpenAMS mock mode (HELIX_MOCK_AMS=openams): the `oams_manager` status
    // object the production AmsBackendOpenAms claims through discovery.
    // try_create_mock() declines this value so the real backend runs.
    if (is_mock_openams()) {
        mock_objects.push_back("oams_manager");
        spdlog::info("[MoonrakerClientMock] OpenAMS mock: oams_manager status object");
    }

    append_skip_wrapper_objects(mock_objects);

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
    mock_config["printer"] = {{"kinematics", kinematics()}};
    // Add gcode_macro entries for param detection (shared with configfile.config response)
    mock_config.merge_patch(mock_internal::get_mock_gcode_macro_config());
    mock_config.merge_patch(skip_wrapper_sections());
    // Probe section — shared with the configfile.config query/subscribe responses
    // so all three payloads describe the same probe.
    mock_config.merge_patch(mock_internal::get_mock_probe_config(printer_type_));
    // Stepper travel limits, matching the configfile.settings the query and
    // subscribe handlers report. The real discovery sequence derives the build
    // volume from these before detection runs, so the mock has to carry them or
    // --test detects against an empty volume the live path no longer sees.
    const auto axis_max = persona.axis_max;
    mock_config["stepper_x"] = {{"position_min", mock_internal::MOCK_BED_X_MIN},
                                {"position_max", axis_max.x}};
    mock_config["stepper_y"] = {{"position_min", mock_internal::MOCK_BED_Y_MIN},
                                {"position_max", axis_max.y}};
    mock_config["stepper_z"] = {{"position_min", 0.0}, {"position_max", axis_max.z}};
    for (const auto& [name, settings] : extra_config_settings_.items()) {
        mock_config[name] = settings;
    }

    std::unordered_set<std::string> macros_snapshot;
    discovery_.modify_hardware([&](PrinterDiscovery& hw) {
        hw.parse_config_keys(mock_config);
        hw.parse_build_volume(mock_config);
        macros_snapshot = hw.macros();
    });

    // Populate macro param cache from mock config (same as real discovery sequence)
    helix::MacroParamCache::instance().populate_from_configfile(mock_config, macros_snapshot);

    spdlog::debug("[MoonrakerClientMock] Mock config: adxl345, resonance_tester, kinematics={}",
                  kinematics());

    // Populate printer objects for hardware discovery. Klipper lists each object
    // once; a persona's own lists can repeat an inherited default (a chamber
    // sensor, a chamber heater, a macro), so keep only the first.
    std::vector<std::string> all_objects;
    std::unordered_set<std::string> listed;
    for (const auto& obj : mock_objects) {
        std::string name = obj.get<std::string>();
        if (listed.insert(name).second) {
            all_objects.push_back(std::move(name));
        }
    }
    discovery_.modify_hardware([&](PrinterDiscovery& hw) { hw.set_printer_objects(all_objects); });
    update_cached_chamber_key();

    // Set mock MCU version data (after parse_objects which clears everything)
    const bool ebb_can = inherits(EBB_CAN_MCU);
    discovery_.modify_hardware([ebb_can](PrinterDiscovery& hw) {
        hw.set_mcu("stm32f446xx");
        if (ebb_can) {
            hw.set_mcu_list({"stm32f446xx", "stm32g0b1xx"});
            hw.set_mcu_versions(
                {{"mcu", "v0.12.0-155-g4cfa273e"}, {"mcu EBBCan", "v0.12.0-155-g4cfa273e"}});
        } else {
            hw.set_mcu_list({"stm32f446xx"});
            hw.set_mcu_versions({{"mcu", "v0.12.0-155-g4cfa273e"}});
        }
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
            for (const std::string& key : {std::string(backend->diagnostics_object()),
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
    // The sequence finishes inside the locked call, but its outcome is
    // delivered after the lock is released: a caller's callback may read or
    // rebuild the very lists the lock protects. The base class keeps the
    // completion callback for force_reconnect(), so a later call goes straight
    // through.
    struct Outcome {
        std::atomic<bool> deferring{true};
        bool completed = false;
        std::optional<std::string> failure;
    };
    auto outcome = std::make_shared<Outcome>();
    {
        std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
        MoonrakerClient::discover_printer(
            [outcome, on_complete]() {
                if (outcome->deferring) {
                    outcome->completed = true;
                } else if (on_complete) {
                    on_complete();
                }
            },
            [outcome, on_error](const std::string& reason) {
                if (outcome->deferring) {
                    outcome->failure = reason;
                } else if (on_error) {
                    on_error(reason);
                }
            });
    }
    outcome->deferring = false;
    if (outcome->completed && on_complete) {
        on_complete();
    } else if (outcome->failure && on_error) {
        on_error(*outcome->failure);
    }
}

namespace {
std::string effective_mock_ams_env() {
    return helix::mock::effective_mock_ams(std::getenv("HELIX_MOCK_AMS"),
                                           std::getenv("HELIX_MOCK_PRINTER"));
}
} // namespace

bool MoonrakerClientMock::mock_toolchanger_selected() {
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc";
}

bool MoonrakerClientMock::is_mock_toolchanger() const {
    return mock_toolchanger_selected();
}

MoonrakerClientMock::MedusaVariant MoonrakerClientMock::mock_medusa_variant() {
    const std::string ams_type = effective_mock_ams_env();
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

void MoonrakerClientMock::append_k2_status(json& status) const {
    // Creality's motor controller and fan tachometer modules, in the fields
    // cfs::parse_motor_control and PrinterFanState read.
    status["motor_control"] = {{"motor_ready", true}};
    status["fan_feedback"] = {{"fan0_speed", 0}, {"fan1_speed", 0}, {"fan2_speed", 0}};
}

bool MoonrakerClientMock::is_mock_cfs() const {
    // "cfs"/"cfs-k1": the K1 stock dialect. try_create_mock() declines these
    // values so the production AmsBackendCfs runs (pair with
    // HELIX_MOCK_PRINTER=k1 to latch the dialect).
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "cfs" || ams_type == "cfs-k1";
}

bool MoonrakerClientMock::is_mock_openams() const {
    return effective_mock_ams_env() == "openams";
}

namespace {

// One unit of the openams plugin shape: its hub lane and what its family is.
struct OpenAmsUnitSpec {
    const char* name;
    const char* display_name;
    const char* family;
    const char* lane;
    int bays;
    double dryer_max_c;
    bool dryer_requires_unloaded;
};

constexpr OpenAmsUnitSpec kAmsHt{"ams_ht", "AMS HT", "ams_ht", "fps", 1, 80.0, false};
constexpr OpenAmsUnitSpec kAms2{"ams2", "AMS 2 Pro", "ams2", "fps", 4, 65.0, true};
constexpr OpenAmsUnitSpec kAms2b{"ams2b", "AMS 2 Pro", "ams2", "fps2", 4, 65.0, true};
constexpr OpenAmsUnitSpec kAms2c{"ams2c", "AMS 2 Pro", "ams2", "fps2", 4, 65.0, true};

std::vector<OpenAmsUnitSpec> openams_unit_specs() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    const std::string shape = units ? units : "";
    if (shape == "two_lanes") {
        return {kAmsHt, kAms2, kAms2b, kAms2c};
    }
    if (shape == "shared") {
        return {kAmsHt, kAms2};
    }
    if (shape == "fleet") {
        // An AMS HT then eleven AMS 2 Pro: units 1-10 feed lane `fps`, the last two `fps2`.
        static constexpr const char* kNames[] = {"ams_ht", "ams2_1", "ams2_2",  "ams2_3",
                                                 "ams2_4", "ams2_5", "ams2_6",  "ams2_7",
                                                 "ams2_8", "ams2_9", "ams2_10", "ams2_11"};
        std::vector<OpenAmsUnitSpec> fleet = {kAmsHt};
        for (std::size_t i = 1; i < std::size(kNames); ++i) {
            OpenAmsUnitSpec spec = kAms2;
            spec.name = kNames[i];
            spec.lane = i < 10 ? "fps" : "fps2";
            fleet.push_back(spec);
        }
        return fleet;
    }
    return {};
}

// The extruder each lane feeds; the lane's position in the plugin's lane list.
const char* openams_lane_extruder(const std::string& lane) {
    return lane == "fps2" ? "extruder1" : "extruder";
}

} // namespace

bool MoonrakerClientMock::openams_shared_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "shared";
}

bool MoonrakerClientMock::openams_fleet_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "fleet";
}

double MoonrakerClientMock::openams_ambient_c(int unit) {
    return openams_fleet_lane_units() ? 24.0 + 1.5 * (unit % 5) : 27.7;
}

bool MoonrakerClientMock::openams_two_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "two_lanes";
}

bool MoonrakerClientMock::openams_plugin_units() {
    return openams_shared_lane_units() || openams_two_lane_units() || openams_fleet_lane_units();
}

int MoonrakerClientMock::openams_unit_count() {
    return static_cast<int>(openams_unit_specs().size());
}

int MoonrakerClientMock::openams_slot_count() {
    int slots = 0;
    for (const auto& spec : openams_unit_specs()) {
        slots += spec.bays;
    }
    return slots;
}

nlohmann::json MoonrakerClientMock::openams_shared_status_json() const {
    // Units spread over one or two hub lanes (an AMS HT and AMS 2 Pro on `fps`, and
    // with two_lanes two AMS 2 Pro on `fps2`): each lane is one hub, one FPS and one
    // extruder. Groups T0-Tn name a slot each; they are filament groups, not
    // toolheads.
    const auto specs = openams_unit_specs();
    const int loaded = openams_loaded_slot_.load();

    std::vector<std::string> lane_ids;
    nlohmann::json units = nlohmann::json::array();
    nlohmann::json groups = nlohmann::json::array();
    std::vector<std::string> slot_lane;
    int next_slot = 0;
    for (std::size_t u = 0; u < specs.size(); ++u) {
        const auto& spec = specs[u];
        if (std::find(lane_ids.begin(), lane_ids.end(), spec.lane) == lane_ids.end()) {
            lane_ids.push_back(spec.lane);
        }
        nlohmann::json slots = nlohmann::json::array();
        for (int b = 0; b < spec.bays; ++b) {
            const int id = next_slot++;
            slots.push_back({{"id", id}, {"bay", b}, {"ready", true}, {"loaded", id == loaded}});
            slot_lane.push_back(spec.lane);
            const std::string name =
                "T" + std::to_string(id); // DISPLAY_NUMBERING_OK: OpenAMS group id
            groups.push_back({{"name", name}, {"lane", spec.lane}, {"slots", {id}}});
        }
        units.push_back({{"id", std::to_string(u + 1)},
                         {"name", spec.name},
                         {"kind", "oams"},
                         {"topology", "hub"},
                         {"lane", spec.lane},
                         {"connected", true},
                         {"slots", slots}});
    }

    const bool loaded_valid = loaded >= 0 && loaded < static_cast<int>(slot_lane.size());
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto& id : lane_ids) {
        const bool here = loaded_valid && slot_lane[loaded] == id;
        nlohmann::json lane = {{"id", id},          {"state", here ? "loaded" : "unloaded"},
                               {"following", here}, {"direction", 1},
                               {"pressure", 0.79},  {"set_point", 0.5}};
        if (here) {
            lane["current_group"] =
                "T" + std::to_string(loaded); // DISPLAY_NUMBERING_OK: OpenAMS group id
            lane["current_slot"] = loaded;
        } else {
            lane["current_group"] = nullptr;
            lane["current_slot"] = -1;
        }
        lanes.push_back(lane);
    }
    nlohmann::json status = {{"api_version", 1}, {"schema", "openams.manager"},
                             {"ready", true},    {"lanes", lanes},
                             {"units", units},   {"groups", groups}};
    // klipper_openams publishes nothing beyond the versioned core above and
    // advertises its macros; the openams plugin adds per-lane and topology views
    // and advertises the manager commands the Mainsail panel runs.
    const char* api = std::getenv("HELIX_MOCK_OPENAMS_API");
    if (api && std::string(api) == "legacy") {
        status["commands"] = {{"load", "OPENAMS_LOAD"},
                              {"unload", "OPENAMS_UNLOAD"},
                              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                              {"reset", "OAMSM_CLEAR_ERRORS"}};
    } else {
        status["commands"] = {{"load", "OAMSM_LOAD_TO_TOOLHEAD"},
                              {"unload", "OAMSM_UNLOAD_FROM_TOOLHEAD"},
                              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                              {"reset", "OAMSM_CLEAR_ERRORS"}};
        status["devices"] = nlohmann::json::object();
        for (size_t i = 0; i < specs.size(); ++i) {
            nlohmann::json device = openams_device_json(static_cast<int>(i));
            device["faults"] = nlohmann::json::array();
            if (i == 0 && openams_fault_active_.load()) {
                const char* code = std::getenv("HELIX_MOCK_OPENAMS_FAULT");
                device["faults"].push_back({{"severity", "stop"},
                                            {"code", code && *code ? code : "motor_drive_fault"},
                                            {"text", code && *code ? code : "motor_drive_fault"},
                                            {"bay", nullptr},
                                            {"actions", {"clear_fault"}},
                                            {"source", "firmware"}});
            }
            status["devices"][specs[i].name] = device;
        }
        status["devices"]["ams_ht"].update(openams_device_json(0));
        for (std::size_t u = 1; u < specs.size(); ++u) {
            status["devices"][specs[u].name] = openams_device_json(static_cast<int>(u));
            if (!status["devices"][specs[u].name].contains("faults")) {
                status["devices"][specs[u].name]["faults"] = nlohmann::json::array();
            }
        }
        nlohmann::json by_fps = nlohmann::json::object();
        nlohmann::json fps_ids = nlohmann::json::array();
        for (const auto& id : lane_ids) {
            const bool here = loaded_valid && slot_lane[loaded] == id;
            by_fps[id] = {{"op", here ? "loaded" : "idle"},
                          {"pressure", 0.79},
                          {"set_point", 0.5},
                          {"extruder", openams_lane_extruder(id)}};
            fps_ids.push_back(id);
        }
        status["lanes_by_fps"] = by_fps;
        nlohmann::json oams = nlohmann::json::object();
        for (std::size_t u = 0; u < specs.size(); ++u) {
            oams[specs[u].name] = {
                {"idx", static_cast<int>(u) + 1}, {"lane", specs[u].lane}, {"bays", specs[u].bays}};
        }
        status["topology"] = {{"schema_version", 1}, {"fps", fps_ids}, {"oams", oams}};
    }
    return status;
}

nlohmann::json MoonrakerClientMock::openams_device_json(int unit) const {
    // What the openams plugin publishes per unit: capabilities, the environment
    // sensor, the dryer and the actions the unit advertises. An AMS 2 Pro's dryer
    // needs every bay unloaded and tops out at 65 C.
    const auto specs = openams_unit_specs();
    const OpenAmsUnitSpec& spec = specs.at(static_cast<std::size_t>(unit));
    const bool ht = unit == 0;
    const OpenAmsDryerSim& sim = openams_dryers_[unit];
    const double target = sim.target_c.load();
    const double remaining = sim.remaining_s.load();
    const double chamber = sim.chamber_c.load();
    const bool running = remaining > 0.0 && target > 0.0;
    const char* state = !running ? "off" : (chamber < target - 1.0 ? "heating" : "holding");
    const int bays = spec.bays;
    nlohmann::json device = {
        {"oams_idx", unit + 1},
        {"connected", true},
        {"capabilities",
         {{"family", spec.family},
          {"display_name", spec.display_name},
          {"bays", bays},
          {"dryer", true},
          {"dryer_target_min_c", 45.0},
          {"dryer_target_max_c", spec.dryer_max_c},
          {"dryer_requires_unloaded", spec.dryer_requires_unloaded},
          {"heater_count", 1},
          {"fan_count", 1}}},
        {"environment",
         {{"temp_c", std::round(chamber * 10.0) / 10.0},
          {"rh_pct", running                      ? 18.0
                     : openams_fleet_lane_units() ? 22.0 + (unit * 7) % 30
                                                  : (ht ? 29.0 : 31.0)},
          {"source", "firmware"}}},
        {"dryer",
         {{"state", state},
          {"target_c", running ? target : 0.0},
          {"remaining_s", running ? static_cast<int>(remaining) : 0},
          {"heater_pct", !running ? 0.0 : (chamber < target - 1.0 ? 100.0 : 35.0)},
          {"fan_pct", running ? 60.0 : 0.0},
          {"fault", "none"},
          {"adapter", nullptr}}},
        {"telemetry",
         {{"dryer",
           {{"state_name", state},
            {"target_c", running ? target : 0.0},
            {"chamber_c", std::round(chamber * 10.0) / 10.0},
            {"remaining_s", running ? static_cast<int>(remaining) : 0}}}}},
        {"supported_actions",
         running ? nlohmann::json::array(
                       {"clear_errors", "load", "unload", "follower", "dryer_stop", "clear_fault"})
                 : nlohmann::json::array({"clear_errors", "load", "unload", "follower",
                                          "dryer_start", "dryer_stop", "clear_fault"})}};
    return device;
}

void MoonrakerClientMock::service_openams_dryers(double dt_s) {
    constexpr double kRateCPerS = 0.5;
    for (int unit = 0; unit < kOpenAmsMaxUnits; ++unit) {
        OpenAmsDryerSim& sim = openams_dryers_[unit];
        const double kAmbientC = openams_ambient_c(unit);
        const double remaining = sim.remaining_s.load();
        const double target = sim.target_c.load();
        double chamber = sim.chamber_c.load();
        if (remaining > 0.0 && target > 0.0) {
            chamber = chamber < target ? std::min(target, chamber + kRateCPerS * dt_s) : target;
            sim.remaining_s = std::max(0.0, remaining - dt_s);
            if (sim.remaining_s.load() <= 0.0) {
                sim.target_c = 0.0;
            }
        } else if (chamber > kAmbientC) {
            chamber = std::max(kAmbientC, chamber - kRateCPerS * dt_s);
        }
        sim.chamber_c = chamber;
    }
}

nlohmann::json MoonrakerClientMock::openams_status_json() const {
    if (openams_plugin_units()) {
        return openams_shared_status_json();
    }
    // One hub unit, four bays, one FPS lane. Only bays 2 and 3 hold spools;
    // the lane's current slot follows openams_loaded_slot_.
    static const char* const GROUP_OF_SLOT[] = {"T0", "T1", "T2", "T0"};
    const int loaded = openams_loaded_slot_.load();
    nlohmann::json slots = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        slots.push_back({{"id", i}, {"bay", i}, {"ready", i >= 2}, {"loaded", i == loaded}});
    }
    nlohmann::json lane = {{"id", "fps"},
                           {"state", loaded >= 0 ? "loaded" : "unloaded"},
                           {"pressure", 0.5},
                           {"set_point", 0.5}};
    if (loaded >= 0) {
        lane["current_group"] = GROUP_OF_SLOT[loaded];
        lane["current_slot"] = loaded;
    } else {
        lane["current_group"] = nullptr;
        lane["current_slot"] = -1;
    }
    return {{"api_version", 1},
            {"schema", "openams.manager"},
            {"ready", true},
            {"commands",
             {{"load", "OPENAMS_LOAD"},
              {"unload", "OPENAMS_UNLOAD"},
              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
              {"reset", "OAMSM_CLEAR_ERRORS"}}},
            {"lanes", nlohmann::json::array({lane})},
            {"units", nlohmann::json::array({{{"id", "1"},
                                              {"name", "OpenAMS"},
                                              {"kind", "oams"},
                                              {"topology", "hub"},
                                              {"lane", "fps"},
                                              {"connected", true},
                                              {"slots", slots}}})},
            {"groups", nlohmann::json::array({{{"name", "T0"}, {"lane", "fps"}, {"slots", {0, 3}}},
                                              {{"name", "T1"}, {"lane", "fps"}, {"slots", {1}}},
                                              {{"name", "T2"}, {"lane", "fps"}, {"slots", {2}}}})}};
}

nlohmann::json MoonrakerClientMock::cfs_box_status_json() const {
    // Stock K1 `box` frame: T1 = unit 1, one array entry per bay. Bay A
    // carries a spool; the others report the "none"/-1 sentinels. Same shape
    // the unit-test fixtures feed parse_stock_box_status().
    auto box = nlohmann::json::parse(R"({
        "state": "connect", "filament": 0, "auto_refill": 0, "enable": 1,
        "same_material": 0,
        "map": {"T1A": "T1A", "T1B": "T1B", "T1C": "T1C", "T1D": "T1D"},
        "T1": {"state": "connect", "filament": "None",
               "vender": ["Creality", "none", "none", "none"],
               "remain_len": ["212", "-1", "-1", "-1"],
               "color_value": ["0E8E4F", "-1", "-1", "-1"],
               "material_type": ["000003", "-1", "-1", "-1"]}
    })");
    // HELIX_MOCK_CFS_BOXES lists the box addresses on the bus ("1,2,4"); box 1
    // is always present, every other listed box is empty.
    const char* boxes = std::getenv("HELIX_MOCK_CFS_BOXES");
    for (const char* c = boxes; c && *c; ++c) {
        if (*c < '2' || *c > '4') {
            continue;
        }
        const std::string unit = std::string("T") + *c;
        box[unit] = {{"state", "connect"},
                     {"filament", "None"},
                     {"vender", {"none", "none", "none", "none"}},
                     {"remain_len", {"-1", "-1", "-1", "-1"}},
                     {"color_value", {"-1", "-1", "-1", "-1"}},
                     {"material_type", {"-1", "-1", "-1", "-1"}}};
        for (char bay : {'A', 'B', 'C', 'D'}) {
            box["map"][unit + bay] = unit + bay;
        }
    }
    // The loaded bay: its unit's `filament` carries the bay letter, "None" elsewhere.
    if (const int loaded = cfs_loaded_slot_.load(); loaded >= 0) {
        // The box frame's wire key for a unit, not a display label.
        const std::string unit =
            "T" + std::to_string(loaded / 4 + 1); // DISPLAY_NUMBERING_OK: box frame wire key
        if (box.contains(unit)) {
            box[unit]["filament"] = std::string(1, static_cast<char>('A' + loaded % 4));
        }
    }
    return box;
}

void MoonrakerClientMock::simulate_cfs_find_cut_pos() {
    // The real sweep streams a position line every ~0.4s for ~60s and the
    // gcode/script RPC returns when the macro finishes, so the terminal lines
    // always precede the RPC ack. The mock reproduces that order by
    // dispatching the lines synchronously inside gcode_script (the same shape
    // the G0 out-of-range handler uses for its `!!` broadcast): a listener
    // registered before the send sees them, and the RPC's completion fires
    // after. The cut position scales off the persona envelope (a K1 Max
    // envelope reproduces the verified 304.0 reading).
    const double cut_y = helix::mock::descriptor(printer_type_).axis_max.y - 3.5;
    char buf[96];
    snprintf(buf, sizeof(buf), "Found cut position y: %.1f", cut_y);
    dispatch_gcode_response(buf);
    snprintf(buf, sizeof(buf), "MODIFY_BOX_CFG: success, cut_pos_y=%.1f,", cut_y);
    dispatch_gcode_response(buf);
    snprintf(buf, sizeof(buf), "SAVE_BOX_CFG ok: cut_pos_y=%.1f", cut_y);
    dispatch_gcode_response(buf);
}

// Gated only by is_mock_cfs() at the call site: CR_BOX_* is the K2 dialect's
// vocabulary, and a K1-dialect script never contains it, so no persona check is needed.
bool MoonrakerClientMock::apply_cfs_cr_box_script(const std::string& gcode) {
    bool touched = false;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("CR_BOX_EXTRUDE", 0) == 0) {
            // CR_BOX_EXTRUDE TNN=T<unit><bay>
            const size_t t = line.find("TNN=T");
            if (t == std::string::npos || t + 6 >= line.size()) {
                continue;
            }
            const int unit = line[t + 5] - '0';
            const int bay = line[t + 6] - 'A';
            if (unit < 1 || unit > 4 || bay < 0 || bay > 3) {
                continue;
            }
            cfs_loaded_slot_.store((unit - 1) * 4 + bay);
            touched = true;
        } else if (line.rfind("CR_BOX_RETRUDE", 0) == 0) {
            cfs_loaded_slot_.store(-1);
            touched = true;
        }
    }
    if (touched) {
        // The toolhead switch sees filament exactly while a bay is loaded.
        dispatch_status_update(
            {{"box", cfs_box_status_json()},
             {"filament_switch_sensor filament_sensor",
              {{"filament_detected", cfs_toolhead_filament_detected()}, {"enabled", true}}}});
    }
    return touched;
}

bool MoonrakerClientMock::apply_cfs_box_custom_command(const std::string& gcode) {
    const size_t c = gcode.find("CMD=");
    if (c == std::string::npos) {
        return false;
    }
    size_t start = c + 4;
    size_t end = gcode.find_first_of(" \t", start);
    const std::string cmd =
        gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);

    // Geometry scales off the persona envelope the way the verified K1 Max
    // values scale off its 307.5: safe_pos_y = y_max - 16 (291.5 on a Max).
    // The Max parks at the captured box.cfg extrude_pos_x (184.5); smaller
    // K1s were never captured, so they scale the X envelope.
    const auto max = helix::mock::descriptor(printer_type_).axis_max;
    const double safe_y = max.y - 16.0;
    const double extrude_x = printer_type_ == PrinterType::CREALITY_K1_MAX ? 184.5 : max.x * 0.8;

    // Move to a parked position: homed, motors on, snapshot-dispatched as one
    // frame (same shape the G0 handler emits).
    auto park = [&](double x, double y) {
        {
            std::lock_guard<std::mutex> pos_lock(pos_mutex_);
            pos_x_.store(x);
            pos_y_.store(y);
        }
        motors_enabled_.store(true);
        {
            std::lock_guard<std::mutex> lock(homed_axes_mutex_);
            homed_axes_ = "xyz";
        }
        reset_idle_timeout();
        dispatch_status_update(
            {{"toolhead", {{"homed_axes", "xyz"}, {"position", {x, y, pos_z_.load(), 0.0}}}},
             {"motion_report",
              {{"live_position", {x, y, pos_z_.load() + gcode_offset_z_.load(), 0.0}}}}});
    };

    if (cmd == "XYZ_ZERO") {
        // Full home (incl. PRTouch Z) then park; the real firmware echoes the
        // park coordinates on the response stream.
        park(extrude_x, safe_y);
        char buf[64];
        snprintf(buf, sizeof(buf), "x_park = %.2f", extrude_x);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "y_park = %.2f", safe_y);
        dispatch_gcode_response(buf);
        return true;
    }
    if (cmd == "COORDINATES_ADJUST_PREPARE") {
        park(extrude_x, safe_y);
        return true;
    }
    if (cmd == "COORDINATES_ADJUST_SAVE_POS") {
        // The firmware reads the LIVE toolhead position; mock reports the
        // same pair the save lines carry.
        double x, y, z;
        read_position_snapshot(x, y, z);
        char buf[112];
        snprintf(buf, sizeof(buf), "cmd_save_extrude_pos x=%.2f y=%.2f", x, y);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "MODIFY_BOX_CFG: success, extrude_pos_x=%.1f,", x);
        dispatch_gcode_response(buf);
        snprintf(buf, sizeof(buf), "SAVE_BOX_CFG ok: extrude_pos_x=%.1f,extrude_pos_y=%.1f", x, y);
        dispatch_gcode_response(buf);
        return true;
    }
    if (cmd == "Y_SAFE") {
        park(pos_x_.load(), safe_y);
        return true;
    }
    spdlog::debug("[MoonrakerClientMock] BOX_CUSTOM_COMMAND {} not simulated", cmd);
    return false;
}

bool MoonrakerClientMock::is_mock_ifs_module() const {
    // "ifs-module", not "ifs": the bare value (and "ad5x") selects the
    // AmsBackendMock simulation in try_create_mock(); this mode runs the real
    // backend, so the two must not collide.
    const std::string ams_type = effective_mock_ams_env();
    return ams_type == "ifs-module" || ams_type == "ifs_module" || ams_type == "ad5x-module";
}

nlohmann::json MoonrakerClientMock::ifs_module_status_json() const {
    const int loaded = ifs_module_loaded_.load();
    const uint8_t presence = ifs_module_presence_.load();
    json loaded_channels = json::array();
    for (int i = 0; i < 4; ++i) {
        if (presence & (1u << i)) {
            loaded_channels.push_back(i + 1);
        }
    }
    // Shape mirrored from the module's ifs.py get_status().
    return json{{"connected", true},
                {"error", nullptr},
                {"channel_count", 4},
                {"version", "mock"},
                {"probed", true},
                {"state", 5},
                {"activity", "ready"},
                {"activity_channel", 0},
                {"active_channel", loaded},
                {"loaded_channels", std::move(loaded_channels)},
                {"moving_channels", json::array()},
                {"pending_insert_channels", json::array()},
                {"params", json::object()}};
}

nlohmann::json MoonrakerClientMock::ifs_module_materials_json() const {
    static const std::map<std::string, double> handling_temps{
        {"PLA", 220.0}, {"PLA-CF", 220.0}, {"SILK", 230.0},   {"TPU", 230.0},
        {"ABS", 250.0}, {"PETG", 250.0},   {"PETG-CF", 250.0}};

    json temps = json::object();
    for (const auto& [name, t] : handling_temps) {
        temps[name] = t;
    }

    json slots = json::object();
    {
        std::lock_guard<std::mutex> lock(ifs_module_materials_mutex_);
        for (const auto& [slot, tm] : ifs_module_materials_) {
            auto temp_it = handling_temps.find(tm.first);
            slots[std::to_string(slot)] = json{
                {"type", tm.first.empty() ? json(nullptr) : json(tm.first)},
                {"color", tm.second.empty() ? json(nullptr) : json("#" + tm.second)},
                {"temp", temp_it != handling_temps.end() ? json(temp_it->second) : json(nullptr)}};
        }
    }

    const int loaded = ifs_module_loaded_.load();
    json loaded_entry = nullptr;
    if (loaded >= 1 && slots.contains(std::to_string(loaded))) {
        loaded_entry = slots[std::to_string(loaded)];
    }
    return json{{"available", true},
                {"channel_count", 4},
                {"enabled", true},
                {"slots", std::move(slots)},
                {"loaded", std::move(loaded_entry)},
                {"purge_first_mm", json::object()},
                {"temperatures", std::move(temps)}};
}

nlohmann::json MoonrakerClientMock::ifs_module_vars_json() const {
    return json{{"variables", json{{"ifs_loaded", ifs_module_loaded_.load()}, {"ifs_at_hub", 0}}}};
}

bool MoonrakerClientMock::apply_ifs_module_gcode(const std::string& cmd, const std::string& gcode) {
    auto slot_param = [&]() -> int {
        const size_t s = gcode.find("SLOT=");
        if (s == std::string::npos) {
            return -1;
        }
        try {
            return std::stoi(gcode.substr(s + 5));
        } catch (...) {
            return -1;
        }
    };
    auto value_param = [&](const char* key) -> std::optional<std::string> {
        const std::string pattern = std::string(key) + "=";
        const size_t p = gcode.find(pattern);
        if (p == std::string::npos) {
            return std::nullopt;
        }
        const size_t start = p + pattern.size();
        const size_t end = gcode.find_first_of(" \t", start);
        return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    if (cmd == "IFS_SET_MATERIAL") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            auto type = value_param("TYPE");
            auto color = value_param("COLOR");
            std::lock_guard<std::mutex> lock(ifs_module_materials_mutex_);
            auto& tm = ifs_module_materials_[slot];
            if (type) {
                tm.first = *type;
            }
            if (color) {
                tm.second = *color;
            }
            spdlog::info("[MoonrakerClientMock] IFS module: slot {} -> {} #{}", slot, tm.first,
                         tm.second);
        }
        return true;
    }
    if (cmd == "IFS_LOAD" || cmd == "IFS_SELECT") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            ifs_module_loaded_.store(slot);
            spdlog::info("[MoonrakerClientMock] IFS module: loaded slot {}", slot);
        }
        return true;
    }
    if (cmd == "IFS_UNLOAD") {
        ifs_module_loaded_.store(0);
        spdlog::info("[MoonrakerClientMock] IFS module: unloaded");
        return true;
    }
    if (cmd == "IFS_EJECT") {
        const int slot = slot_param();
        if (slot >= 1 && slot <= 4) {
            ifs_module_presence_.fetch_and(static_cast<uint8_t>(~(1u << (slot - 1))));
            if (ifs_module_loaded_.load() == slot) {
                ifs_module_loaded_.store(0);
            }
            spdlog::info("[MoonrakerClientMock] IFS module: ejected slot {}", slot);
        }
        return true;
    }
    // Bare T<n>: the module's slicer spelling (T0..T3 -> slots 1..4).
    if (cmd.size() >= 2 && cmd[0] == 'T' &&
        std::all_of(cmd.begin() + 1, cmd.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int tool = std::stoi(cmd.substr(1));
        if (tool >= 0 && tool < 4) {
            ifs_module_loaded_.store(tool + 1);
            spdlog::info("[MoonrakerClientMock] IFS module: T{} -> slot {}", tool, tool + 1);
        }
        return true;
    }
    return false;
}

namespace {

// Z-Mod's filament.json defaults: the 24 palette colours in index order (an
// off-palette HEX snaps to index 0, white) and the material types
// CHANGE_ZCOLOR accepts.
constexpr std::array<const char*, 24> kZmodPalette = {
    "FFFFFF", "FEF043", "DCF478", "0ACC38", "067749", "0C6283", "0DE2A0", "75D9F3",
    "45A8F9", "2750E0", "46328E", "A03CF7", "F330F9", "D4B0DC", "F95D73", "F72224",
    "7C4B00", "F98D33", "FDEBD5", "D3C4A3", "AF7836", "898989", "BCBCBC", "161616"};
constexpr std::array<const char*, 17> kZmodValidTypes = {
    "PLA",     "PETG",  "PLA-CF", "PETG-CF", "ABS",     "ASA",     "SILK",    "PET-CF", "S-PAHT",
    "S-MULTI", "PA-CF", "HIPS",   "PVA",     "TPU-90A", "TPU-95A", "TPU-64D", "?"};

} // namespace

json MoonrakerClientMock::zmod_color_status() const {
    std::lock_guard<std::mutex> lock(zmod_mutex_);
    json slots = json::array();
    for (std::size_t i = 0; i < zmod_slots_.size(); ++i) {
        slots.push_back({{"ID", std::to_string(i + 1)},
                         {"Material", zmod_slots_[i].first},
                         {"HEX", zmod_slots_[i].second}});
    }
    return json{{"active_tool_id", zmod_active_tool_.load()},
                {"total_tools", 4},
                // The head count, as the firmware reports it; not the 24-entry
                // palette size, which is what the field name suggests.
                {"color_limit", 4},
                {"display", false},
                {"valid_types", kZmodValidTypes},
                {"hidden_types", json::array()},
                {"palette", kZmodPalette},
                {"slots", std::move(slots)}};
}

// Z-Mod commands (creator5_zmod persona): the zmod_color extra's macros.
// Matched on the command token exactly, and armed only for this persona.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_zmod(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    auto param = [&gcode](const char* key) -> std::string {
        const std::string needle = std::string(key) + "=";
        const size_t at = gcode.find(needle);
        if (at == std::string::npos) {
            return {};
        }
        const size_t start = at + needle.size();
        const size_t end = gcode.find_first_of(" \t", start);
        return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    auto refuse = [this](const std::string& message) {
        spdlog::warn("[MoonrakerClientMock] Z-Mod gcode rejected: {}", message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    };

    if (cmd == "_T_IN") {
        int tool = -1;
        try {
            tool = std::stoi(param("T"));
        } catch (...) {
        }
        if (tool < 0 || tool > 3) {
            return refuse("T out of range: " + param("T"));
        }
        zmod_active_tool_.store(tool);
        dispatch_status_update({{"zmod_color", {{"active_tool_id", tool}}}});
        return 0;
    }
    if (cmd == "_T_OUT") {
        zmod_active_tool_.store(-1);
        dispatch_status_update({{"zmod_color", {{"active_tool_id", -1}}}});
        return 0;
    }
    if (cmd == "CHANGE_ZCOLOR") {
        const std::string type = param("TYPE");
        if (std::find(kZmodValidTypes.begin(), kZmodValidTypes.end(), type) ==
            kZmodValidTypes.end()) {
            return refuse("Unknown material type: " + type);
        }
        int slot = 0;
        try {
            slot = std::stoi(param("SLOT"));
        } catch (...) {
        }
        if (slot < 1 || slot > 4) {
            return refuse("SLOT out of range: " + param("SLOT"));
        }
        std::string hex = param("HEX");
        hex = helix::text_io::to_upper(hex);
        // The firmware stores only palette colours; anything else snaps to
        // index 0, white.
        const bool in_palette =
            std::find(kZmodPalette.begin(), kZmodPalette.end(), hex) != kZmodPalette.end();
        {
            std::lock_guard<std::mutex> lock(zmod_mutex_);
            zmod_slots_[slot - 1] = {type, in_palette ? hex : "FFFFFF"};
        }
        dispatch_status_update({{"zmod_color", {{"slots", zmod_color_status()["slots"]}}}});
        return 0;
    }
    return std::nullopt;
}

nlohmann::json MoonrakerClientMock::medusa_status_json() const {
    const MedusaVariant variant = mock_medusa_variant();
    if (variant == MedusaVariant::NONE) {
        return nlohmann::json::object();
    }

    constexpr int kToolCount = 4;
    // Out of the machine entirely - not on the head and not in its dock. The
    // ejected lane the ghost rendering and the EMPTY slot status need, and the
    // state a user produces by lifting a hot end off the rack.
    constexpr int kEjectedTool = 3;
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
            sensors["t" + std::to_string(i)] = (i == current || i == kEjectedTool) ? 0 : 1;
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
            obj["tool" + std::to_string(i) + "_docked"] = (i != current && i != kEjectedTool);
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

void MoonrakerClientMock::publish_u1_channel_frame(int ext) {
    // "filament_feed left" carries extruders 0/1, "right" carries 2/3.
    const int first = (ext < 2) ? 0 : 2;
    const std::string side = (ext < 2) ? "left" : "right";
    nlohmann::json channels = nlohmann::json::object();
    for (int i = first; i < first + 2; ++i) {
        channels["extruder" + std::to_string(i)] = {
            {"channel_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"channel_error", "ok"},
            {"channel_error_state", "none"},
            {"channel_action_state", u1_channel_state_[static_cast<size_t>(i)]},
            {"disable_auto", false},
            {"filament_detected", u1_filament_detected_[static_cast<size_t>(i)]},
            {"module_exist", true},
        };
    }
    dispatch_status_update({{"filament_feed " + side, std::move(channels)}});
}

bool MoonrakerClientMock::apply_u1_feeding_gcode(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    // AUTO_FEEDING and AUTO_FEEDING_BATCH ACTION=DOING both drive a channel.
    // The BATCH START/END lines name no EXTRUDER, so they land in the "feeder
    // command, nothing to walk" branch and change no channel state.
    if (cmd.rfind("AUTO_FEEDING", 0) != 0) {
        return false;
    }
    const size_t e = gcode.find("EXTRUDER=");
    if (e == std::string::npos) {
        return true;
    }
    int ext = -1;
    try {
        ext = std::stoi(gcode.substr(e + 9));
    } catch (...) {
        return true;
    }
    if (ext < 0 || ext >= U1_CHANNELS) {
        return true;
    }
    // "UNLOAD=1" contains "LOAD=1" as a substring; the direction must be read
    // unload-first.
    const bool unload = gcode.find("UNLOAD=1") != std::string::npos;

    int fail_slot = -1;
    if (const char* fail_env = std::getenv("HELIX_MOCK_BATCH_FAIL_SLOT")) {
        try {
            fail_slot = std::stoi(fail_env);
        } catch (...) {
            fail_slot = -1;
        }
    }

    const size_t ext_idx = static_cast<size_t>(ext);
    // The state sequences the firmware's own filament_feed walks: preload then
    // load for a feed, heat then retract for an unload.
    u1_channel_state_[ext_idx] = unload ? "unload_heat_finish" : "preload_finish";
    if (!unload) {
        u1_filament_detected_[ext_idx] = true;
    }
    publish_u1_channel_frame(ext);

    const char* terminal = unload ? "unload_finish" : "load_finish";
    if (ext == fail_slot) {
        terminal = unload ? "unload_fail" : "load_fail";
    }
    u1_channel_state_[ext_idx] = terminal;
    if (unload && ext != fail_slot) {
        u1_filament_detected_[ext_idx] = false;
    }
    publish_u1_channel_frame(ext);
    spdlog::info("[MoonrakerClientMock] U1 feeder: extruder {} {} -> {}", ext,
                 unload ? "unload" : "load", terminal);
    return true;
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
    case PrinterType::CREALITY_K1_MAX:
        // Creality K1-family configuration
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
        // Fans mirror assets/config/presets/ad5m.json hardware/expected. No chamber
        // light: a chamber_l* LED is what tells the 5M Pro apart.
        discovery_.fans() = {"fan", "heater_fan hotend_fan", "controller_fan stepper_driver_fan"};
        discovery_.leds() = {};
        break;

    case PrinterType::FLASHFORGE_CREATOR5_ZMOD: // Z-Mod: same machine, same hardware
    case PrinterType::FLASHFORGE_CREATOR5:
        // FlashForge Creator 5 Pro: 4-head tool changer, enclosed, heated chamber.
        // Object names mirror assets/config/presets/creator5_pro.json so the mock and
        // the shipped preset describe the same machine.
        discovery_.heaters() = {"heater_bed", "extruder",  "extruder1",
                                "extruder2",  "extruder3", "heater_generic chamber_heater"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder",   // Hotend thermistors, one per head
                                "extruder1",
                                "extruder2",
                                "extruder3",
                                "heater_generic chamber_heater",
                                "temperature_sensor mcu_temp"};
        discovery_.fans() = {"heater_fan heat_fan", "fan_generic fanM106",
                             "fan_generic chamber_fan", "fan_generic chamber_loop_fan"};
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

    case PrinterType::DELTA:
        // Generic linear delta: no gantry leveling, bare heaters and fans
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {
            "heater_bed", // Bed thermistor (Klipper naming: bare heater name)
            "extruder"    // Hotend thermistor (Klipper naming: bare heater name)
        };
        discovery_.fans() = {"heater_fan hotend_fan", "fan"};
        discovery_.leds() = {};
        break;

    case PrinterType::ELEGOO_CC1:
        // Elegoo Centauri Carbon on COSMOS. Names mirror
        // tests/fixtures/printers/elegoo_centauri_carbon.json and
        // assets/config/presets/cc1.json hardware/expected.
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber", "temperature_sensor mcu_toolhead",
                                "temperature_sensor mcu_bed"};
        discovery_.fans() = {"heater_fan extruder", "fan", "fan_generic aux_fan",
                             "fan_generic case_fan", "temperature_fan mainboard"};
        discovery_.leds() = {"led case", "led hotend"};
        break;

    case PrinterType::FLASHFORGE_AD5X:
        // FlashForge Adventurer 5X. Fans mirror assets/config/presets/ad5x.json
        // hardware/expected.
        discovery_.heaters() = {"heater_bed", "extruder"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber"};
        discovery_.fans() = {"fan_generic fanM106", "heater_fan heat_fan",
                             "fan_generic chamber_fan", "fan_generic pcb_fan"};
        discovery_.leds() = {};
        break;

    case PrinterType::CREALITY_K2_PLUS:
        // Creality K2 Plus. Names mirror tests/fixtures/printers/creality_k2_plus.json
        // and assets/config/presets/k2.json hardware/expected.
        discovery_.heaters() = {"heater_bed", "extruder", "heater_generic chamber_heater"};
        discovery_.sensors() = {"heater_bed", // Bed thermistor (Klipper naming: bare heater name)
                                "extruder", // Hotend thermistor (Klipper naming: bare heater name)
                                "temperature_sensor chamber_temp"};
        discovery_.fans() = {"fan", "heater_fan chamber_fan", "output_pin fan0", "output_pin fan1",
                             "output_pin fan2"};
        discovery_.leds() = {"output_pin LED"};
        break;

    case PrinterType::SNAPMAKER_U1:
        // Snapmaker U1: four independent extruders. Names mirror
        // tests/fixtures/printers/snapmaker_u1.json and assets/config/presets/snapmaker_u1.json.
        discovery_.heaters() = {"heater_bed", "extruder", "extruder1", "extruder2", "extruder3"};
        discovery_.sensors() = {"heater_bed", "extruder",  "extruder1",
                                "extruder2",  "extruder3", "temperature_sensor cavity"};
        discovery_.fans() = {"fan",
                             "heater_fan power_fan",
                             "fan_generic cavity_fan",
                             "heater_fan e0_nozzle_fan",
                             "fan_generic e1_fan",
                             "heater_fan e1_nozzle_fan",
                             "fan_generic e2_fan",
                             "heater_fan e2_nozzle_fan",
                             "fan_generic e3_fan",
                             "heater_fan e3_nozzle_fan"};
        discovery_.leds() = {"led cavity_led"};
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
    const auto bounds = mock_internal::mesh_bounds(printer_type_);
    auto generate_mesh = [bounds](const std::string& name, float amplitude, float x_tilt,
                                  float y_tilt) -> BedMeshProfile {
        BedMeshProfile mesh;
        mesh.name = name;
        mesh.mesh_min[0] = static_cast<float>(bounds.x_min);
        mesh.mesh_min[1] = static_cast<float>(bounds.y_min);
        mesh.mesh_max[0] = static_cast<float>(bounds.x_max);
        mesh.mesh_max[1] = static_cast<float>(bounds.y_max);
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

    // Probed area of the persona's bed
    const auto bounds = mock_internal::mesh_bounds(printer_type_);
    active_bed_mesh_.mesh_min[0] = static_cast<float>(bounds.x_min);
    active_bed_mesh_.mesh_min[1] = static_cast<float>(bounds.y_min);
    active_bed_mesh_.mesh_max[0] = static_cast<float>(bounds.x_max);
    active_bed_mesh_.mesh_max[1] = static_cast<float>(bounds.y_max);
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

json MoonrakerClientMock::bed_mesh_status() const {
    const auto matrix_json = [](const std::vector<std::vector<float>>& matrix) {
        json rows = json::array();
        for (const auto& row : matrix) {
            json row_json = json::array();
            for (float val : row) {
                row_json.push_back(val);
            }
            rows.push_back(row_json);
        }
        return rows;
    };

    json profiles_json = json::object();
    for (const auto& [name, profile] : stored_bed_mesh_profiles_) {
        profiles_json[name] = {{"points", matrix_json(profile.probed_matrix)},
                               {"mesh_params",
                                {{"min_x", profile.mesh_min[0]},
                                 {"min_y", profile.mesh_min[1]},
                                 {"max_x", profile.mesh_max[0]},
                                 {"max_y", profile.mesh_max[1]},
                                 {"x_count", profile.x_count},
                                 {"y_count", profile.y_count}}}};
    }

    json status = {{"profile_name", active_bed_mesh_.name},
                   {"probed_matrix", matrix_json(active_bed_mesh_.probed_matrix)},
                   {"mesh_min", {active_bed_mesh_.mesh_min[0], active_bed_mesh_.mesh_min[1]}},
                   {"mesh_max", {active_bed_mesh_.mesh_max[0], active_bed_mesh_.mesh_max[1]}},
                   {"profiles", profiles_json},
                   {"mesh_params", {{"algo", active_bed_mesh_.algo}}}};
    return status;
}

void MoonrakerClientMock::calibrate_mock_mesh(const std::string& profile_name) {
    // Regenerate mesh with slight random variation
    active_bed_mesh_.name = profile_name;
    generate_mock_bed_mesh_with_variation();

    if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
        bed_mesh_profiles_.end()) {
        bed_mesh_profiles_.push_back(profile_name);
    }
    stored_bed_mesh_profiles_[profile_name] = active_bed_mesh_;

    spdlog::info("[MoonrakerClientMock] Mesh calibration: generated new mesh for profile '{}'",
                 profile_name);
    dispatch_bed_mesh_update();
}

void MoonrakerClientMock::dispatch_bed_mesh_update() {
    json status = {{"bed_mesh", bed_mesh_status()}};
    dispatch_status_update(status);
}

void MoonrakerClientMock::disconnect() {
    spdlog::info("[MoonrakerClientMock] Simulating disconnection");
    stop_temperature_simulation(false);
    set_connection_state(ConnectionState::DISCONNECTED);
    sim_link_down_ = true;
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

    // Mirror MoonrakerClient::send_jsonrpc, but only between an explicit
    // disconnect() and the next successful connect(): a request on a dropped
    // link is refused and reported to the error callback as CONNECTION_LOST,
    // so code under test sees the same immediate failure production shows.
    // Fixtures that never simulate a link keep getting answers.
    if (sim_link_down_) {
        if (error_cb) {
            error_cb(MoonrakerError::connection_lost(method));
        }
        return INVALID_REQUEST_ID;
    }

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

    std::optional<MoonrakerError> injected_error;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        ++call_counts_[method];
        if (auto f = fail_next_.find(method); f != fail_next_.end()) {
            injected_error = std::move(f->second);
            fail_next_.erase(f);
        } else if (defer_next_.erase(method) > 0) {
            held_requests_[method] = HeldRequest{params, success_cb, error_cb};
            return next_mock_request_id();
        }
    }
    if (injected_error) {
        if (error_cb) {
            error_cb(*injected_error);
        }
        return next_mock_request_id();
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

void MoonrakerClientMock::fail_next(const std::string& method, MoonrakerError err) {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    err.method = method;
    fail_next_[method] = std::move(err);
}

void MoonrakerClientMock::defer_next(const std::string& method) {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    defer_next_.insert(method);
}

void MoonrakerClientMock::fire_deferred(const std::string& method) {
    std::optional<HeldRequest> held;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        if (auto h = held_requests_.find(method); h != held_requests_.end()) {
            held = std::move(h->second);
            held_requests_.erase(h);
        }
    }
    if (!held) {
        return;
    }
    if (auto it = method_handlers_.find(method); it != method_handlers_.end()) {
        it->second(this, held->params, held->success_cb, held->error_cb);
    }
}

void MoonrakerClientMock::fire_deferred_error(const std::string& method,
                                              const MoonrakerError& err) {
    std::optional<HeldRequest> held;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        if (auto h = held_requests_.find(method); h != held_requests_.end()) {
            held = std::move(h->second);
            held_requests_.erase(h);
        }
    }
    if (held && held->error_cb) {
        held->error_cb(err);
    }
}

int MoonrakerClientMock::call_count(const std::string& method) const {
    std::lock_guard<std::mutex> lock(fault_mutex_);
    auto it = call_counts_.find(method);
    return it == call_counts_.end() ? 0 : it->second;
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

    // A staged rewrite names the original it was made from; that is the test
    // file holding the metadata.
    const std::string lookup_filename = helix::gcode::resolve_gcode_filename(filename);

    if (lookup_filename.find(RuntimeConfig::TEST_GCODE_DIR) == 0) {
        // Already a full path, use as-is
        full_path = lookup_filename;
    } else {
        // Bare filename, prepend test directory
        full_path = helix::mock::gcode_disk_path(lookup_filename);
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
            objects_array.push_back(mock_object_entry(
                names.back(), i, total, helix::mock::descriptor(printer_type_).axis_max));
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
    double pause_sim =
        std::chrono::duration<double>(sim_speed().accelerate_progress(pause_real)).count();
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

    // Klippy enters SHUTDOWN, reported both ways Moonraker does: a webhooks
    // frame carrying the state with Klipper's reason, and notify_klippy_shutdown
    // (which lands on the global PrinterState, so must defer to main thread).
    klippy_state_.store(KlippyState::SHUTDOWN);
    dispatch_status_update(
        {{"webhooks", {{"state", "shutdown"}, {"state_message", kM112ShutdownMessage}}}});
    helix::ui::queue_update("MoonrakerClientMock::estop_shutdown", []() {
        get_printer_state().set_klippy_state_sync(helix::KlippyState::SHUTDOWN);
    });
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

    const std::string homed = get_homed_axes();

    // Get print state with thread safety
    std::string print_state_str = get_print_state_string();
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        filename = print_filename_;
    }
    double progress = print_progress_.load();

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
          {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
          {"axis_maximum",
           {helix::mock::descriptor(printer_type_).axis_max.x,
            helix::mock::descriptor(printer_type_).axis_max.y,
            helix::mock::descriptor(printer_type_).axis_max.z, 0.0}},
          {"kinematics", kinematics()}}},
        {"gcode_move",
         {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
          {"speed_factor", speed / 100.0},
          {"extrude_factor", flow / 100.0},
          {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
        {"fan", {{"speed", fan / 255.0}}},
        {"webhooks",
         {{"state", klippy_str},
          {"state_message",
           klippy == KlippyState::SHUTDOWN ? kM112ShutdownMessage : "Printer is ready"}}},
        {"print_stats", {{"state", print_state_str}, {"filename", filename}}},
        {"virtual_sdcard", {{"progress", progress}}},
        {"bed_mesh", bed_mesh_status()}};

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

    // The chamber and width sensors carry status only where the persona reports them.
    using helix::mock::inherits_default;
    namespace default_object = helix::mock::default_object;
    if (inherits_default(printer_type_, default_object::CHAMBER_SENSOR) || has_chamber_sensor()) {
        initial_status["temperature_sensor chamber"] = {{"temperature", 42.3}};
    }
    // The chamber heater: the persona's own, else the inherited default one
    // (which rides with the chamber sensor).
    {
        const auto heater_key = chamber_heater_status_key();
        if (!heater_key.empty() ||
            inherits_default(printer_type_, default_object::CHAMBER_SENSOR)) {
            initial_status[heater_key.empty() ? "heater_generic chamber" : heater_key] = {
                {"temperature", 42.3},
                {"target", chamber_target_.load()},
                {"power", mock_heater_duty(42.3, chamber_target_.load())}};
        }
    }
    if (inherits_default(printer_type_, default_object::WIDTH_SENSOR)) {
        // Hall-effect filament diameter measurement
        initial_status["hall_filament_width_sensor"] = {
            {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
    }
    if (printer_type_ == PrinterType::CREALITY_K2_PLUS) {
        append_k2_status(initial_status);
        // The box starts with nothing loaded, so the toolhead switch sees none.
        initial_status["filament_switch_sensor filament_sensor"] = {
            {"filament_detected", cfs_toolhead_filament_detected()}, {"enabled", true}};
    }

    // Probe objects (the same ones populate_capabilities() lists)
    // (assigned, not merge_patch'd: a patch drops the null fields they carry).
    initial_status.update(helix::sim::mock_probe_status(printer_type_));

    // Chamber backend diagnostics + filter pin (e.g. dragonbreath trio via
    // HELIX_MOCK_OBJECTS). Tail of the builder; keys are distinct from every
    // merge above (the LED loop only walks profile LEDs in discovery_.leds(),
    // which never contains env-materialized pins).
    append_chamber_backend_status(initial_status, 0.0);
    append_aux_heater_status(initial_status, 0.0);

    spdlog::debug("[MoonrakerClientMock] Dispatching initial state: extruder={}/{}°C, bed={}/{}°C, "
                  "homed_axes='{}', leds={}, filament_sensors={}",
                  ext_temp, ext_target, bed_temp_val, bed_target_val, homed, led_json.size(),
                  discovery_.filament_sensors().size());

    // The discovery subscription is built before configfile is read here, so
    // the leveling-skip objects it never asked for are announced with the rest.
    initial_status.update(skip_wrapper_status());

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

        // Add width sensor data (Hall-effect filament diameter measurement) on
        // every frame of a persona that has one, so WidthSensorManager receives updates
        if (helix::mock::inherits_default(printer_type_,
                                          helix::mock::default_object::WIDTH_SENSOR)) {
            status_obj["hall_filament_width_sensor"] = {
                {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
        }

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
    if (simulates_chamber_temp()) {
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

void MoonrakerClientMock::dispatch_status_update(const json& status, bool from_cached_snapshot,
                                                 bool whole_objects) {
    MoonrakerClient::dispatch_status_update(status, from_cached_snapshot, whole_objects);

    dispatch_method_callback(
        "notify_status_update",
        helix::make_status_notification(status, from_cached_snapshot, whole_objects));
}

bool MoonrakerClientMock::led_effect_enabled(const std::string& object_name) const {
    std::lock_guard<std::mutex> lock(led_mutex_);
    return enabled_led_effects_.count(object_name) > 0;
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

void MoonrakerClientMock::service_openams_late_links(uint32_t tick) {
    // HELIX_MOCK_OPENAMS_LATE_LINKS=1: the spool links of slots 3 and 4 arrive
    // ~10s after start, the way openams_spoolman writes lane_data and announces
    // it once a Spoolman lookup finishes.
    static constexpr uint32_t kLateTick = 20;
    if (tick != kLateTick || !is_mock_openams() || !openams_plugin_units() ||
        !std::getenv("HELIX_MOCK_OPENAMS_LATE_LINKS")) {
        return;
    }
    static const char* const COLORS[] = {"#1F3A93", "#F5F5F5"};
    for (int i = 0; i < 2; ++i) {
        mock_db_set("lane_data", "lane" + std::to_string(i + 4),
                    json{{"lane", std::to_string(i + 3)},
                         {"color", COLORS[i]},
                         {"material", "PETG"},
                         {"spool_id", 40 + i},
                         {"name", "Late spool"}});
    }
    dispatch_method_callback("notify_openams_spoolman_status",
                             json{{"method", "notify_openams_spoolman_status"},
                                  {"params", json::array({json{{"state", "ready"}}})}});
}

void MoonrakerClientMock::temperature_simulation_loop() {
    spdlog::debug("[MoonrakerClientMock] temperature_simulation_loop ENTERED");
    const double base_dt = SIMULATION_INTERVAL_MS / 1000.0; // Base time step (0.5s)

    while (simulation_running_.load()) {
        // Parked by set_simulation_paused(): no physics, no tick, no push, so a
        // value a caller wrote into a subject by hand stays written. The wait is
        // bounded like the one at the bottom of the loop, so a notify that races
        // the predicate costs one interval rather than wedging shutdown.
        if (simulation_paused_.load()) {
            std::unique_lock<std::mutex> lock(sim_mutex_);
            sim_cv_.wait_for(lock, std::chrono::milliseconds(SIMULATION_INTERVAL_MS), [this] {
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
        constexpr double IDLE_VARIATION_AMPLITUDE = 1.5; // +/- 1.5°C variation
        constexpr double EXTRUDER_WAVE_PERIOD = 45.0;    // 45 second period for extruder
        constexpr double BED_WAVE_PERIOD = 60.0;         // 60 second period for bed
        constexpr double PHASE_OFFSET = 1.57;            // Phase offset between heaters (pi/2)

        double sim_time = tick * base_dt; // Simulated elapsed time in seconds
        // The dryer clock runs at the simulation speed, so --sim-speed plays a
        // multi-hour cycle through in minutes.
        mock_sim_time_.store(mock_sim_time_.load() + sim_speed().accelerate_progress(base_dt));

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
        if (simulates_chamber_temp()) {
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

        // A console line per layer while printing, so the console has live
        // output to show in --test.
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
              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
              {"axis_maximum",
               {helix::mock::descriptor(printer_type_).axis_max.x,
                helix::mock::descriptor(printer_type_).axis_max.y,
                helix::mock::descriptor(printer_type_).axis_max.z, 0.0}},
              {"kinematics", kinematics()}}},
            {"gcode_move",
             {{"gcode_position", {x, y, z, 0.0}}, // Commanded position (same as toolhead in mock)
              {"speed", feed_mm_s},
              {"speed_factor", speed / 100.0},
              {"extrude_factor", flow / 100.0},
              {"homing_origin", {0.0, 0.0, z_offset, 0.0}}}},
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

        // Add width sensor data (Hall-effect filament diameter measurement) on
        // every frame of a persona that has one, so WidthSensorManager receives updates
        if (helix::mock::inherits_default(printer_type_,
                                          helix::mock::default_object::WIDTH_SENSOR)) {
            status_obj["hall_filament_width_sensor"] = {
                {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
        }

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

        // Standalone IFS module mock: the module's pushed objects. The lane
        // sensors and toolhead switch ride the same notification so the
        // backend's head-presence path sees what the board would report.
        if (is_mock_ifs_module()) {
            status_obj["ifs"] = ifs_module_status_json();
            status_obj["ifs_materials"] = ifs_module_materials_json();
            status_obj["save_variables"] = ifs_module_vars_json();
            const int loaded = ifs_module_loaded_.load();
            const uint8_t presence = ifs_module_presence_.load();
            for (int i = 0; i < 4; ++i) {
                status_obj["filament_switch_sensor lane" + std::to_string(i + 1)] = {
                    {"filament_detected", (presence & (1u << i)) != 0}};
            }
            status_obj["filament_switch_sensor toolhead"] = {{"filament_detected", loaded > 0}};
        }

        // CFS mock: the stock `box` frame AmsBackendCfs parses (bay states,
        // map, and the vendor/color/material arrays).
        if (is_mock_cfs()) {
            status_obj["box"] = cfs_box_status_json();
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

void MoonrakerClientMock::dispatch_toolchanger_tool(int tool) {
    // klipper-toolchanger publishes `toolchanger.tool` as the gcode name itself,
    // so this is the wire value the mock reproduces, not a label the UI renders.
    json update = {
        {"toolchanger",
         {{"tool_number", tool},
          {"tool", "T" + std::to_string(tool)}}}}; // DISPLAY_NUMBERING_OK: wire value, not a label
    dispatch_status_update(update);
    // The mock AMS backend does not read toolchanger.tool_number (a production
    // backend does); it follows the simulator through this hook instead, the
    // same one a print's tool changes use. Without it ToolState's active tool
    // never moves under --test and no row ever reads Measuring.
    notify_active_gcode_tool_observers(tool);
}

void MoonrakerClientMock::apply_calibrated_tool_offset(int tool, double x, double y, double z) {
    const double values[] = {x, y, z};
    for (helix::Axis axis : helix::kAllAxes) {
        const double value = values[helix::axis_index(axis)];
        {
            std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
            tool_offsets_[tool][axis] = value;
        }
        dispatch_tool_update(tool, axis);
        char text[32];
        std::snprintf(text, sizeof(text), "%.6g", value);
        stage_config_change("tool T" + std::to_string(tool), tool_offset_param(axis), text);
    }
    spdlog::info("[MoonrakerClientMock] T{} offsets calibrated: x={:.4f} y={:.4f} z={:.4f} "
                 "(staged, awaiting SAVE_CONFIG)",
                 tool, x, y, z);
}

bool MoonrakerClientMock::simulate_tool_offset_calibration(
    const std::string& script, std::function<void(const nlohmann::json&)> success_cb,
    std::function<void(const MoonrakerError&)> error_cb) {
    // Whole-line match: the macro takes no parameters worth modelling.
    std::string cmd = script;
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
        cmd.pop_back();
    }
    if (cmd.rfind("CALIBRATE_TOOL_OFFSETS", 0) != 0) {
        return false;
    }

    const int tool_count = static_cast<int>(discovery_.hardware().tool_names().size());
    if (tool_count < 1) {
        if (error_cb) {
            MoonrakerError err;
            err.message = "Unknown command:\"CALIBRATE_TOOL_OFFSETS\"";
            error_cb(err);
        }
        return true;
    }

    int fail_tool = -1;
    if (const char* env = std::getenv("HELIX_MOCK_TOOL_CAL_FAIL")) {
        try {
            fail_tool = std::stoi(env);
        } catch (const std::exception&) {
            fail_tool = -1;
        }
    }
    spdlog::info(
        "[MoonrakerClientMock] CALIBRATE_TOOL_OFFSETS: {} tools{}", tool_count,
        fail_tool >= 0 ? fmt::format(", T{} will fail (HELIX_MOCK_TOOL_CAL_FAIL)", fail_tool) : "");

    // Two ticks per tool - select, then measure - and a final park on T0. A
    // failing tool's measure tick ends the run instead.
    const auto select = [this](int tool) {
        dispatch_gcode_response(fmt::format("Selected tool {} (T{})", tool, tool));
        dispatch_toolchanger_tool(tool);
    };
    // Measuring. The probe prints a contact per sample; the numbers only
    // need to look like a nozzle near the sensor.
    const double sensor_x = 229.0, sensor_y = 2.5, sensor_z = 1.25;
    const auto probe = [this, sensor_x, sensor_y, sensor_z](int tool) {
        // Distinct from the per-tool seed on EVERY axis (see tool_offset()):
        // a measured value equal to the seed leaves that axis clean after a
        // run, so nothing stages it and the save path's handling of it is
        // never exercised.
        const double dx = 0.12 * tool, dy = -0.07 * tool, dz = -0.03 * tool;
        for (int sample = 0; sample < 3; ++sample) {
            dispatch_gcode_response(fmt::format("Probe made contact at {:.6f},{:.6f},{:.6f}",
                                                sensor_x + dx + 0.001 * sample, sensor_y + dy,
                                                sensor_z + dz));
        }
    };
    const auto measure = [this, probe, sensor_x, sensor_y, sensor_z](int tool) {
        probe(tool);
        if (tool == 0) {
            dispatch_gcode_response(fmt::format("Sensor location at {:.6f},{:.6f},{:.6f}", sensor_x,
                                                sensor_y, sensor_z));
            return;
        }
        const double dx = 0.12 * tool, dy = -0.07 * tool, dz = -0.03 * tool;
        dispatch_gcode_response(fmt::format("Tool offset is {:.6f},{:.6f},{:.6f}", dx, dy, dz));
        apply_calibrated_tool_offset(tool, dx, dy, dz);
    };

    const bool fails = fail_tool >= 0 && fail_tool < tool_count;
    const int last_tool = fails ? fail_tool : tool_count - 1;
    std::vector<std::function<void()>> steps;
    for (int tool = 0; tool <= last_tool; ++tool) {
        steps.emplace_back([select, tool] { select(tool); });
        if (tool != fail_tool) {
            steps.emplace_back([measure, tool] { measure(tool); });
        }
    }

    std::function<void()> on_done;
    if (fails) {
        on_done = [this, probe, fail_tool, error_cb = std::move(error_cb)] {
            probe(fail_tool);
            const std::string error = "Probe samples exceed samples_tolerance";
            dispatch_gcode_response("!! " + error);
            if (error_cb) {
                MoonrakerError err;
                err.message = error;
                error_cb(err);
            }
        };
    } else {
        on_done = [this, success_cb = std::move(success_cb)] {
            // Park on the reference tool, as the macro's last SELECT_TOOL does.
            dispatch_gcode_response("Selected tool 0 (T0)");
            dispatch_toolchanger_tool(0);
            if (success_cb) {
                success_cb(json{{"result", "ok"}});
            }
        };
    }
    play_console_steps(std::move(steps), 600, std::move(on_done));
    return true;
}

void MoonrakerClientMock::dispatch_tool_update(int tool, helix::Axis axis) {
    double value = 0.0;
    {
        std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
        auto tool_it = tool_offsets_.find(tool);
        if (tool_it == tool_offsets_.end()) {
            return;
        }
        auto axis_it = tool_it->second.find(axis);
        if (axis_it == tool_it->second.end()) {
            return;
        }
        value = axis_it->second;
    }
    // Only the field that changed, matching Moonraker: it republishes just the
    // deltas, and code that assumes a full object here is code that would break
    // against a real printer.
    json update = {{"tool T" + std::to_string(tool), {{tool_offset_param(axis), value}}}};
    dispatch_status_update(update);
}

const char* MoonrakerClientMock::tool_offset_param(helix::Axis axis) {
    static constexpr const char* names[] = {"gcode_x_offset", "gcode_y_offset", "gcode_z_offset"};
    return names[helix::axis_index(axis)];
}

std::optional<helix::Axis> MoonrakerClientMock::tool_offset_axis(const std::string& param) {
    for (helix::Axis axis : helix::kAllAxes) {
        if (param == tool_offset_param(axis)) {
            return axis;
        }
    }
    return std::nullopt;
}

double MoonrakerClientMock::tool_offset_seed(int tool, helix::Axis axis) {
    // Distinct per-tool AND per-axis. All-zero would make "every tool shows the
    // same number" — the characteristic per-tool display bug — look correct,
    // and equal X/Y/Z would hide an axis mix-up the same way. T0 is the
    // reference tool and sits at zero on every axis.
    static constexpr double seed_per_tool[] = {0.100, -0.050, -0.025};
    return seed_per_tool[helix::axis_index(axis)] * tool;
}

double MoonrakerClientMock::tool_offset(int tool, helix::Axis axis) const {
    std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
    auto tool_it = tool_offsets_.find(tool);
    if (tool_it != tool_offsets_.end()) {
        auto axis_it = tool_it->second.find(axis);
        if (axis_it != tool_it->second.end()) {
            return axis_it->second;
        }
    }
    return tool_offset_seed(tool, axis);
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
        for (const auto& [option, value] : options) {
            auto axis = tool_offset_axis(option);
            if (!axis) {
                continue;
            }
            try {
                int tool = std::stoi(section.substr(6));
                std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
                tool_offsets_saved_[tool][*axis] = std::stod(value);
            } catch (...) {
            }
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

bool MoonrakerClientMock::simulate_pa_calibration(
    const std::string& script, std::function<void(const nlohmann::json&)> success_cb,
    std::function<void(const MoonrakerError&)> error_cb) {
    if (script.rfind("FLOW_CALIBRATE", 0) != 0) {
        return false;
    }
    record_gcode_script(script);

    const bool should_fail = helix::env_flag("HELIX_MOCK_PA_FAIL");

    // Roughly what the real thing costs once the nozzle is already hot: a
    // handful of purge-and-measure cycles, not an instant answer.
    constexpr int CANDIDATES = 5;
    constexpr int STEP_MS = 1200;

    std::lock_guard<std::mutex> lock(pa_cal_mutex_);
    pending_pa_lines_.clear();
    auto due = std::chrono::steady_clock::now();

    if (should_fail) {
        due += std::chrono::milliseconds(STEP_MS);
        pending_pa_lines_.push_back({due, "!! [flow_calibrate] not edit filament info!", true,
                                     std::move(success_cb), std::move(error_cb)});
        spdlog::info("[MoonrakerClientMock] FLOW_CALIBRATE: simulating refusal"
                     " (HELIX_MOCK_PA_FAIL)");
        return true;
    }

    // Candidate probes, in the U1 flow calibrator's own format: each measured
    // K, then the flow mismatch it read there. Shaped like a real root-find
    // converging on 0.0412.
    static constexpr double CANDIDATE_K[CANDIDATES] = {0.0200, 0.0600, 0.0400, 0.0420, 0.0412};
    static constexpr double CANDIDATE_AREA[CANDIDATES] = {0.0181, -0.0142, 0.0011, -0.0004,
                                                          0.00002};
    for (int i = 0; i < CANDIDATES; ++i) {
        due += std::chrono::milliseconds(STEP_MS);
        pending_pa_lines_.push_back(
            {due, fmt::format("// measure k: {:.5f}", CANDIDATE_K[i]), false, nullptr, nullptr});
        pending_pa_lines_.push_back({due, fmt::format("// measure area: {:.5f}", CANDIDATE_AREA[i]),
                                     false, nullptr, nullptr});
    }

    // The result line the firmware prints as it applies the value.
    due += std::chrono::milliseconds(STEP_MS);
    pending_pa_lines_.push_back(
        {due, "// Got pressure advance: 0.0412", true, std::move(success_cb), std::move(error_cb)});

    spdlog::info("[MoonrakerClientMock] FLOW_CALIBRATE: simulating {} candidates (~{}s)",
                 CANDIDATES, ((CANDIDATES + 1) * STEP_MS) / 1000);
    return true;
}

void MoonrakerClientMock::service_pending_pa_lines() {
    std::vector<PendingPaLine> due;
    {
        std::lock_guard<std::mutex> lock(pa_cal_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_pa_lines_.begin(); it != pending_pa_lines_.end();) {
            if (it->due <= now) {
                due.push_back(std::move(*it));
                it = pending_pa_lines_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& item : due) {
        dispatch_gcode_response(item.line);
        if (!item.is_final) {
            continue;
        }
        // A refusal answers the RPC as an error, the way Klipper does; a
        // successful run answers plainly and lets the console line carry the
        // result, which is the contract the collector relies on.
        if (item.line.rfind("!! ", 0) == 0) {
            if (item.error_cb) {
                MoonrakerError err;
                err.type = MoonrakerErrorType::JSON_RPC_ERROR;
                err.message = item.line.substr(3);
                err.method = "printer.gcode.script";
                item.error_cb(err);
            }
        } else if (item.success_cb) {
            item.success_cb(nlohmann::json{{"result", "ok"}});
        }
    }
}
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

/**
 * @brief Write a mock TEST_RESONANCES OUTPUT=resonances CSV for one belt path
 *
 * Bins at Klipper's raw-PSD resolution (3200 Hz / 4096 samples), with a
 * Lorentzian at the path's simulated peak over a noise floor plus the
 * ~42 Hz peak every belt rig shows.
 */
void write_mock_belt_csv(const std::string& path, char path_letter, float peak_hz, double max_freq,
                         BeltMockFailure mode) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        spdlog::warn("[MoonrakerClientMock] Failed to write mock belt CSV to {}", path);
        return;
    }

    if (mode == BeltMockFailure::MULTICHIP) {
        std::fprintf(f, "freq,adxl345,adxl345_hotend\n");
    } else {
        std::fprintf(f, "freq,psd_x,psd_y,psd_z,psd_xyz");
        if (mode == BeltMockFailure::KALICO) {
            std::fprintf(f, ",accel_per_hz");
        }
        std::fprintf(f, "\n");
    }

    std::mt19937 rng(7 + static_cast<unsigned>(path_letter));
    std::uniform_real_distribution<float> noise_dist(0.9f, 1.1f);

    constexpr float PEAK_HEIGHT = 3e4f;
    constexpr float SECONDARY_HEIGHT = 0.22f * PEAK_HEIGHT;
    constexpr double BIN_STEP = 3200.0 / 4096.0;
    // HWHM of 10 Hz (Q ~ 5 at 100 Hz), the width a real belt rig shows. A
    // spike much narrower than that tanks the curve-similarity leg of
    // compare_belt_paths() for peak pairs a real printer scores as close.
    constexpr float MAIN_HALF_WIDTH_SQ = 100.0f;
    for (double freq = 5.0; freq <= max_freq + 1e-9; freq += BIN_STEP) {
        const float df = static_cast<float>(freq) - peak_hz;
        const float main = PEAK_HEIGHT / (1.0f + (df * df) / MAIN_HALF_WIDTH_SQ);
        const float ds = static_cast<float>(freq) - 42.0f;
        const float secondary = SECONDARY_HEIGHT / (1.0f + (ds * ds) / 20.25f);
        const float psd_xyz = 150.0f * noise_dist(rng) + main + secondary;

        if (mode == BeltMockFailure::MULTICHIP) {
            std::fprintf(f, "%.1f,%.3e,%.3e\n", freq, psd_xyz, psd_xyz * 0.97f);
        } else {
            std::fprintf(f, "%.1f,%.3e,%.3e,%.3e,%.3e", freq, psd_xyz * 0.45f, psd_xyz * 0.45f,
                         psd_xyz * 0.10f, psd_xyz);
            if (mode == BeltMockFailure::KALICO) {
                std::fprintf(f, ",%.1f", 60.0);
            }
            std::fprintf(f, "\n");
        }
    }
    std::fclose(f);
    spdlog::info("[MoonrakerClientMock] Wrote mock belt CSV to {}", path);
}

} // anonymous namespace

std::string MoonrakerClientMock::shaper_csv_path(char axis_lower) {
    // getpid(), not a random suffix: the path has to be stable for the whole
    // process so a fixture's std::remove() between cases still finds the file
    // its own mock wrote, while staying disjoint from every concurrent shard.
    return "/tmp/calibration_data_" + std::string(1, axis_lower) + "_mock_" +
           std::to_string(static_cast<long>(::getpid())) + ".csv";
}

void MoonrakerClientMock::remove_shaper_csvs() {
    std::remove(shaper_csv_path('x').c_str());
    std::remove(shaper_csv_path('y').c_str());
}

float MoonrakerClientMock::belt_peak_hz(char path) const {
    return belt_peaks_hz_[(path == 'A' || path == 'a') ? 0 : 1];
}

void MoonrakerClientMock::set_belt_peaks_hz(float a_hz, float b_hz) {
    belt_peaks_hz_[0] = a_hz;
    belt_peaks_hz_[1] = b_hz;
}

std::string MoonrakerClientMock::belt_csv_path(const std::string& axis_name,
                                               const std::string& name) {
    // PID-scoped for the same reason as shaper_csv_path(): sharded test
    // processes sharing /tmp must not read or delete each other's fixtures.
    return "/tmp/resonances_" + axis_name + "_" + name + "_mock_" +
           std::to_string(static_cast<long>(::getpid())) + ".csv";
}

void MoonrakerClientMock::remove_belt_csvs() {
    // The axis/name parts come from the G-code, so match the fixed prefix and
    // this process' PID suffix rather than enumerating paths.
    const std::string prefix = "resonances_";
    const std::string suffix = "_mock_" + std::to_string(static_cast<long>(::getpid())) + ".csv";
    if (DIR* dir = opendir("/tmp")) {
        while (struct dirent* entry = readdir(dir)) {
            const std::string file(entry->d_name);
            if (file.size() > prefix.size() + suffix.size() &&
                file.compare(0, prefix.size(), prefix) == 0 &&
                file.compare(file.size() - suffix.size(), suffix.size(), suffix) == 0) {
                std::remove(("/tmp/" + file).c_str());
            }
        }
        closedir(dir);
    }
}

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

void MoonrakerClientMock::play_console_lines(std::vector<std::string> lines, uint32_t interval_ms,
                                             std::function<void()> on_done) {
    std::vector<std::function<void()>> steps;
    steps.reserve(lines.size());
    for (auto& line : lines) {
        steps.emplace_back([this, line = std::move(line)] { dispatch_gcode_response(line); });
    }
    play_console_steps(std::move(steps), interval_ms, std::move(on_done));
}

void MoonrakerClientMock::play_console_steps(std::vector<std::function<void()>> steps,
                                             uint32_t interval_ms, std::function<void()> on_done) {
    struct PlaybackState {
        MoonrakerClientMock* mock;
        std::vector<std::function<void()>> steps;
        std::function<void()> on_done;
        size_t index;
    };
    auto* sim = new PlaybackState{this, std::move(steps), std::move(on_done), 0};
    const auto total_ticks = static_cast<int32_t>(sim->steps.size()) + 1; // + on_done

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            auto* s = static_cast<PlaybackState*>(lv_timer_get_user_data(t));

            if (s->index < s->steps.size()) {
                s->steps[s->index++]();
                return;
            }

            s->on_done();
            auto& timers = s->mock->calibration_timers_;
            timers.erase(std::remove_if(timers.begin(), timers.end(),
                                        [t](const CalibrationTimer& ct) { return ct.timer == t; }),
                         timers.end());
            delete s;
            lv_timer_delete(t);
        },
        interval_ms, sim);

    lv_timer_set_repeat_count(timer, total_ticks);
    calibration_timers_.push_back({timer, [sim] { delete sim; }});
}

void MoonrakerClientMock::dispatch_shaper_calibrate_response(char axis) {
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
    snprintf(buf, sizeof(buf), "Shaper calibration data written to %s file",
             shaper_csv_path(axis_lower).c_str());
    const std::string csv_line(buf);

    play_console_lines(std::move(lines), 100, [this, axis_lower, csv_line] {
        // Write actual CSV file so frequency response chart has data.
        // When shaper_csv_writable_ is false, simulate Klipper's /tmp
        // output being unreadable (e.g. PrivateTmp) by removing any
        // stale file at the path instead of writing it.
        const std::string csv_path = shaper_csv_path(axis_lower);
        if (shaper_csv_writable_) {
            write_mock_shaper_csv(csv_path, axis_lower);
        } else {
            std::remove(csv_path.c_str());
        }
        dispatch_gcode_response(csv_line);
        spdlog::info("[MoonrakerClientMock] Dispatched SHAPER_CALIBRATE response for axis {}",
                     static_cast<char>(std::toupper(static_cast<unsigned char>(axis_lower))));
    });

    spdlog::info("[MoonrakerClientMock] Started SHAPER_CALIBRATE timer for axis {} ({:.0f}-{:.0f} "
                 "Hz sweep)",
                 axis, min_freq, max_freq);
}

void MoonrakerClientMock::dispatch_test_resonances_response(const std::string& gcode) {
    // AXIS= names a belt diagonal in Klipper's XY-vector form. Path A is
    // 1,-1 and Path B is 1,1, the Voron motor names Shake&Tune uses.
    // Anything else cannot be swept.
    std::string axis_value;
    if (auto axis_pos = gcode.find("AXIS="); axis_pos != std::string::npos) {
        const size_t start = axis_pos + 5;
        const size_t end = gcode.find_first_of(" \t", start);
        axis_value = gcode.substr(start, end == std::string::npos ? end : end - start);
    }
    const bool is_a = axis_value == "1,-1";
    const bool is_b = axis_value == "1,1";
    if (!is_a && !is_b) {
        dispatch_gcode_response("!! Unsupported axis");
        spdlog::warn("[MoonrakerClientMock] TEST_RESONANCES unsupported axis '{}'", axis_value);
        return;
    }
    const int idx = is_a ? 0 : 1;
    const char path_letter = is_a ? 'A' : 'B';

    std::string name = "adxl345"; // Klipper's NAME= default
    if (auto name_pos = gcode.find("NAME="); name_pos != std::string::npos) {
        const size_t start = name_pos + 5;
        const size_t end = gcode.find_first_of(" \t", start);
        name = gcode.substr(start, end == std::string::npos ? end : end - start);
    }

    // Each re-measurement after the first walks this path's peak toward the
    // other's by at most 4 Hz: tightening a belt moves its resonance, and the
    // clamp keeps the walk from ever overshooting past equal.
    if (belt_measure_count_[idx] > 0) {
        const float step = std::clamp(belt_peaks_hz_[1 - idx] - belt_peaks_hz_[idx], -4.0f, 4.0f);
        belt_peaks_hz_[idx] += step;
    }
    ++belt_measure_count_[idx];

    const bool kalico = belt_failure_ == BeltMockFailure::KALICO;
    const std::string axis_name =
        kalico ? (is_a ? "axis=1.000,-1.000" : "axis=1.000,1.000")
               : (is_a ? "axis=1.000,-1.000,0.000" : "axis=1.000,1.000,0.000");

    // Whole-Hz console lines across the configured [resonance_tester] range.
    std::vector<std::string> lines;
    char buf[256];
    for (double freq = std::ceil(resonance_min_freq_); freq <= resonance_max_freq_ + 1e-9;
         freq += 1.0) {
        snprintf(buf, sizeof(buf), "Testing frequency %.0f Hz", freq);
        lines.emplace_back(buf);
    }
    if (belt_failure_ == BeltMockFailure::STALL) {
        lines.resize(lines.size() / 2);
    } else if (belt_failure_ == BeltMockFailure::ERROR) {
        lines.resize(3);
        lines.emplace_back("!! Invalid adxl345 id (got 0 vs e5).");
    }

    const bool has_terminal =
        belt_failure_ != BeltMockFailure::STALL && belt_failure_ != BeltMockFailure::ERROR;
    const bool write = belt_failure_ == BeltMockFailure::NONE ||
                       belt_failure_ == BeltMockFailure::MULTICHIP || kalico;
    const std::string csv_path = belt_csv_path(axis_name, name);
    std::string final_line;
    if (has_terminal) {
        snprintf(buf, sizeof(buf), "Resonances data written to %s file", csv_path.c_str());
        final_line = buf;
    }

    const uint32_t interval = belt_line_interval_ms_ != 0
                                  ? belt_line_interval_ms_
                                  : static_cast<uint32_t>(std::max(
                                        1, sim_speed().shorten_wait_ms(static_cast<int>(
                                               1000.0 / std::max(1.0, resonance_hz_per_sec_)))));

    play_console_lines(
        std::move(lines), interval,
        [this, final_line, csv_path, path_letter, peak_hz = belt_peaks_hz_[idx],
         max_freq = resonance_max_freq_, write, mode = belt_failure_] {
            if (!final_line.empty()) {
                if (write) {
                    // HELIX_MOCK_BELT_CSV_A/_B replay a real capture for that
                    // path instead of the synthetic curve.
                    const char* replay = std::getenv(path_letter == 'A' ? "HELIX_MOCK_BELT_CSV_A"
                                                                        : "HELIX_MOCK_BELT_CSV_B");
                    std::ifstream src(replay ? replay : "");
                    if (src && mode == BeltMockFailure::NONE) {
                        std::ofstream(csv_path) << src.rdbuf();
                        spdlog::info("[MoonrakerClientMock] Replayed belt CSV {} to {}", replay,
                                     csv_path);
                    } else {
                        write_mock_belt_csv(csv_path, path_letter, peak_hz, max_freq, mode);
                    }
                } else {
                    // No file may survive at the path the terminal line names,
                    // or a caller would read stale data as this run's result.
                    std::remove(csv_path.c_str());
                }
                dispatch_gcode_response(final_line);
            }
            spdlog::info("[MoonrakerClientMock] Dispatched TEST_RESONANCES response for belt "
                         "path {}",
                         path_letter);
        });

    spdlog::info("[MoonrakerClientMock] Started TEST_RESONANCES timer for belt path {} "
                 "({:.0f}-{:.0f} Hz sweep, name '{}')",
                 path_letter, resonance_min_freq_, resonance_max_freq_, name);
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
        if (consume_skip(helix::skip_wrappers::Op::Qgl)) {
            dispatch_gcode_response("HelixScreen: quad gantry level skipped for this print");
        } else {
            dispatch_gcode_response("QUAD_GANTRY_LEVEL");
            dispatch_gcode_response("// Gantry leveling complete");
        }
        simulated_print_start_phase_.store(static_cast<uint8_t>(SimulatedPrintStartPhase::QGL));
        return;
    }

    // Phase 6: Bed mesh (when bed is ~70% heated)
    if (current_phase < static_cast<uint8_t>(SimulatedPrintStartPhase::BED_MESH) &&
        bed_progress > 0.65) {
        if (consume_skip(helix::skip_wrappers::Op::BedMesh)) {
            dispatch_gcode_response("HelixScreen: bed mesh skipped for this print");
        } else {
            dispatch_gcode_response("BED_MESH_CALIBRATE");
            dispatch_gcode_response("// Bed mesh calibration complete");
        }
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

    // Per-tool offsets come back from printer.cfg, so anything SET_TOOL_PARAMETER
    // wrote but SAVE_TOOL_PARAMETER + SAVE_CONFIG never committed is lost here -
    // as on a real printer. Axes with no saved entry fall back to the distinct
    // seed in tool_offset().
    {
        std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
        tool_offsets_ = tool_offsets_saved_;
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

    // Fast-forward the wait, not the simulated delay it stands for
    double effective_delay = sim_speed().shorten_wait_seconds(delay_sec);

    // Cancel and wait for any existing restart thread (under lock to prevent race with destructor)
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        restart_pending_.store(false);
        if (restart_thread_.joinable()) {
            restart_thread_.join();
        }

        // Launch new restart thread (still under lock to prevent race on assignment)
        restart_pending_.store(true);
        restart_thread_ = helix::make_thread([this, effective_delay, is_firmware]() {
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
